#ifndef XTRA_FORMAT_GUARD_H
#define XTRA_FORMAT_GUARD_H

#include <stddef.h>
#include <stdint.h>

enum XtraFormatStatus {
    XTRA_FORMAT_INVALID,
    XTRA_FORMAT_GENERATION_1,
    XTRA_FORMAT_SUPPORTED
};

// The MSM8974 modem rejects generation 1 and clears loaded orbit data.
// Byte 0 is the container major and bytes 1 and 6 name the generation;
// bytes 2-5 change as the servers regenerate files, so they stay
// unchecked. Only a recognized generation 2 or 3 header reaches the modem.
static inline XtraFormatStatus classifyXtraFormat(const void* data, size_t length)
{
    if (data == NULL || length < 16) {
        return XTRA_FORMAT_INVALID;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (bytes[0] != 0x01) {
        return XTRA_FORMAT_INVALID;
    }

    if (bytes[1] == 0x1b && bytes[6] == 0x01) {
        return XTRA_FORMAT_GENERATION_1;
    }
    if ((bytes[1] == 0x34 && bytes[6] == 0x02) ||
        (bytes[1] == 0x32 && bytes[6] == 0x03)) {
        return XTRA_FORMAT_SUPPORTED;
    }
    return XTRA_FORMAT_INVALID;
}

#endif
