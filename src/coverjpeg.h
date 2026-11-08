/* Album-art CPU JPEG fallback. Does not use or change PSP ME/JPEG state. */
#ifndef MELODY_COVERJPEG_H
#define MELODY_COVERJPEG_H
#include <stddef.h>

#define COVERJPEG_MAX_SOURCE_DIM 1024
#define COVERJPEG_MAX_PIXELS (1024u * 1024u)
#define COVERJPEG_MAX_OUTPUT_DIM 192
#define COVERJPEG_MEMORY_LIMIT (4u * 1024u * 1024u)

/* Decode baseline/progressive JPEG, aspect fit without enlargement.
 * out_bytes must hold actual output width * height * 4 bytes, tightly packed.
 * max_dim: 1..192 (112 is supported). RGBA bytes, alpha 255.
 * Returns 0 success, -1 malformed/unsupported/invalid argument, -2 limit/OOM.
 * On failure neither out nor the returned dimensions is modified. Buffers must not overlap.
 * Source is bounded to 1024 per edge, 1M pixels and 2MiB compressed bytes.
 * The 4MiB allocation budget includes decoded coefficients and staging, plus
 * a conservative 128KiB reservation for libjpeg's bootstrap/context allocations.
 * Some maximum-size 4:4:4/CMYK progressive images exceed this budget: -2.
 * Link existing PSPSDK libjpeg-turbo using -ljpeg, without overriding its ABI.
 * Only call for album art, after the owner has quiesced active MV playback.
 */
int coverjpeg_decode_rgba_scaled(const unsigned char *jpg, int len,
                                unsigned char *out, size_t out_bytes,
                                int max_dim, int *ow, int *oh);
#endif
