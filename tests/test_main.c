#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_auth.h"
#include "bridge_io.h"
#include "bridge_session.h"
#include "expansion_protocol.h"
#include "expansion_state_machine.h"

static unsigned tests_run;
static unsigned tests_failed;

typedef struct {
    uint8_t output[32];
    size_t output_size;
    unsigned calls;
} PartialWriter;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        ++tests_run;                                                                        \
        if(!(condition)) {                                                                  \
            ++tests_failed;                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
        }                                                                                   \
    } while(0)

static ExpansionDecodeResult decode_bytes(
    const uint8_t* bytes,
    size_t size,
    ExpansionFrame* frame) {
    ExpansionDecoder decoder;
    expansion_decoder_reset(&decoder);
    ExpansionDecodeResult result = ExpansionDecodeNeedMore;
    for(size_t index = 0; index < size; ++index) {
        result = expansion_decoder_push(&decoder, bytes[index], frame);
    }
    return result;
}

static ptrdiff_t partial_write(
    void* context,
    const uint8_t* bytes,
    size_t size,
    bool* retryable) {
    PartialWriter* writer = context;
    ++writer->calls;
    if(writer->calls == 2U) {
        *retryable = true;
        return -1;
    }
    *retryable = false;
    const size_t chunk = size > 2U ? 2U : size;
    memcpy(writer->output + writer->output_size, bytes, chunk);
    writer->output_size += chunk;
    return (ptrdiff_t)chunk;
}

static void test_supported_hardware_mapping(void) {
    CHECK(BRIDGE_UART_PORT == 1);
    CHECK(BRIDGE_UART_TX_GPIO == 17);
    CHECK(BRIDGE_UART_RX_GPIO == 18);
    CHECK(BRIDGE_EXPANSION_STARTUP_DELAY_MS >= 3000U);
}

static void test_expansion_frames(void) {
    const ExpansionFrame frames[] = {
        {.type = ExpansionFrameTypeHeartbeat},
        {.type = ExpansionFrameTypeStatus, .content.status = ExpansionFrameErrorNone},
        {.type = ExpansionFrameTypeBaudRate, .content.baud_rate = 230400U},
        {.type = ExpansionFrameTypeControl, .content.control = ExpansionControlStartRpc},
        {.type = ExpansionFrameTypeControl, .content.control = ExpansionControlStopRpc},
        {.type = ExpansionFrameTypeData, .content.data = {.size = 3U, .bytes = {1U, 2U, 3U}}},
    };
    for(size_t index = 0U; index < sizeof(frames) / sizeof(frames[0]); ++index) {
        uint8_t encoded[BRIDGE_EXPANSION_MAX_DATA + 3U];
        size_t encoded_size = 0U;
        CHECK(expansion_protocol_encode(&frames[index], encoded, sizeof(encoded), &encoded_size));
        ExpansionFrame decoded;
        CHECK(decode_bytes(encoded, encoded_size, &decoded) == ExpansionDecodeFrameReady);
        CHECK(decoded.type == frames[index].type);
    }

    ExpansionFrame maximum = {.type = ExpansionFrameTypeData};
    maximum.content.data.size = BRIDGE_EXPANSION_MAX_DATA;
    memset(maximum.content.data.bytes, 0xA5, sizeof(maximum.content.data.bytes));
    uint8_t encoded[BRIDGE_EXPANSION_MAX_DATA + 3U];
    size_t encoded_size = 0U;
    CHECK(!expansion_protocol_encode(&maximum, encoded, 0U, &encoded_size));
    CHECK(expansion_protocol_encode(&maximum, encoded, sizeof(encoded), &encoded_size));
    CHECK(encoded_size == BRIDGE_EXPANSION_MAX_DATA + 3U);
    ExpansionFrame decoded;
    CHECK(decode_bytes(encoded, encoded_size, &decoded) == ExpansionDecodeFrameReady);
    CHECK(decoded.content.data.size == BRIDGE_EXPANSION_MAX_DATA);
    CHECK(memcmp(decoded.content.data.bytes, maximum.content.data.bytes, BRIDGE_EXPANSION_MAX_DATA) == 0);

    encoded[encoded_size - 1U] ^= 0x01U;
    CHECK(decode_bytes(encoded, encoded_size, &decoded) == ExpansionDecodeChecksumError);

    const uint8_t oversized[] = {ExpansionFrameTypeData, BRIDGE_EXPANSION_MAX_DATA + 1U};
    CHECK(decode_bytes(oversized, sizeof(oversized), &decoded) == ExpansionDecodeMalformed);
}

