#include "expansion_transport.h"

#include <stdlib.h>
#include <string.h>

#include "activity_led.h"
#include "bridge_config.h"
#include "bridge_io.h"
#include "bridge_metrics.h"
#include "expansion_protocol.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define TRANSPORT_STOP_BIT (1U << 0U)
#define TRANSPORT_LOST_BIT (1U << 1U)
#define TRANSPORT_READY_BIT (1U << 2U)
#define TRANSPORT_RECONNECT_BIT (1U << 3U)

static const char* TAG = "expansion";

const uint32_t bridge_preferred_baud_rates[] = {
    230400U,
    115200U,
    57600U,
    38400U,
    19200U,
    9600U,
};
const size_t bridge_preferred_baud_rate_count =
    sizeof(bridge_preferred_baud_rates) / sizeof(bridge_preferred_baud_rates[0]);

struct ExpansionTransport {
    uart_port_t uart;
    ExpansionStateMachine machine;
    uint32_t negotiated_baud;
    bool running;
    bool uart_installed;
    bool awaiting_status;
    ExpansionFrameError last_status;
    int64_t last_rx_us;
    int64_t last_tx_us;
    EventGroupHandle_t events;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t tx_mutex;
    SemaphoreHandle_t request_mutex;
    SemaphoreHandle_t status_semaphore;
    StreamBufferHandle_t rpc_rx;
    TaskHandle_t supervisor_task;
    TaskHandle_t rx_task;
    TaskHandle_t heartbeat_task;
};

static void mark_rx_activity(ExpansionTransport* transport) {
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    transport->last_rx_us = esp_timer_get_time();
    xSemaphoreGive(transport->state_mutex);
}

static void mark_tx_activity(ExpansionTransport* transport) {
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    transport->last_tx_us = esp_timer_get_time();
    xSemaphoreGive(transport->state_mutex);
}

static bool transport_apply(
    ExpansionTransport* transport,
    ExpansionEvent event,
    ExpansionAction* action) {
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    const bool accepted = expansion_state_machine_apply(&transport->machine, event, action);
    xSemaphoreGive(transport->state_mutex);
    return accepted;
}

ExpansionState expansion_transport_state(ExpansionTransport* transport) {
    if(!transport) return ExpansionStateDisconnected;
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    const ExpansionState state = transport->machine.state;
    xSemaphoreGive(transport->state_mutex);
    return state;
}

uint32_t expansion_transport_negotiated_baud(ExpansionTransport* transport) {
    if(!transport) return 0U;
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    const uint32_t baud = transport->negotiated_baud;
    xSemaphoreGive(transport->state_mutex);
    return baud;
}

const char* expansion_transport_state_name(ExpansionState state) {
    switch(state) {
    case ExpansionStateDisconnected: return "disconnected";
    case ExpansionStateInitialUart: return "initial_uart";
    case ExpansionStateNegotiatingBaud: return "negotiating_baud";
    case ExpansionStateReady: return "ready";
    case ExpansionStateStartingRpc: return "starting_rpc";
    case ExpansionStateRpcReady: return "rpc_ready";
    case ExpansionStateStoppingRpc: return "stopping_rpc";
    case ExpansionStateBackoff: return "backoff";
    }
    return "unknown";
}

static void signal_link_lost(ExpansionTransport* transport, const char* reason) {
    const EventBits_t previous = xEventGroupGetBits(transport->events);
    xEventGroupSetBits(transport->events, TRANSPORT_LOST_BIT);
    if(!(previous & TRANSPORT_LOST_BIT)) {
        bridge_metrics_protocol_error();
        ESP_LOGW(TAG, "Expansion connection lost reason=%s", reason);
    }
}

