#include "bridge_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "bridge_auth.h"
#include "bridge_config.h"
#include "bridge_io.h"
#include "bridge_metrics.h"
#include "bridge_session.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "rpc_proxy.h"
#include "wifi_manager.h"

static const char* TAG = "bridge_server";
static portMUX_TYPE client_lock = portMUX_INITIALIZER_UNLOCKED;
static bool client_active;

typedef struct {
    int socket_fd;
    ExpansionTransport* transport;
    uint8_t pairing_secret[BRIDGE_PAIRING_SECRET_BYTES];
} ClientContext;

typedef struct {
    ExpansionTransport* transport;
    uint8_t pairing_secret[BRIDGE_PAIRING_SECRET_BYTES];
} ServerContext;

static bool claim_client(void) {
    bool claimed = false;
    taskENTER_CRITICAL(&client_lock);
    if(!client_active) {
        client_active = true;
        claimed = true;
    }
    taskEXIT_CRITICAL(&client_lock);
    return claimed;
}

static void release_client(void) {
    taskENTER_CRITICAL(&client_lock);
    client_active = false;
    taskEXIT_CRITICAL(&client_lock);
}

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

static esp_err_t send_response(
    int socket_fd,
    const BridgeMessage* request,
    BridgeStatus status,
    const uint8_t* payload,
    size_t payload_size) {
    if(payload_size > BRIDGE_MANAGEMENT_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    BridgeMessage response = {
        .version = FLIPFORGE_BRIDGE_PROTOCOL_VERSION,
        .command = request ? request->command : BridgeCommandHello,
        .flags = BRIDGE_MANAGEMENT_RESPONSE_FLAG,
        .status = status,
        .request_id = request ? request->request_id : 0U,
        .payload_size = (uint16_t)payload_size,
    };
    if(payload_size > 0U) memcpy(response.payload, payload, payload_size);
    uint8_t encoded[BRIDGE_MANAGEMENT_HEADER_SIZE + BRIDGE_MANAGEMENT_MAX_PAYLOAD];
    size_t encoded_size = 0U;
    if(!bridge_message_encode(&response, encoded, sizeof(encoded), &encoded_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_all(socket_fd, encoded, encoded_size);
}

static void set_socket_timeouts(int socket_fd) {
    const struct timeval timeout = {
        .tv_sec = (time_t)(BRIDGE_TCP_IO_TIMEOUT_MS / 1000U),
        .tv_usec = (suseconds_t)((BRIDGE_TCP_IO_TIMEOUT_MS % 1000U) * 1000U),
    };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const int keepalive = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
}

static esp_err_t handle_authenticated_command(
    ClientContext* context,
    BridgeSession* session,
    const BridgeMessage* request,
    bool* begin_proxy) {
    if(session->state != BridgeSessionAuthenticated) {
        return send_response(context->socket_fd, request, BridgeStatusUnauthorized, NULL, 0U);
    }

    char json[BRIDGE_MANAGEMENT_MAX_PAYLOAD];
    switch(request->command) {
    case BridgeCommandGetBridgeInfo: {
        if(request->payload_size != 0U) {
            return send_response(context->socket_fd, request, BridgeStatusMalformed, NULL, 0U);
        }
        const int length = snprintf(
            json,
            sizeof(json),
            "{\"product\":\"%s\",\"version\":\"%s\",\"protocol\":%u,"
            "\"chip\":\"ESP32-S2\",\"uptime_ms\":%llu,\"wifi\":\"%s\","
            "\"expansion\":\"%s\",\"baud\":%lu,\"rpc_proxy\":false}",
            FLIPFORGE_BRIDGE_PRODUCT,
            FLIPFORGE_BRIDGE_VERSION,
            FLIPFORGE_BRIDGE_PROTOCOL_VERSION,
            (unsigned long long)(esp_timer_get_time() / 1000LL),
            wifi_manager_is_started() ? "softap" : "stopped",
            expansion_transport_state_name(expansion_transport_state(context->transport)),
            (unsigned long)expansion_transport_negotiated_baud(context->transport));
        if(length < 0 || (size_t)length >= sizeof(json)) return ESP_ERR_INVALID_SIZE;
        return send_response(
            context->socket_fd, request, BridgeStatusOk, (const uint8_t*)json, (size_t)length);
    }
    case BridgeCommandGetStatus: {
        if(request->payload_size != 0U) {
            return send_response(context->socket_fd, request, BridgeStatusMalformed, NULL, 0U);
        }
        const BridgeMetricsSnapshot metrics = bridge_metrics_snapshot();
        const int length = snprintf(
            json,
            sizeof(json),
            "{\"expansion\":\"%s\",\"baud\":%lu,\"tcp_rx\":%llu,"
            "\"tcp_tx\":%llu,\"rpc_rx\":%llu,\"rpc_tx\":%llu,"
            "\"retries\":%lu,\"errors\":%lu}",
            expansion_transport_state_name(expansion_transport_state(context->transport)),
            (unsigned long)expansion_transport_negotiated_baud(context->transport),
            (unsigned long long)metrics.tcp_rx_bytes,
            (unsigned long long)metrics.tcp_tx_bytes,
            (unsigned long long)metrics.rpc_payload_rx_bytes,
            (unsigned long long)metrics.rpc_payload_tx_bytes,
            (unsigned long)metrics.retries,
            (unsigned long)metrics.protocol_errors);
        if(length < 0 || (size_t)length >= sizeof(json)) return ESP_ERR_INVALID_SIZE;
        return send_response(
            context->socket_fd, request, BridgeStatusOk, (const uint8_t*)json, (size_t)length);
    }
    case BridgeCommandPing:
        return send_response(
            context->socket_fd,
            request,
            BridgeStatusOk,
            request->payload,
            request->payload_size);
    case BridgeCommandBeginRpcProxy:
        if(request->payload_size != 0U ||
           expansion_transport_state(context->transport) != ExpansionStateReady) {
            expansion_transport_request_reconnect(context->transport);
            return send_response(
                context->socket_fd, request, BridgeStatusUnavailable, NULL, 0U);
        }
        if(expansion_transport_begin_rpc(context->transport) != ESP_OK) {
            return send_response(
                context->socket_fd, request, BridgeStatusUnavailable, NULL, 0U);
        }
        if(!bridge_session_begin_proxy(session)) {
            expansion_transport_end_rpc(context->transport);
            return ESP_ERR_INVALID_STATE;
        }
        *begin_proxy = true;
        const esp_err_t response_error =
            send_response(context->socket_fd, request, BridgeStatusOk, NULL, 0U);
        if(response_error != ESP_OK) {
            *begin_proxy = false;
            expansion_transport_end_rpc(context->transport);
        }
        return response_error;
    case BridgeCommandEndRpcProxy:
        if(request->payload_size != 0U) {
            return send_response(context->socket_fd, request, BridgeStatusMalformed, NULL, 0U);
        }
        return send_response(context->socket_fd, request, BridgeStatusOk, NULL, 0U);
    default:
        return send_response(context->socket_fd, request, BridgeStatusInvalidState, NULL, 0U);
    }
}

static esp_err_t handle_management_message(
    ClientContext* context,
    BridgeSession* session,
    const BridgeMessage* request,
    bool* begin_proxy) {
    if(request->flags != 0U || request->status != BridgeStatusOk) {
        return send_response(context->socket_fd, request, BridgeStatusMalformed, NULL, 0U);
    }
    if(!bridge_protocol_version_supported(request->version)) {
        return send_response(
            context->socket_fd, request, BridgeStatusUnsupportedProtocol, NULL, 0U);
    }

    if(request->command == BridgeCommandHello) {
        if(request->payload_size != BRIDGE_NONCE_BYTES ||
           !bridge_session_accept_hello(session, request->payload)) {
            return send_response(context->socket_fd, request, BridgeStatusInvalidState, NULL, 0U);
        }
        uint8_t response[BRIDGE_NONCE_BYTES + BRIDGE_SESSION_ID_BYTES];
        memcpy(response, session->bridge_nonce, BRIDGE_NONCE_BYTES);
        memcpy(response + BRIDGE_NONCE_BYTES, session->session_id, BRIDGE_SESSION_ID_BYTES);
        return send_response(context->socket_fd, request, BridgeStatusOk, response, sizeof(response));
    }

    if(request->command == BridgeCommandAuthenticate) {
        if(session->state != BridgeSessionAwaitingAuthentication ||
           request->payload_size != BRIDGE_HMAC_BYTES) {
            return send_response(context->socket_fd, request, BridgeStatusInvalidState, NULL, 0U);
        }
        uint8_t material[128];
        const size_t material_size = bridge_session_auth_material(session, material, sizeof(material));
        uint8_t expected[BRIDGE_HMAC_BYTES];
        const esp_err_t hmac_error = bridge_auth_hmac_sha256(
            context->pairing_secret, material, material_size, expected);
        memset(material, 0, sizeof(material));
        const bool valid = hmac_error == ESP_OK &&
                           bridge_session_authenticate(session, expected, request->payload);
        memset(expected, 0, sizeof(expected));
        if(!valid) {
            ESP_LOGW(TAG, "Client authentication failed");
            send_response(context->socket_fd, request, BridgeStatusUnauthorized, NULL, 0U);
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGI(TAG, "Client authentication succeeded");
        return send_response(context->socket_fd, request, BridgeStatusOk, NULL, 0U);
    }

    return handle_authenticated_command(context, session, request, begin_proxy);
}

static void client_task(void* argument) {
    ClientContext* context = argument;
    set_socket_timeouts(context->socket_fd);
    uint8_t bridge_nonce[BRIDGE_NONCE_BYTES];
    uint8_t session_id[BRIDGE_SESSION_ID_BYTES];
    esp_fill_random(bridge_nonce, sizeof(bridge_nonce));
    esp_fill_random(session_id, sizeof(session_id));
    BridgeSession session;
    bridge_session_init(&session, bridge_nonce, session_id);
    memset(bridge_nonce, 0, sizeof(bridge_nonce));
    memset(session_id, 0, sizeof(session_id));

    BridgeMessageDecoder decoder;
    bridge_message_decoder_reset(&decoder);
    uint8_t input[BRIDGE_TCP_READ_BYTES];
    bool begin_proxy = false;
    int64_t last_activity_us = esp_timer_get_time();

    while(!begin_proxy) {
        const ssize_t received = recv(context->socket_fd, input, sizeof(input), 0);
        if(received == 0) break;
        if(received < 0) {
            if(errno == EINTR) continue;
            if((errno == EAGAIN || errno == EWOULDBLOCK) &&
               esp_timer_get_time() - last_activity_us <
                   (int64_t)BRIDGE_CLIENT_IDLE_TIMEOUT_MS * 1000LL) {
                continue;
            }
            break;
        }
        bridge_metrics_add_tcp_rx((size_t)received);
        last_activity_us = esp_timer_get_time();
        size_t offset = 0U;
        while(offset < (size_t)received && !begin_proxy) {
            BridgeMessage request;
            size_t consumed = 0U;
            const BridgeMessageDecodeResult decoded = bridge_message_decoder_push(
                &decoder,
                input + offset,
                (size_t)received - offset,
                &consumed,
                &request);
            offset += consumed;
            if(decoded == BridgeDecodeReady) {
                if(handle_management_message(context, &session, &request, &begin_proxy) != ESP_OK) {
                    begin_proxy = false;
                    goto done;
                }
                if(begin_proxy) {
                    const esp_err_t proxy_result = rpc_proxy_run(
                        context->socket_fd,
                        context->transport,
                        input + offset,
                        (size_t)received - offset);
                    if(proxy_result != ESP_OK) {
                        ESP_LOGW(TAG, "Proxy ended result=%s", esp_err_to_name(proxy_result));
                    }
                }
            } else if(decoded == BridgeDecodeMalformed || decoded == BridgeDecodeTooLarge) {
                BridgeMessage fallback = {
                    .version = FLIPFORGE_BRIDGE_PROTOCOL_VERSION,
                    .command = BridgeCommandHello,
                };
                send_response(
                    context->socket_fd,
                    &fallback,
                    decoded == BridgeDecodeTooLarge ? BridgeStatusTooLarge : BridgeStatusMalformed,
                    NULL,
                    0U);
                goto done;
            }
        }
    }

done:
    bridge_session_close(&session);
    shutdown(context->socket_fd, SHUT_RDWR);
    close(context->socket_fd);
    memset(context->pairing_secret, 0, sizeof(context->pairing_secret));
    free(context);
    release_client();
    ESP_LOGI(TAG, "Client disconnected");
    vTaskDelete(NULL);
}

static void reject_busy_client(int socket_fd) {
    set_socket_timeouts(socket_fd);
    BridgeMessage request = {
        .version = FLIPFORGE_BRIDGE_PROTOCOL_VERSION,
        .command = BridgeCommandHello,
    };
    send_response(socket_fd, &request, BridgeStatusBusy, NULL, 0U);
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
}

static void server_task(void* argument) {
    ServerContext* server = argument;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(listen_fd < 0) {
        ESP_LOGE(TAG, "Socket creation failed errno=%d", errno);
        goto done;
    }
    const int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(BRIDGE_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if(bind(listen_fd, (const struct sockaddr*)&address, sizeof(address)) != 0 ||
       listen(listen_fd, BRIDGE_TCP_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Listen setup failed errno=%d", errno);
        close(listen_fd);
        goto done;
    }
    ESP_LOGI(TAG, "TCP server listening port=%u", BRIDGE_TCP_PORT);

    for(;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if(client_fd < 0) {
            if(errno == EINTR) continue;
            ESP_LOGW(TAG, "Accept failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(250U));
            continue;
        }
        if(!claim_client()) {
            reject_busy_client(client_fd);
            continue;
        }
        ClientContext* client = calloc(1U, sizeof(*client));
        if(!client) {
            close(client_fd);
            release_client();
            continue;
        }
        client->socket_fd = client_fd;
        client->transport = server->transport;
        memcpy(client->pairing_secret, server->pairing_secret, sizeof(client->pairing_secret));
        if(xTaskCreate(client_task, "bridge_client", 8192U, client, 7U, NULL) != pdPASS) {
            memset(client->pairing_secret, 0, sizeof(client->pairing_secret));
            free(client);
            close(client_fd);
            release_client();
            continue;
        }
        ESP_LOGI(TAG, "Client connected");
    }

done:
    memset(server->pairing_secret, 0, sizeof(server->pairing_secret));
    free(server);
    vTaskDelete(NULL);
}

esp_err_t bridge_server_start(
    ExpansionTransport* transport,
    const BridgeCredentials* credentials) {
    if(!transport || !credentials) return ESP_ERR_INVALID_ARG;
    ServerContext* context = calloc(1U, sizeof(*context));
    if(!context) return ESP_ERR_NO_MEM;
    context->transport = transport;
    memcpy(context->pairing_secret, credentials->pairing_secret, sizeof(context->pairing_secret));
    if(xTaskCreate(server_task, "bridge_server", 6144U, context, 6U, NULL) != pdPASS) {
        memset(context->pairing_secret, 0, sizeof(context->pairing_secret));
        free(context);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
