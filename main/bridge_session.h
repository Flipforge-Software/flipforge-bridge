#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"

#define BRIDGE_MANAGEMENT_MAGIC "FBRG"
#define BRIDGE_MANAGEMENT_RESPONSE_FLAG 0x01U

typedef enum {
    BridgeCommandHello = 0x01,
    BridgeCommandAuthenticate = 0x02,
    BridgeCommandGetBridgeInfo = 0x03,
    BridgeCommandGetStatus = 0x04,
    BridgeCommandBeginRpcProxy = 0x05,
    BridgeCommandEndRpcProxy = 0x06,
    BridgeCommandPing = 0x07,
} BridgeCommand;

typedef enum {
    BridgeStatusOk = 0,
    BridgeStatusMalformed = 1,
    BridgeStatusUnsupportedProtocol = 2,
    BridgeStatusUnauthorized = 3,
    BridgeStatusBusy = 4,
    BridgeStatusUnavailable = 5,
    BridgeStatusTooLarge = 6,
    BridgeStatusInvalidState = 7,
} BridgeStatus;

typedef struct {
    uint8_t version;
    BridgeCommand command;
    uint8_t flags;
    BridgeStatus status;
    uint32_t request_id;
    uint16_t payload_size;
    uint8_t payload[BRIDGE_MANAGEMENT_MAX_PAYLOAD];
} BridgeMessage;

typedef struct {
    uint8_t bytes[BRIDGE_MANAGEMENT_HEADER_SIZE + BRIDGE_MANAGEMENT_MAX_PAYLOAD];
    size_t used;
    size_t expected;
} BridgeMessageDecoder;

typedef enum {
    BridgeDecodeNeedMore,
    BridgeDecodeReady,
    BridgeDecodeMalformed,
    BridgeDecodeTooLarge,
} BridgeMessageDecodeResult;

typedef enum {
    BridgeSessionAwaitingHello,
    BridgeSessionAwaitingAuthentication,
    BridgeSessionAuthenticated,
    BridgeSessionProxying,
    BridgeSessionClosed,
} BridgeSessionState;

typedef struct {
    BridgeSessionState state;
    uint8_t client_nonce[BRIDGE_NONCE_BYTES];
    uint8_t bridge_nonce[BRIDGE_NONCE_BYTES];
    uint8_t session_id[BRIDGE_SESSION_ID_BYTES];
} BridgeSession;

void bridge_message_decoder_reset(BridgeMessageDecoder* decoder);
BridgeMessageDecodeResult bridge_message_decoder_push(
    BridgeMessageDecoder* decoder,
    const uint8_t* bytes,
    size_t size,
    size_t* consumed,
    BridgeMessage* message);
bool bridge_message_encode(
    const BridgeMessage* message,
    uint8_t* output,
    size_t capacity,
    size_t* output_size);
void bridge_session_init(
    BridgeSession* session,
    const uint8_t bridge_nonce[BRIDGE_NONCE_BYTES],
    const uint8_t session_id[BRIDGE_SESSION_ID_BYTES]);
bool bridge_session_accept_hello(
    BridgeSession* session,
    const uint8_t client_nonce[BRIDGE_NONCE_BYTES]);
bool bridge_session_mark_authenticated(BridgeSession* session);
bool bridge_session_authenticate(
    BridgeSession* session,
    const uint8_t expected[BRIDGE_HMAC_BYTES],
    const uint8_t provided[BRIDGE_HMAC_BYTES]);
bool bridge_session_begin_proxy(BridgeSession* session);
void bridge_session_close(BridgeSession* session);
size_t bridge_session_auth_material(const BridgeSession* session, uint8_t* output, size_t capacity);
bool bridge_constant_time_equal(const uint8_t* lhs, const uint8_t* rhs, size_t size);
bool bridge_protocol_version_supported(uint8_t version);