static void test_expansion_state_machine(void) {
    ExpansionStateMachine machine;
    ExpansionAction action;
    expansion_state_machine_init(&machine);
    CHECK(machine.state == ExpansionStateDisconnected);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventStart, &action));
    CHECK(machine.state == ExpansionStateInitialUart && action == ExpansionActionDetectPulse);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventHeartbeatReceived, &action));
    CHECK(machine.state == ExpansionStateNegotiatingBaud && action == ExpansionActionRequestBaud);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventBaudRejected, &action));
    CHECK(machine.state == ExpansionStateNegotiatingBaud && action == ExpansionActionTryLowerBaud);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventBaudAccepted, &action));
    CHECK(machine.state == ExpansionStateReady && action == ExpansionActionSwitchBaud);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventBeginRpc, &action));
    CHECK(machine.state == ExpansionStateStartingRpc && action == ExpansionActionSendStartRpc);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventRpcStarted, &action));
    CHECK(machine.state == ExpansionStateRpcReady);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventEndRpc, &action));
    CHECK(machine.state == ExpansionStateStoppingRpc && action == ExpansionActionSendStopRpc);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventRpcStopped, &action));
    CHECK(machine.state == ExpansionStateReady);

    CHECK(expansion_state_machine_apply(&machine, ExpansionEventTimeout, &action));
    CHECK(machine.state == ExpansionStateBackoff && action == ExpansionActionScheduleReconnect);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventBackoffElapsed, &action));
    CHECK(machine.state == ExpansionStateInitialUart && action == ExpansionActionDetectPulse);

    expansion_state_machine_init(&machine);
    for(unsigned attempt = 0; attempt < BRIDGE_EXPANSION_MAX_RETRIES; ++attempt) {
        CHECK(expansion_state_machine_apply(&machine, ExpansionEventTimeout, &action));
        CHECK(machine.state == ExpansionStateBackoff);
        CHECK(expansion_state_machine_apply(&machine, ExpansionEventBackoffElapsed, &action));
    }
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventTimeout, &action));
    CHECK(machine.state == ExpansionStateDisconnected && action == ExpansionActionNone);

    expansion_state_machine_init(&machine);
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventStart, &action));
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventHeartbeatReceived, &action));
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventBaudAccepted, &action));
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventBeginRpc, &action));
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventRpcStarted, &action));
    CHECK(expansion_state_machine_apply(&machine, ExpansionEventFrameError, &action));
    CHECK(machine.state == ExpansionStateBackoff && action == ExpansionActionScheduleReconnect);
}

