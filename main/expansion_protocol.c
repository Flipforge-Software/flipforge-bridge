#include "expansion_protocol.h"

#include <string.h>

static void write_u32_le(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint32_t read_u32_le(const uint8_t* input) {
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

uint8_t expansion_protocol_checksum(const uint8_t* bytes, size_t size) {
    uint8_t checksum = 0U;
    if(!bytes) return checksum;
    for(size_t index = 0; index < size; ++index) checksum ^= bytes[index];
    return checksum;
}

bool expansion_protocol_encode(
    const ExpansionFrame* frame,
    uint8_t* output,
    size_t capacity,
    size_t* output_size) {
    if(!frame || !output || !output_size || capacity < 2U) return false;

    size_t body_size = 0U;
    output[0] = (uint8_t)frame->type;
    switch(frame->type) {
    case ExpansionFrameTypeHeartbeat:
        body_size = 1U;
        break;
    case ExpansionFrameTypeStatus:
        body_size = 2U;
        if(capacity < body_size + 1U) return false;
        output[1] = (uint8_t)frame->content.status;
        break;
    case ExpansionFrameTypeBaudRate:
        body_size = 5U;
        if(capacity < body_size + 1U) return false;
        write_u32_le(&output[1], frame->content.baud_rate);
        break;
    case ExpansionFrameTypeControl:
        body_size = 2U;
        if(capacity < body_size + 1U || frame->content.control > ExpansionControlStopRpc) {
            return false;
        }
        output[1] = (uint8_t)frame->content.control;
        break;
    case ExpansionFrameTypeData:
        if(frame->content.data.size > BRIDGE_EXPANSION_MAX_DATA) return false;
        body_size = 2U + frame->content.data.size;
        if(capacity < body_size + 1U) return false;
        output[1] = frame->content.data.size;
        memcpy(&output[2], frame->content.data.bytes, frame->content.data.size);
        break;
    default:
        return false;
    }
    if(capacity < body_size + 1U) return false;
    output[body_size] = expansion_protocol_checksum(output, body_size);
    *output_size = body_size + 1U;
    return true;
}

void expansion_decoder_reset(ExpansionDecoder* decoder) {
    if(decoder) memset(decoder, 0, sizeof(*decoder));
}

static bool decoder_set_expected(ExpansionDecoder* decoder) {
    if(decoder->used == 1U) {
        switch(decoder->bytes[0]) {
        case ExpansionFrameTypeHeartbeat:
            decoder->expected = 2U;
            break;
        case ExpansionFrameTypeStatus:
        case ExpansionFrameTypeControl:
            decoder->expected = 3U;
            break;
        case ExpansionFrameTypeBaudRate:
            decoder->expected = 6U;
            break;
        case ExpansionFrameTypeData:
            decoder->expected = 0U;
            break;
        default:
            return false;
        }
    } else if(decoder->used == 2U && decoder->bytes[0] == ExpansionFrameTypeData) {
        if(decoder->bytes[1] > BRIDGE_EXPANSION_MAX_DATA) return false;
        decoder->expected = 3U + decoder->bytes[1];
    }
    return true;
}

ExpansionDecodeResult expansion_decoder_push(
    ExpansionDecoder* decoder,
    uint8_t byte,
    ExpansionFrame* frame) {
    if(!decoder || !frame || decoder->used >= sizeof(decoder->bytes)) {
        if(decoder) expansion_decoder_reset(decoder);
        return ExpansionDecodeMalformed;
    }
    decoder->bytes[decoder->used++] = byte;
    if(!decoder_set_expected(decoder)) {
        expansion_decoder_reset(decoder);
        return ExpansionDecodeMalformed;
    }
    if(decoder->expected == 0U || decoder->used < decoder->expected) {
        return ExpansionDecodeNeedMore;
    }
    if(decoder->used != decoder->expected) {
        expansion_decoder_reset(decoder);
        return ExpansionDecodeMalformed;
    }

    const size_t body_size = decoder->expected - 1U;
    if(expansion_protocol_checksum(decoder->bytes, body_size) != decoder->bytes[body_size]) {
        expansion_decoder_reset(decoder);
        return ExpansionDecodeChecksumError;
    }

    memset(frame, 0, sizeof(*frame));
    frame->type = (ExpansionFrameType)decoder->bytes[0];
    switch(frame->type) {
    case ExpansionFrameTypeHeartbeat:
        break;
    case ExpansionFrameTypeStatus:
        if(decoder->bytes[1] > ExpansionFrameErrorBaudRate) {
            expansion_decoder_reset(decoder);
            return ExpansionDecodeMalformed;
        }
        frame->content.status = (ExpansionFrameError)decoder->bytes[1];
        break;
    case ExpansionFrameTypeBaudRate:
        frame->content.baud_rate = read_u32_le(&decoder->bytes[1]);
        break;
    case ExpansionFrameTypeControl:
        if(decoder->bytes[1] > ExpansionControlStopRpc) {
            expansion_decoder_reset(decoder);
            return ExpansionDecodeMalformed;
        }
        frame->content.control = (ExpansionControlCommand)decoder->bytes[1];
        break;
    case ExpansionFrameTypeData:
        frame->content.data.size = decoder->bytes[1];
        memcpy(frame->content.data.bytes, &decoder->bytes[2], frame->content.data.size);
        break;
    default:
        expansion_decoder_reset(decoder);
        return ExpansionDecodeMalformed;
    }
    expansion_decoder_reset(decoder);
    return ExpansionDecodeFrameReady;
}
