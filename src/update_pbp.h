#ifndef MELODY_UPDATE_PBP_H
#define MELODY_UPDATE_PBP_H

#include <stddef.h>

/* Header-only update structure check, not payload decoding/authentication.
 * Empty optional sections and arbitrary versions are accepted; DATA.PSP
 * must be nonempty. Offsets are absolute little-endian 32-bit values.
 */
static int update_pbp_valid(const unsigned char *header, size_t header_size,
                            unsigned long long file_bytes)
{
    unsigned long long previous = 40, data_psp = 0;
    size_t i;

    if (!header || header_size < 40 || file_bytes < 40 ||
        file_bytes > 0xffffffffULL)
        return 0;
    if (header[0] != 0 || header[1] != 'P' ||
        header[2] != 'B' || header[3] != 'P')
        return 0;

    for (i = 0; i < 8; ++i) {
        const unsigned char *p = header + 8 + i * 4;
        unsigned long long offset = (unsigned long long)p[0] |
            ((unsigned long long)p[1] << 8) |
            ((unsigned long long)p[2] << 16) |
            ((unsigned long long)p[3] << 24);
        if (offset < previous || offset > file_bytes)
            return 0;
        if (i == 6)
            data_psp = offset;
        if (i == 7 && offset == data_psp)
            return 0;
        previous = offset;
    }
    return 1;
}

#endif
