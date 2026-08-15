#include "bridge_session.h"

#include <string.h>

static const uint8_t auth_context[] = "Flipforge Bridge Auth v1";

static void write_u16_le(uint8_t* output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint16_t read_u16_le(const uint8_t* input) {
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

static uint32_t read_u32_le(const uint8_t* input) {
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

void bridge_message_decoder_reset(BridgeMessageDecoder* decoder) {
    if(decoder) memset(decoder, 0, sizeof(*decoder));
}

BridgeMessageDecodeResult bridge_message_decoder_push(
    BridgeMessageDecoder* decoder,
    const uint8_t* bytes,
    size_t size,
    size_t* consumed,
    BridgeMessage* message) {
    if(!decoder || !bytes || !consumed || !message) return BridgeDecodeMalformed;
    *consumed = 0U;
    while(*consumed < size) {
        if(decoder->used >= sizeof(decoder->bytes)) {
            bridge_message_decoder_reset(decoder);
            return BridgeDecodeTooLarge;
        }
        decoder->bytes[decoder->used++] = bytes[(*consumed)++];
        if(decoder->used == BRIDGE_MANAGEMENT_HEADER_SIZE) {
            if(memcmp(decoder->bytes, BRIDGE_MANAGEMENT_MAGIC, 4U) != 0 ||
               decoder->bytes[6] & (uint8_t)~BRIDGE_MANAGEMENT_RESPONSE_FLAG ||
               decoder->bytes[7] > BridgeStatusInvalidState) {
                bridge_message_decoder_reset(decoder);
                return BridgeDecodeMalformed;
            }
            const uint16_t payload_size = read_u16_le(&decoder->bytes[12]);
            if(payload_size > BRIDGE_MANAGEMENT_MAX_PAYLOAD) {
                bridge_message_decoder_reset(decoder);
                return BridgeDecodeTooLarge;
            }
            decoder->expected = BRIDGE_MANAGEMENT_HEADER_SIZE + payload_size;
        }
        if(decoder->expected > 0U && decoder->used == decoder->expected) {
            memset(message, 0, sizeof(*message));
            message->version = decoder->bytes[4];
            message->command = (BridgeCommand)decoder->bytes[5];
            message->flags = decoder->bytes[6];
            message->status = (BridgeStatus)decoder->bytes[7];
            message->request_id = read_u32_le(&decoder->bytes[8]);
            message->payload_size = read_u16_le(&decoder->bytes[12]);
            memcpy(
                message->payload,
                &decoder->bytes[BRIDGE_MANAGEMENT_HEADER_SIZE],
                message->payload_size);
            bridge_message_decoder_reset(decoder);
            return BridgeDecodeReady;
        }
    }
    return BridgeDecodeNeedMore;
}

bool bridge_message_encode(
    const BridgeMessage* message,
    uint8_t* output,
    size_t capacity,
    size_t* output_size) {
    if(!message || !output || !output_size ||
       message->payload_size > BRIDGE_MANAGEMENT_MAX_PAYLOAD ||
       message->command < BridgeCommandHello || message->command > BridgeCommandPing) {
        return false;
    }
    const size_t required = BRIDGE_MANAGEMENT_HEADER_SIZE + message->payload_size;
    if(capacity < required) return false;
    memcpy(output, BRIDGE_MANAGEMENT_MAGIC, 4U);
    output[4] = message->version;
    output[5] = (uint8_t)message->command;
    output[6] = message->flags;
    output[7] = (uint8_t)message->status;
    write_u32_le(&output[8], message->request_id);
    write_u16_le(&output[12], message->payload_size);
    memcpy(&output[BRIDGE_MANAGEMENT_HEADER_SIZE], message->payload, message->payload_size);
    *output_size = required;
    return true;
}

void bridge_session_init(
    BridgeSession* session,
    const uint8_t bridge_nonce[BRIDGE_NONCE_BYTES],
    const uint8_t session_id[BRIDGE_SESSION_ID_BYTES]) {
    if(!session || !bridge_nonce || !session_id) return;
    memset(session, 0, sizeof(*session));
    session->state = BridgeSessionAwaitingHello;
    memcpy(session->bridge_nonce, bridge_nonce, BRIDGE_NONCE_BYTES);
    memcpy(session->session_id, session_id, BRIDGE_SESSION_ID_BYTES);
}

bool bridge_session_accept_hello(
    BridgeSession* session,
    const uint8_t client_nonce[BRIDGE_NONCE_BYTES]) {
    if(!session || !client_nonce || session->state != BridgeSessionAwaitingHello) return false;
    memcpy(session->client_nonce, client_nonce, BRIDGE_NONCE_BYTES);
    session->state = BridgeSessionAwaitingAuthentication;
    return true;
}

bool bridge_session_mark_authenticated(BridgeSession* session) {
    if(!session || session->state != BridgeSessionAwaitingAuthentication) return false;
    session->state = BridgeSessionAuthenticated;
    return true;
}

bool bridge_session_authenticate(
    BridgeSession* session,
    const uint8_t expected[BRIDGE_HMAC_BYTES],
    const uint8_t provided[BRIDGE_HMAC_BYTES]) {
    if(!session || session->state != BridgeSessionAwaitingAuthentication ||
       !bridge_constant_time_equal(expected, provided, BRIDGE_HMAC_BYTES)) {
        return false;
    }
    return bridge_session_mark_authenticated(session);
}

bool bridge_session_begin_proxy(BridgeSession* session) {
    if(!session || session->state != BridgeSessionAuthenticated) return false;
    session->state = BridgeSessionProxying;
    return true;
}

void bridge_session_close(BridgeSession* session) {
    if(!session) return;
    memset(session->client_nonce, 0, sizeof(session->client_nonce));
    memset(session->bridge_nonce, 0, sizeof(session->bridge_nonce));
    memset(session->session_id, 0, sizeof(session->session_id));
    session->state = BridgeSessionClosed;
}

size_t bridge_session_auth_material(const BridgeSession* session, uint8_t* output, size_t capacity) {
    const size_t required = sizeof(auth_context) - 1U + 1U + BRIDGE_NONCE_BYTES * 2U +
                            BRIDGE_SESSION_ID_BYTES;
    if(!session || !output || capacity < required) return 0U;
    size_t offset = 0U;
    memcpy(&output[offset], auth_context, sizeof(auth_context) - 1U);
    offset += sizeof(auth_context) - 1U;
    output[offset++] = FLIPFORGE_BRIDGE_PROTOCOL_VERSION;
    memcpy(&output[offset], session->client_nonce, BRIDGE_NONCE_BYTES);
    offset += BRIDGE_NONCE_BYTES;
    memcpy(&output[offset], session->bridge_nonce, BRIDGE_NONCE_BYTES);
    offset += BRIDGE_NONCE_BYTES;
    memcpy(&output[offset], session->session_id, BRIDGE_SESSION_ID_BYTES);
    return required;
}

bool bridge_constant_time_equal(const uint8_t* lhs, const uint8_t* rhs, size_t size) {
    if(!lhs || !rhs) return false;
    uint8_t difference = 0U;
    for(size_t index = 0U; index < size; ++index) difference |= lhs[index] ^ rhs[index];
    return difference == 0U;
}

bool bridge_protocol_version_supported(uint8_t version) {
    return version == FLIPFORGE_BRIDGE_PROTOCOL_VERSION;
}
