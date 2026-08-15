#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef ptrdiff_t (*BridgeWriteCallback)(
    void* context,
    const uint8_t* bytes,
    size_t size,
    bool* retryable);

bool bridge_io_write_all(
    BridgeWriteCallback callback,
    void* context,
    const uint8_t* bytes,
    size_t size);
bool bridge_io_buffer_can_accept(size_t used, size_t incoming, size_t capacity);