static void test_management_fragmentation_and_coalescing(void) {
    BridgeMessage request = {
        .version = FLIPFORGE_BRIDGE_PROTOCOL_VERSION,
        .command = BridgeCommandPing,
        .request_id = 42U,
        .payload_size = 5U,
        .payload = {'h', 'e', 'l', 'l', 'o'},
    };
    uint8_t encoded[BRIDGE_MANAGEMENT_HEADER_SIZE + BRIDGE_MANAGEMENT_MAX_PAYLOAD];
    size_t encoded_size = 0U;
    CHECK(bridge_message_encode(&request, encoded, sizeof(encoded), &encoded_size));

    BridgeMessageDecoder decoder;
    bridge_message_decoder_reset(&decoder);
    BridgeMessage output;
    size_t consumed = 0U;
    CHECK(bridge_message_decoder_push(&decoder, encoded, 3U, &consumed, &output) == BridgeDecodeNeedMore);
    CHECK(consumed == 3U);
    CHECK(bridge_message_decoder_push(
              &decoder, &encoded[3], encoded_size - 3U, &consumed, &output) == BridgeDecodeReady);
    CHECK(output.command == BridgeCommandPing && output.request_id == 42U);
    CHECK(output.payload_size == 5U && memcmp(output.payload, "hello", 5U) == 0);

    uint8_t coalesced[(BRIDGE_MANAGEMENT_HEADER_SIZE + 5U) * 2U];
    memcpy(coalesced, encoded, encoded_size);
    memcpy(&coalesced[encoded_size], encoded, encoded_size);
    bridge_message_decoder_reset(&decoder);
    CHECK(bridge_message_decoder_push(&decoder, coalesced, encoded_size * 2U, &consumed, &output) == BridgeDecodeReady);
    CHECK(consumed == encoded_size);
    size_t second_consumed = 0U;
    CHECK(bridge_message_decoder_push(
              &decoder,
              &coalesced[consumed],
              encoded_size * 2U - consumed,
              &second_consumed,
              &output) == BridgeDecodeReady);
    CHECK(second_consumed == encoded_size);

    encoded[12] = 0xFFU;
    encoded[13] = 0x7FU;
    bridge_message_decoder_reset(&decoder);
    CHECK(bridge_message_decoder_push(
              &decoder, encoded, BRIDGE_MANAGEMENT_HEADER_SIZE, &consumed, &output) ==
          BridgeDecodeTooLarge);
}

static void test_session_auth_material_and_replay_state(void) {
    uint8_t bridge_nonce[BRIDGE_NONCE_BYTES];
    uint8_t client_nonce[BRIDGE_NONCE_BYTES];
    uint8_t session_id[BRIDGE_SESSION_ID_BYTES];
    memset(bridge_nonce, 0x11, sizeof(bridge_nonce));
    memset(client_nonce, 0x22, sizeof(client_nonce));
    memset(session_id, 0x33, sizeof(session_id));

    BridgeSession session;
    bridge_session_init(&session, bridge_nonce, session_id);
    CHECK(bridge_session_accept_hello(&session, client_nonce));
    CHECK(!bridge_session_accept_hello(&session, client_nonce));
    uint8_t material[128];
    const size_t material_size = bridge_session_auth_material(&session, material, sizeof(material));
    CHECK(material_size > BRIDGE_NONCE_BYTES * 2U);
    CHECK(memcmp(
              &material[sizeof("Flipforge Bridge Auth v1")],
              client_nonce,
              sizeof(client_nonce)) == 0);
    CHECK(bridge_session_mark_authenticated(&session));
    CHECK(!bridge_session_mark_authenticated(&session));
    CHECK(bridge_session_begin_proxy(&session));
    CHECK(!bridge_session_begin_proxy(&session));
    bridge_session_close(&session);
    CHECK(session.state == BridgeSessionClosed);

    uint8_t lhs[BRIDGE_HMAC_BYTES] = {0};
    uint8_t rhs[BRIDGE_HMAC_BYTES] = {0};
    CHECK(bridge_constant_time_equal(lhs, rhs, sizeof(lhs)));
    rhs[BRIDGE_HMAC_BYTES - 1U] = 1U;
    CHECK(!bridge_constant_time_equal(lhs, rhs, sizeof(lhs)));

    BridgeSession second;
    bridge_nonce[0] ^= 0xFFU;
    bridge_session_init(&second, bridge_nonce, session_id);
    CHECK(bridge_session_accept_hello(&second, client_nonce));
    uint8_t second_material[128];
    const size_t second_size = bridge_session_auth_material(&second, second_material, sizeof(second_material));
    CHECK(second_size == material_size);
    CHECK(memcmp(material, second_material, material_size) != 0);
}