static esp_err_t uart_configure(ExpansionTransport* transport, uint32_t baud_rate) {
    const uart_config_t config = {
        .baud_rate = (int)baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t error = uart_param_config(transport->uart, &config);
    if(error == ESP_OK) {
        error = uart_set_pin(
            transport->uart,
            BRIDGE_UART_TX_GPIO,
            BRIDGE_UART_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
    }
    if(error == ESP_OK) error = uart_flush_input(transport->uart);
    return error;
}

static esp_err_t send_frame(ExpansionTransport* transport, const ExpansionFrame* frame) {
    uint8_t encoded[BRIDGE_EXPANSION_MAX_DATA + 3U];
    size_t encoded_size = 0U;
    if(!expansion_protocol_encode(frame, encoded, sizeof(encoded), &encoded_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(xSemaphoreTake(transport->tx_mutex, pdMS_TO_TICKS(BRIDGE_EXPANSION_ACK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int written = uart_write_bytes(transport->uart, encoded, encoded_size);
    esp_err_t error = written == (int)encoded_size ?
                          uart_wait_tx_done(
                              transport->uart,
                              pdMS_TO_TICKS(BRIDGE_EXPANSION_ACK_TIMEOUT_MS)) : ESP_FAIL;
    if(error == ESP_OK) {
        mark_tx_activity(transport);
        bridge_metrics_add_expansion_tx(encoded_size);
        bridge_metrics_frame();
    }
    xSemaphoreGive(transport->tx_mutex);
    return error;
}

static esp_err_t send_status(ExpansionTransport* transport, ExpansionFrameError status);

static esp_err_t receive_frame_sync(
    ExpansionTransport* transport,
    ExpansionFrame* frame,
    uint32_t timeout_ms) {
    ExpansionDecoder decoder;
    expansion_decoder_reset(&decoder);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while(esp_timer_get_time() < deadline) {
        uint8_t byte;
        const int count = uart_read_bytes(transport->uart, &byte, 1U, pdMS_TO_TICKS(10U));
        if(count < 0) return ESP_FAIL;
        if(count == 0) continue;
        bridge_metrics_add_expansion_rx(1U);
        const ExpansionDecodeResult result = expansion_decoder_push(&decoder, byte, frame);
        if(result == ExpansionDecodeFrameReady) {
            mark_rx_activity(transport);
            bridge_metrics_frame();
            return ESP_OK;
        }
        if(result == ExpansionDecodeMalformed || result == ExpansionDecodeChecksumError) {
            send_status(transport, ExpansionFrameErrorUnknown);
            bridge_metrics_protocol_error();
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_status(ExpansionTransport* transport, ExpansionFrameError status) {
    const ExpansionFrame frame = {
        .type = ExpansionFrameTypeStatus,
        .content.status = status,
    };
    return send_frame(transport, &frame);
}

static esp_err_t send_wait_status(
    ExpansionTransport* transport,
    const ExpansionFrame* frame,
    ExpansionFrameError* status) {
    if(xSemaphoreTake(transport->request_mutex, pdMS_TO_TICKS(BRIDGE_EXPANSION_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    while(xSemaphoreTake(transport->status_semaphore, 0U) == pdTRUE) {}
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    transport->awaiting_status = true;
    transport->last_status = ExpansionFrameErrorUnknown;
    xSemaphoreGive(transport->state_mutex);
    const int64_t started = esp_timer_get_time();
    esp_err_t error = send_frame(transport, frame);
    if(error == ESP_OK &&
       xSemaphoreTake(
           transport->status_semaphore,
           pdMS_TO_TICKS(BRIDGE_EXPANSION_ACK_TIMEOUT_MS)) != pdTRUE) {
        error = ESP_ERR_TIMEOUT;
    }
    bridge_metrics_ack_wait((uint64_t)(esp_timer_get_time() - started));
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    transport->awaiting_status = false;
    if(status) *status = transport->last_status;
    xSemaphoreGive(transport->state_mutex);
    xSemaphoreGive(transport->request_mutex);
    return error;
}

static void expansion_rx_task(void* argument) {
    ExpansionTransport* transport = argument;
    ExpansionDecoder decoder;
    expansion_decoder_reset(&decoder);
    while(!(xEventGroupGetBits(transport->events) &
            (TRANSPORT_STOP_BIT | TRANSPORT_LOST_BIT))) {
        uint8_t byte;
        const int count = uart_read_bytes(transport->uart, &byte, 1U, pdMS_TO_TICKS(20U));
        if(count < 0) {
            signal_link_lost(transport, "uart_read");
            break;
        }
        if(count == 0) continue;
        bridge_metrics_add_expansion_rx(1U);
        ExpansionFrame frame;
        const ExpansionDecodeResult result = expansion_decoder_push(&decoder, byte, &frame);
        if(result == ExpansionDecodeNeedMore) continue;
        if(result != ExpansionDecodeFrameReady) {
            signal_link_lost(transport, result == ExpansionDecodeChecksumError ? "checksum" : "frame");
            break;
        }
        mark_rx_activity(transport);
        bridge_metrics_frame();
        if(frame.type == ExpansionFrameTypeHeartbeat) {
            continue;
        } else if(frame.type == ExpansionFrameTypeStatus) {
            xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
            const bool expected = transport->awaiting_status;
            if(expected) transport->last_status = frame.content.status;
            xSemaphoreGive(transport->state_mutex);
            if(expected) xSemaphoreGive(transport->status_semaphore);
            else {
                signal_link_lost(transport, "unsolicited_status");
                break;
            }
        } else if(frame.type == ExpansionFrameTypeData &&
                  expansion_transport_state(transport) == ExpansionStateRpcReady) {
            const size_t used = xStreamBufferBytesAvailable(transport->rpc_rx);
            if(!bridge_io_buffer_can_accept(
                   used, frame.content.data.size, BRIDGE_RPC_RX_BUFFER_BYTES)) {
                send_status(transport, ExpansionFrameErrorUnknown);
                signal_link_lost(transport, "rpc_rx_overflow");
                break;
            }
            const size_t sent = xStreamBufferSend(
                transport->rpc_rx,
                frame.content.data.bytes,
                frame.content.data.size,
                pdMS_TO_TICKS(BRIDGE_EXPANSION_ACK_TIMEOUT_MS));
            if(sent != frame.content.data.size) {
                signal_link_lost(transport, "rpc_rx_overflow");
                break;
            }
            if(send_status(transport, ExpansionFrameErrorNone) != ESP_OK) {
                signal_link_lost(transport, "data_ack");
                break;
            }
            bridge_metrics_add_rpc_rx(sent);
            bridge_metrics_queue_used(xStreamBufferBytesAvailable(transport->rpc_rx));
        } else {
            signal_link_lost(transport, "unexpected_frame");
            break;
        }
    }
    transport->rx_task = NULL;
    vTaskDelete(NULL);
}

static void expansion_heartbeat_task(void* argument) {
    ExpansionTransport* transport = argument;
    const TickType_t interval = pdMS_TO_TICKS(BRIDGE_EXPANSION_HEARTBEAT_MS / 4U);
    while(!(xEventGroupGetBits(transport->events) &
            (TRANSPORT_STOP_BIT | TRANSPORT_LOST_BIT))) {
        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        const int64_t last_rx_us = transport->last_rx_us;
        const int64_t last_tx_us = transport->last_tx_us;
        const bool request_pending = transport->awaiting_status;
        xSemaphoreGive(transport->state_mutex);
        if(now - last_rx_us > (int64_t)BRIDGE_EXPANSION_TIMEOUT_MS * 1000LL) {
            signal_link_lost(transport, "heartbeat_timeout");
            break;
        }
        if(now - last_tx_us >=
           (int64_t)BRIDGE_EXPANSION_HEARTBEAT_MS * 1000LL) {
            if(request_pending) {
                vTaskDelay(interval);
                continue;
            }
            const ExpansionFrame heartbeat = {.type = ExpansionFrameTypeHeartbeat};
            if(send_frame(transport, &heartbeat) != ESP_OK) {
                signal_link_lost(transport, "heartbeat_send");
                break;
            }
        }
        vTaskDelay(interval);
    }
    transport->heartbeat_task = NULL;
    vTaskDelete(NULL);
}

static void release_expansion_pins(void) {
    gpio_reset_pin(BRIDGE_UART_TX_GPIO);
    gpio_reset_pin(BRIDGE_UART_RX_GPIO);
}

static esp_err_t pulse_flipper_rx(void) {
    esp_err_t error = gpio_reset_pin(BRIDGE_UART_TX_GPIO);
    if(error != ESP_OK) return error;
    error = gpio_set_direction(BRIDGE_UART_TX_GPIO, GPIO_MODE_OUTPUT_OD);
    if(error != ESP_OK) return error;
    error = gpio_set_level(BRIDGE_UART_TX_GPIO, 1);
    if(error != ESP_OK) return error;
    esp_rom_delay_us(100U);
    error = gpio_set_level(BRIDGE_UART_TX_GPIO, 0);
    if(error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(BRIDGE_EXPANSION_DETECT_PULSE_MS));
    return gpio_set_level(BRIDGE_UART_TX_GPIO, 1);
}

static esp_err_t establish_expansion(ExpansionTransport* transport) {
    ExpansionAction action;
    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    expansion_state_machine_init(&transport->machine);
    transport->negotiated_baud = 0U;
    xSemaphoreGive(transport->state_mutex);
    if(!transport_apply(transport, ExpansionEventStart, &action) ||
       action != ExpansionActionDetectPulse) return ESP_FAIL;
    esp_err_t error = uart_configure(transport, BRIDGE_EXPANSION_INITIAL_BAUD);
    if(error != ESP_OK) return error;
    error = pulse_flipper_rx();
    if(error != ESP_OK) return error;
    error = uart_set_pin(
        transport->uart,
        BRIDGE_UART_TX_GPIO,
        BRIDGE_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if(error != ESP_OK) return error;

    ExpansionFrame response;
    error = receive_frame_sync(transport, &response, BRIDGE_EXPANSION_TIMEOUT_MS);
    if(error != ESP_OK || response.type != ExpansionFrameTypeHeartbeat) return ESP_ERR_TIMEOUT;
    if(!transport_apply(transport, ExpansionEventHeartbeatReceived, &action)) return ESP_FAIL;

    for(size_t index = 0U; index < bridge_preferred_baud_rate_count; ++index) {
        const uint32_t baud = bridge_preferred_baud_rates[index];
        const ExpansionFrame request = {
            .type = ExpansionFrameTypeBaudRate,
            .content.baud_rate = baud,
        };
        error = send_frame(transport, &request);
        if(error != ESP_OK) return error;
        error = receive_frame_sync(transport, &response, BRIDGE_EXPANSION_TIMEOUT_MS);
        if(error != ESP_OK || response.type != ExpansionFrameTypeStatus) return ESP_ERR_INVALID_RESPONSE;
        if(response.content.status == ExpansionFrameErrorBaudRate) {
            transport_apply(transport, ExpansionEventBaudRejected, &action);
            ESP_LOGW(TAG, "Baud rejected rate=%lu", (unsigned long)baud);
            continue;
        }
        if(response.content.status != ExpansionFrameErrorNone ||
           !transport_apply(transport, ExpansionEventBaudAccepted, &action)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_EXPANSION_BAUD_DEAD_TIME_MS));
        error = uart_set_baudrate(transport->uart, baud);
        if(error != ESP_OK) return error;
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        transport->negotiated_baud = baud;
        xSemaphoreGive(transport->state_mutex);
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        transport->last_rx_us = esp_timer_get_time();
        transport->last_tx_us = transport->last_rx_us;
        xSemaphoreGive(transport->state_mutex);
        ESP_LOGI(TAG, "Expansion ready negotiated_baud=%lu", (unsigned long)baud);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static void stop_io_tasks(ExpansionTransport* transport) {
    for(unsigned wait = 0U; wait < 25U &&
         (transport->rx_task || transport->heartbeat_task); ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if(transport->rx_task) {
        vTaskDelete(transport->rx_task);
        transport->rx_task = NULL;
    }
    if(transport->heartbeat_task) {
        vTaskDelete(transport->heartbeat_task);
        transport->heartbeat_task = NULL;
    }
}

static void expansion_supervisor_task(void* argument) {
    ExpansionTransport* transport = argument;
    unsigned attempts = 0U;
    xEventGroupClearBits(transport->events, TRANSPORT_RECONNECT_BIT);
    vTaskDelay(pdMS_TO_TICKS(BRIDGE_EXPANSION_STARTUP_DELAY_MS));
    while(transport->running) {
        if(attempts >= BRIDGE_EXPANSION_MAX_RETRIES) {
            release_expansion_pins();
            ESP_LOGE(TAG, "Reconnect limit reached; waiting for explicit retry");
            const EventBits_t wake = xEventGroupWaitBits(
                transport->events,
                TRANSPORT_RECONNECT_BIT | TRANSPORT_STOP_BIT,
                pdTRUE,
                pdFALSE,
                portMAX_DELAY);
            if(wake & TRANSPORT_STOP_BIT) break;
            attempts = 0U;
        }
        xEventGroupClearBits(transport->events, TRANSPORT_LOST_BIT | TRANSPORT_READY_BIT);
        const esp_err_t error = establish_expansion(transport);
        if(error != ESP_OK) {
            release_expansion_pins();
            ++attempts;
            bridge_metrics_retry();
            ExpansionAction action;
            transport_apply(transport, ExpansionEventTimeout, &action);
            ESP_LOGW(TAG, "Expansion attempt=%u/%u failed error=%s", attempts,
                     BRIDGE_EXPANSION_MAX_RETRIES, esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(BRIDGE_EXPANSION_RETRY_BASE_MS * attempts));
            continue;
        }
        attempts = 0U;
        xStreamBufferReset(transport->rpc_rx);
        if(xTaskCreate(expansion_rx_task, "expansion_rx", 4096U, transport, 10U,
                       &transport->rx_task) != pdPASS ||
           xTaskCreate(expansion_heartbeat_task, "expansion_hb", 3072U, transport, 8U,
                       &transport->heartbeat_task) != pdPASS) {
            signal_link_lost(transport, "task_allocation");
        }
        xEventGroupSetBits(transport->events, TRANSPORT_READY_BIT);
        xEventGroupWaitBits(
            transport->events,
            TRANSPORT_LOST_BIT | TRANSPORT_STOP_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
        stop_io_tasks(transport);
        xEventGroupClearBits(transport->events, TRANSPORT_READY_BIT);
        if(xEventGroupGetBits(transport->events) & TRANSPORT_STOP_BIT) break;
        ExpansionAction action;
        transport_apply(transport, ExpansionEventTimeout, &action);
        ++attempts;
        bridge_metrics_retry();
        uart_flush_input(transport->uart);
        release_expansion_pins();
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_EXPANSION_RETRY_BASE_MS * attempts));
    }
    transport->supervisor_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t expansion_transport_create(ExpansionTransport** output) {
    if(!output) return ESP_ERR_INVALID_ARG;
    ExpansionTransport* transport = calloc(1U, sizeof(*transport));
    if(!transport) return ESP_ERR_NO_MEM;
    transport->uart = (uart_port_t)BRIDGE_UART_PORT;
    transport->events = xEventGroupCreate();
    transport->state_mutex = xSemaphoreCreateMutex();
    transport->tx_mutex = xSemaphoreCreateMutex();
    transport->request_mutex = xSemaphoreCreateMutex();
    transport->status_semaphore = xSemaphoreCreateBinary();
    transport->rpc_rx = xStreamBufferCreate(BRIDGE_RPC_RX_BUFFER_BYTES, 1U);
    if(!transport->events || !transport->state_mutex || !transport->tx_mutex ||
       !transport->request_mutex || !transport->status_semaphore || !transport->rpc_rx) {
        expansion_transport_destroy(transport);
        return ESP_ERR_NO_MEM;
    }
    expansion_state_machine_init(&transport->machine);
    const esp_err_t error = uart_driver_install(
        transport->uart,
        2048U,
        0U,
        0U,
        NULL,
        0U);
    if(error != ESP_OK) {
        expansion_transport_destroy(transport);
        return error;
    }
    transport->uart_installed = true;
    *output = transport;
    return ESP_OK;
}

esp_err_t expansion_transport_start(ExpansionTransport* transport) {
    if(!transport || transport->running) return transport ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    ESP_LOGI(
        TAG,
        "Expansion UART configured port=%d tx_gpio=%d rx_gpio=%d initial_baud=%u",
        BRIDGE_UART_PORT,
        BRIDGE_UART_TX_GPIO,
        BRIDGE_UART_RX_GPIO,
        BRIDGE_EXPANSION_INITIAL_BAUD);
    transport->running = true;
    if(xTaskCreate(expansion_supervisor_task, "expansion", 6144U, transport, 9U,
                   &transport->supervisor_task) != pdPASS) {
        transport->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void expansion_transport_destroy(ExpansionTransport* transport) {
    if(!transport) return;
    transport->running = false;
    if(transport->events) xEventGroupSetBits(transport->events, TRANSPORT_STOP_BIT);
    for(unsigned wait = 0U; wait < 50U && transport->supervisor_task; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    stop_io_tasks(transport);
    if(transport->supervisor_task) vTaskDelete(transport->supervisor_task);
    if(transport->uart_installed) uart_driver_delete(transport->uart);
    if(transport->rpc_rx) vStreamBufferDelete(transport->rpc_rx);
    if(transport->status_semaphore) vSemaphoreDelete(transport->status_semaphore);
    if(transport->request_mutex) vSemaphoreDelete(transport->request_mutex);
    if(transport->tx_mutex) vSemaphoreDelete(transport->tx_mutex);
    if(transport->state_mutex) vSemaphoreDelete(transport->state_mutex);
    if(transport->events) vEventGroupDelete(transport->events);
    free(transport);
}

void expansion_transport_request_reconnect(ExpansionTransport* transport) {
    if(transport) xEventGroupSetBits(transport->events, TRANSPORT_RECONNECT_BIT);
}

esp_err_t expansion_transport_begin_rpc(ExpansionTransport* transport) {
    if(!transport) return ESP_ERR_INVALID_ARG;
    if(expansion_transport_state(transport) != ExpansionStateReady) {
        expansion_transport_request_reconnect(transport);
        return ESP_ERR_INVALID_STATE;
    }
    ExpansionAction action;
    if(!transport_apply(transport, ExpansionEventBeginRpc, &action)) return ESP_ERR_INVALID_STATE;
    const ExpansionFrame frame = {
        .type = ExpansionFrameTypeControl,
        .content.control = ExpansionControlStartRpc,
    };
    ExpansionFrameError status;
    const esp_err_t error = send_wait_status(transport, &frame, &status);
    if(error != ESP_OK || status != ExpansionFrameErrorNone ||
       !transport_apply(transport, ExpansionEventRpcStarted, &action)) {
        signal_link_lost(transport, "start_rpc");
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    ESP_LOGI(TAG, "RPC session started");
    return ESP_OK;
}

esp_err_t expansion_transport_end_rpc(ExpansionTransport* transport) {
    if(!transport) return ESP_ERR_INVALID_ARG;
    if(expansion_transport_state(transport) == ExpansionStateReady) return ESP_OK;
    if(expansion_transport_state(transport) != ExpansionStateRpcReady) return ESP_ERR_INVALID_STATE;
    ExpansionAction action;
    if(!transport_apply(transport, ExpansionEventEndRpc, &action)) return ESP_ERR_INVALID_STATE;
    const ExpansionFrame frame = {
        .type = ExpansionFrameTypeControl,
        .content.control = ExpansionControlStopRpc,
    };
    ExpansionFrameError status;
    const esp_err_t error = send_wait_status(transport, &frame, &status);
    if(error != ESP_OK || status != ExpansionFrameErrorNone ||
       !transport_apply(transport, ExpansionEventRpcStopped, &action)) {
        signal_link_lost(transport, "stop_rpc");
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    xStreamBufferReset(transport->rpc_rx);
    ESP_LOGI(TAG, "RPC session stopped");
    return ESP_OK;
}

esp_err_t expansion_transport_send_rpc(
    ExpansionTransport* transport,
    const uint8_t* bytes,
    size_t size) {
    if(!transport || (!bytes && size > 0U)) return ESP_ERR_INVALID_ARG;
    if(expansion_transport_state(transport) != ExpansionStateRpcReady) return ESP_ERR_INVALID_STATE;
    for(size_t offset = 0U; offset < size;) {
        const size_t chunk = (size - offset) < BRIDGE_EXPANSION_MAX_DATA ?
                                 size - offset : BRIDGE_EXPANSION_MAX_DATA;
        ExpansionFrame frame = {.type = ExpansionFrameTypeData};
        frame.content.data.size = (uint8_t)chunk;
        memcpy(frame.content.data.bytes, &bytes[offset], chunk);
        ExpansionFrameError status;
        const esp_err_t error = send_wait_status(transport, &frame, &status);
        if(error != ESP_OK || status != ExpansionFrameErrorNone) {
            signal_link_lost(transport, "rpc_ack");
            return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
        }
        bridge_metrics_add_rpc_tx(chunk);
        offset += chunk;
    }
    activity_led_note_transfer(size);
    return ESP_OK;
}

esp_err_t expansion_transport_receive_rpc(
    ExpansionTransport* transport,
    uint8_t* output,
    size_t capacity,
    size_t* output_size,
    uint32_t timeout_ms) {
    if(!transport || !output || capacity == 0U || !output_size) return ESP_ERR_INVALID_ARG;
    const size_t received = xStreamBufferReceive(
        transport->rpc_rx,
        output,
        capacity,
        pdMS_TO_TICKS(timeout_ms));
    *output_size = received;
    if(received > 0U) {
        activity_led_note_transfer(received);
        return ESP_OK;
    }
    return expansion_transport_state(transport) == ExpansionStateRpcReady ?
               ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
