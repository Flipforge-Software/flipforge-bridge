#include "bridge_io.h"

bool bridge_io_write_all(
    BridgeWriteCallback callback,
    void* context,
    const uint8_t* bytes,
    size_t size) {
    if(!callback || (!bytes && size > 0U)) return false;
    size_t offset = 0U;
    while(offset < size) {
        bool retryable = false;
        const ptrdiff_t written = callback(context, bytes + offset, size - offset, &retryable);
        if(written > 0 && (size_t)written <= size - offset) {
            offset += (size_t)written;
        } else if(written < 0 && retryable) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool bridge_io_buffer_can_accept(size_t used, size_t incoming, size_t capacity) {
    return used <= capacity && incoming <= capacity - used;
}