static void test_authentication_and_protocol_version(void) {
    uint8_t secret[BRIDGE_PAIRING_SECRET_BYTES];
    uint8_t bridge_nonce[BRIDGE_NONCE_BYTES];
    uint8_t client_nonce[BRIDGE_NONCE_BYTES];
    uint8_t session_id[BRIDGE_SESSION_ID_BYTES];
    memset(secret, 0x44, sizeof(secret));
    memset(bridge_nonce, 0x55, sizeof(bridge_nonce));
    memset(client_nonce, 0x66, sizeof(client_nonce));
    memset(session_id, 0x77, sizeof(session_id));

    BridgeSession valid;
    bridge_session_init(&valid, bridge_nonce, session_id);
    CHECK(bridge_session_accept_hello(&valid, client_nonce));
    uint8_t material[128];
    const size_t material_size = bridge_session_auth_material(&valid, material, sizeof(material));
    uint8_t hmac[BRIDGE_HMAC_BYTES];
    CHECK(bridge_auth_hmac_sha256(secret, material, material_size, hmac) == ESP_OK);
    CHECK(bridge_session_authenticate(&valid, hmac, hmac));
    CHECK(valid.state == BridgeSessionAuthenticated);

    BridgeSession invalid;
    bridge_session_init(&invalid, bridge_nonce, session_id);
    CHECK(bridge_session_accept_hello(&invalid, client_nonce));
    uint8_t altered[BRIDGE_HMAC_BYTES];
    memcpy(altered, hmac, sizeof(altered));
    altered[0] ^= 0x01U;
    CHECK(!bridge_session_authenticate(&invalid, hmac, altered));
    CHECK(invalid.state == BridgeSessionAwaitingAuthentication);

    BridgeSession replay;
    bridge_nonce[0] ^= 0x80U;
    bridge_session_init(&replay, bridge_nonce, session_id);
    CHECK(bridge_session_accept_hello(&replay, client_nonce));
    uint8_t replay_material[128];
    const size_t replay_size =
        bridge_session_auth_material(&replay, replay_material, sizeof(replay_material));
    uint8_t replay_hmac[BRIDGE_HMAC_BYTES];
    CHECK(bridge_auth_hmac_sha256(secret, replay_material, replay_size, replay_hmac) == ESP_OK);
    CHECK(!bridge_session_authenticate(&replay, replay_hmac, hmac));
    CHECK(bridge_protocol_version_supported(FLIPFORGE_BRIDGE_PROTOCOL_VERSION));
    CHECK(!bridge_protocol_version_supported(FLIPFORGE_BRIDGE_PROTOCOL_VERSION + 1U));
}

static void test_partial_writes_and_bounded_buffer(void) {
    static const uint8_t input[] = "fragmented-output";
    PartialWriter writer = {0};
    CHECK(bridge_io_write_all(partial_write, &writer, input, sizeof(input) - 1U));
    CHECK(writer.calls > 2U);
    CHECK(writer.output_size == sizeof(input) - 1U);
    CHECK(memcmp(writer.output, input, sizeof(input) - 1U) == 0);

    CHECK(bridge_io_buffer_can_accept(0U, 64U, 8192U));
    CHECK(bridge_io_buffer_can_accept(8128U, 64U, 8192U));
    CHECK(!bridge_io_buffer_can_accept(8129U, 64U, 8192U));
    CHECK(!bridge_io_buffer_can_accept(8193U, 0U, 8192U));
}

int main(void) {
    test_supported_hardware_mapping();
    test_expansion_frames();
    test_expansion_state_machine();
    test_management_fragmentation_and_coalescing();
    test_session_auth_material_and_replay_state();
    test_authentication_and_protocol_version();
    test_partial_writes_and_bounded_buffer();
    printf("Flipforge Bridge host tests: %u checks, %u failures\n", tests_run, tests_failed);
    return tests_failed == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
