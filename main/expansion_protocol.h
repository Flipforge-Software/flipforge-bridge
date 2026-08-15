#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"

typedef enum {
    ExpansionFrameTypeHeartbeat = 1,
    ExpansionFrameTypeStatus = 2,
    ExpansionFrameTypeBaudRate = 3,
    ExpansionFrameTypeControl = 4,
    ExpansionFrameTypeData = 5,
} ExpansionFrameType;

typedef enum {
    ExpansionFrameErrorNone = 0x00,
    ExpansionFrameErrorUnknown = 0x01,
    ExpansionFrameErrorBaudRate = 0x02,
} ExpansionFrameError;

typedef enum {
    ExpansionControlStartRpc = 0x00,
    ExpansionControlStopRpc = 0x01,
} ExpansionControlCommand;

typedef struct {
    ExpansionFrameType type;
    union {
        ExpansionFrameError status;
        uint32_t baud_rate;
        ExpansionControlCommand control;
        struct {
            uint8_t size;
            uint8_t bytes[BRIDGE_EXPANSION_MAX_DATA];
        } data;
    } content;
} ExpansionFrame;

typedef enum {
    ExpansionDecodeNeedMore,
    ExpansionDecodeFrameReady,
    ExpansionDecodeMalformed,
    ExpansionDecodeChecksumError,
} ExpansionDecodeResult;

typedef struct {
    uint8_t bytes[BRIDGE_EXPANSION_MAX_DATA + 3U];
    size_t used;
    size_t expected;
} ExpansionDecoder;

uint8_t expansion_protocol_checksum(const uint8_t* bytes, size_t size);
bool expansion_protocol_encode(
    const ExpansionFrame* frame,
    uint8_t* output,
    size_t capacity,
    size_t* output_size);
void expansion_decoder_reset(ExpansionDecoder* decoder);
ExpansionDecodeResult expansion_decoder_push(
    ExpansionDecoder* decoder,
    uint8_t byte,
    ExpansionFrame* frame);
