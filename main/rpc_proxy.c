#include "rpc_proxy.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bridge_config.h"
#include "bridge_io.h"
#include "bridge_metrics.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define PROXY_WRITER_DONE_BIT (1U << 0U)
#define PROXY_STOP_BIT (1U << 1U)

static const char* TAG = "rpc_proxy";

typedef struct {
    int socket_fd;
    ExpansionTransport* transport;
    EventGroupHandle_t events;
} ProxyWriterContext;

static ptrdiff_t socket_write(void* context, const uint8_t* bytes, size_t size, bool* retryable) {
    const int socket_fd = *(const int*)context;
    const ssize_t sent = send(socket_fd, bytes, size, 0);
    *retryable = sent < 0 && errno == EINTR;
    if(sent > 0) bridge_metrics_add_tcp_tx((size_t)sent);
    return (ptrdiff_t)sent;
}

static esp_err_t send_all(int socket_fd, const uint8_t* bytes, size_t size) {
    return bridge_io_write_all(socket_write, &socket_fd, bytes, size) ? ESP_OK : ESP_FAIL;
}

static void proxy_writer_task(void* argument) {
    ProxyWriterContext* context = argument;
    uint8_t bytes[BRIDGE_TCP_READ_BYTES];
    while(!(xEventGroupGetBits(context->events) & PROXY_STOP_BIT)) {
        size_t received = 0U;
        const esp_err_t error = expansion_transport_receive_rpc(
            context->transport,
            bytes,
            sizeof(bytes),
            &received,
            100U);
        if(error == ESP_OK) {
            if(send_all(context->socket_fd, bytes, received) != ESP_OK) break;
        } else if(error != ESP_ERR_TIMEOUT) {
            break;
        }
    }
    if(!(xEventGroupGetBits(context->events) & PROXY_STOP_BIT)) {
        shutdown(context->socket_fd, SHUT_RDWR);
    }
    xEventGroupSetBits(context->events, PROXY_WRITER_DONE_BIT);
    vTaskDelete(NULL);
}

esp_err_t rpc_proxy_run(
    int socket_fd,
    ExpansionTransport* transport,
    const uint8_t* initial_bytes,
    size_t initial_size) {
    if(socket_fd < 0 || !transport || (!initial_bytes && initial_size > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if(expansion_transport_state(transport) != ExpansionStateRpcReady) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;

    EventGroupHandle_t events = xEventGroupCreate();
    if(!events) {
        expansion_transport_end_rpc(transport);
        return ESP_ERR_NO_MEM;
    }
    ProxyWriterContext writer = {
        .socket_fd = socket_fd,
        .transport = transport,
        .events = events,
    };
    if(xTaskCreate(proxy_writer_task, "rpc_to_tcp", 4096U, &writer, 8U, NULL) != pdPASS) {
        vEventGroupDelete(events);
        expansion_transport_end_rpc(transport);
        return ESP_ERR_NO_MEM;
    }

    if(initial_size > 0U) {
        result = expansion_transport_send_rpc(transport, initial_bytes, initial_size);
    }

    uint8_t bytes[BRIDGE_TCP_READ_BYTES];
    while(result == ESP_OK &&
          !(xEventGroupGetBits(events) & PROXY_WRITER_DONE_BIT)) {
        const ssize_t received = recv(socket_fd, bytes, sizeof(bytes), 0);
        if(received > 0) {
            bridge_metrics_add_tcp_rx((size_t)received);
            result = expansion_transport_send_rpc(transport, bytes, (size_t)received);
        } else if(received == 0) {
            break;
        } else if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            result = ESP_FAIL;
        }
    }

    xEventGroupSetBits(events, PROXY_STOP_BIT);
    shutdown(socket_fd, SHUT_RDWR);
    xEventGroupWaitBits(
        events,
        PROXY_WRITER_DONE_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
    vEventGroupDelete(events);

    const esp_err_t stop_error = expansion_transport_end_rpc(transport);
    if(result == ESP_OK && stop_error != ESP_OK && stop_error != ESP_ERR_INVALID_STATE) {
        result = stop_error;
    }
    ESP_LOGI(TAG, "RPC proxy stopped result=%s", esp_err_to_name(result));
    return result;
}
