/* CPU-only bounded album art decoder using the installed libjpeg-turbo.
 * No sceJpeg, ME imports, globals or video-state dependencies.
 */
#include "coverjpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <jerror.h>

#define BOOTSTRAP_RESERVE (128u * 1024u)
#define MAX_INPUT_BYTES (2u * 1024u * 1024u)

typedef union Allocation Allocation;
union Allocation {
    struct { Allocation *next; size_t bytes; int pool; } a;
    long double alignment;
    void *pointer_alignment;
};
struct jvirt_sarray_control { JSAMPARRAY rows; JDIMENSION count, access; };
struct jvirt_barray_control { JBLOCKARRAY rows; JDIMENSION count, access; };

typedef struct {
    struct jpeg_decompress_struct jpeg;
    struct jpeg_error_mgr error;
    struct jpeg_memory_mgr memory;
    struct jpeg_memory_mgr *original_memory;
    jmp_buf jump;
    Allocation *allocations;
    size_t used, peak, limit;
    int status;
} Cover;

#ifdef COVERJPEG_TEST
static size_t test_limit = COVERJPEG_MEMORY_LIMIT, test_peak;
size_t coverjpeg_test_peak(void) { return test_peak; }
void coverjpeg_test_limit(size_t limit) { test_limit = limit; }
#endif

static void fail(Cover *c, int status)
{
    c->status = status;
    longjmp(c->jump, 1);
}
static void on_error(j_common_ptr j)
{
    Cover *c = (Cover *)j;
    fail(c, j->err->msg_code == JERR_OUT_OF_MEMORY ? -2 : -1);
}
static void on_message(j_common_ptr j, int level)
{
    /* No repair/padded pixels from truncated or corrupt compressed input. */
    if (level < 0) fail((Cover *)j, -1);
}
static void quiet_message(j_common_ptr j) { (void)j; }

static void *allocate(j_common_ptr j, int pool, size_t bytes)
{
    Cover *c = (Cover *)j;
    Allocation *a;
    size_t total;
    if (pool < 0 || pool >= JPOOL_NUMPOOLS || bytes > c->limit ||
        bytes > (size_t)-1 - sizeof(*a)) fail(c, -2);
    total = bytes + sizeof(*a);
    if (c->used > c->limit || total > c->limit - c->used) fail(c, -2);
    a = (Allocation *)calloc(1, total);
    if (!a) fail(c, -2);
    a->a.next = c->allocations;
    a->a.bytes = total;
    a->a.pool = pool;
    c->allocations = a;
    c->used += total;
    if (c->used > c->peak) c->peak = c->used;
    return a + 1;
}
static size_t multiply(Cover *c, size_t x, size_t y)
{
    if (y && x > (size_t)-1 / y) fail(c, -2);
    return x * y;
}
static JSAMPARRAY samples(j_common_ptr j, int pool, JDIMENSION width, JDIMENSION height)
{
    Cover *c = (Cover *)j;
    JSAMPARRAY rows = (JSAMPARRAY)allocate(j, pool, multiply(c, height, sizeof(JSAMPROW)));
    JSAMPLE *data = (JSAMPLE *)allocate(j, pool, multiply(c, width, height));
    JDIMENSION y;
    for (y = 0; y < height; ++y) rows[y] = data + (size_t)y * width;
    return rows;
}
static JBLOCKARRAY blocks(j_common_ptr j, int pool, JDIMENSION width, JDIMENSION height)
{
    Cover *c = (Cover *)j;
    JBLOCKARRAY rows = (JBLOCKARRAY)allocate(j, pool, multiply(c, height, sizeof(JBLOCKROW)));
    JBLOCK *data = (JBLOCK *)allocate(j, pool, multiply(c, multiply(c, width, height), sizeof(JBLOCK)));
    JDIMENSION y;
    for (y = 0; y < height; ++y) rows[y] = data + (size_t)y * width;
    return rows;
}
static jvirt_sarray_ptr request_samples(j_common_ptr j, int pool, boolean zero,
                                        JDIMENSION width, JDIMENSION height, JDIMENSION access)
{
    jvirt_sarray_ptr v = (jvirt_sarray_ptr)allocate(j, pool, sizeof(*v));
    (void)zero; /* All allocations are zeroed, including progressive coefficients. */
    v->rows = samples(j, pool, width, height);
    v->count = height; v->access = access;
    return v;
}
static jvirt_barray_ptr request_blocks(j_common_ptr j, int pool, boolean zero,
                                      JDIMENSION width, JDIMENSION height, JDIMENSION access)
{
    jvirt_barray_ptr v = (jvirt_barray_ptr)allocate(j, pool, sizeof(*v));
    (void)zero;
    v->rows = blocks(j, pool, width, height);
    v->count = height; v->access = access;
    return v;
}
static void realize(j_common_ptr j) { (void)j; }
static JSAMPARRAY access_samples(j_common_ptr j, jvirt_sarray_ptr v,
                                 JDIMENSION start, JDIMENSION count, boolean writable)
{
    (void)writable;
    if (count > v->access || start > v->count || count > v->count - start)
        fail((Cover *)j, -1);
    return v->rows + start;
}
static JBLOCKARRAY access_blocks(j_common_ptr j, jvirt_barray_ptr v,
                                 JDIMENSION start, JDIMENSION count, boolean writable)
{
    (void)writable;
    if (count > v->access || start > v->count || count > v->count - start)
        fail((Cover *)j, -1);
    return v->rows + start;
}
static void free_pool(j_common_ptr j, int pool)
{
    Cover *c = (Cover *)j;
    Allocation **link = &c->allocations;
    while (*link) {
        Allocation *a = *link;
        if (a->a.pool == pool) {
            *link = a->a.next;
            c->used -= a->a.bytes;
            free(a);
        } else link = &a->a.next;
    }
}
static void destroy_memory(j_common_ptr j)
{
    Cover *c = (Cover *)j;
    free_pool(j, JPOOL_IMAGE);
    free_pool(j, JPOOL_PERMANENT);
    j->mem = c->original_memory;
    j->mem->self_destruct(j);
}
static void install_memory(Cover *c)
{
    c->original_memory = c->jpeg.mem;
    c->memory.alloc_small = allocate;
    c->memory.alloc_large = allocate;
    c->memory.alloc_sarray = samples;
    c->memory.alloc_barray = blocks;
    c->memory.request_virt_sarray = request_samples;
    c->memory.request_virt_barray = request_blocks;
    c->memory.realize_virt_arrays = realize;
    c->memory.access_virt_sarray = access_samples;
    c->memory.access_virt_barray = access_blocks;
    c->memory.free_pool = free_pool;
    c->memory.self_destruct = destroy_memory;
    c->memory.max_memory_to_use = (long)c->limit;
    c->memory.max_alloc_chunk = (long)c->limit;
    c->jpeg.mem = &c->memory;
}

/* Convert Adobe inverted CMYK/YCCK and ordinary CMYK to RGB. */
static unsigned int channel(const unsigned char *p, int component, int cmyk, int adobe)
{
    if (!cmyk) return p[component];
    if (adobe) return ((unsigned int)p[component] * p[3] + 127) / 255;
    return ((255u - p[component]) * (255u - p[3]) + 127) / 255;
}

int coverjpeg_decode_rgba_scaled(const unsigned char *jpg, int len,
                                unsigned char *out, size_t out_bytes,
                                int max_dim, int *ow, int *oh)
{
    Cover *c;
    int status;
    if (!jpg || !out || !ow || !oh || len < 4 || max_dim < 1 ||
        max_dim > COVERJPEG_MAX_OUTPUT_DIM || jpg[0] != 255 || jpg[1] != 216)
        return -1;
    if ((unsigned int)len > MAX_INPUT_BYTES) return -2;
    c = (Cover *)calloc(1, sizeof(*c));
    if (!c) return -2;
    c->limit = COVERJPEG_MEMORY_LIMIT;
#ifdef COVERJPEG_TEST
    c->limit = test_limit;
    test_peak = 0;
#endif
    c->used = c->peak = BOOTSTRAP_RESERVE;
    c->jpeg.err = jpeg_std_error(&c->error);
    c->error.error_exit = on_error;
    c->error.emit_message = on_message;
    c->error.output_message = quiet_message;
    if (setjmp(c->jump)) {
        status = c->status;
    } else {
        int w, h, sw, sh, longest, denominator, cmyk, components, adobe;
        unsigned char *raw, *stage;
        size_t required;
        int x, y;
        if (c->limit < BOOTSTRAP_RESERVE) fail(c, -2);
        jpeg_create_decompress(&c->jpeg);
        install_memory(c);
        jpeg_mem_src(&c->jpeg, jpg, (unsigned long)len);
        if (jpeg_read_header(&c->jpeg, TRUE) != JPEG_HEADER_OK) fail(c, -1);
        if (c->jpeg.image_width == 0 || c->jpeg.image_height == 0 ||
            c->jpeg.image_width > COVERJPEG_MAX_SOURCE_DIM ||
            c->jpeg.image_height > COVERJPEG_MAX_SOURCE_DIM ||
            (size_t)c->jpeg.image_width * c->jpeg.image_height > COVERJPEG_MAX_PIXELS)
            fail(c, -2);
        if (c->jpeg.data_precision != 8 ||
            (c->jpeg.num_components != 1 && c->jpeg.num_components != 3 &&
             c->jpeg.num_components != 4)) fail(c, -1);
        w = (int)c->jpeg.image_width; h = (int)c->jpeg.image_height;
        longest = w > h ? w : h;
        if (longest > max_dim) {
            w = w * max_dim / longest; h = h * max_dim / longest;
            if (!w) w = 1;
            if (!h) h = 1;
        }
        required = (size_t)w * h * 4;
        if (out_bytes < required) fail(c, -2);
        /* Choose the smallest native IDCT output still covering the target.
         * Actual output dimensions are read after jpeg_start_decompress. */
        denominator = 8;
        while (denominator > 1 && (longest + denominator - 1) / denominator <
               (w > h ? w : h)) denominator /= 2;
        c->jpeg.scale_num = 1; c->jpeg.scale_denom = denominator;
        cmyk = c->jpeg.jpeg_color_space == JCS_CMYK || c->jpeg.jpeg_color_space == JCS_YCCK;
        c->jpeg.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
        c->jpeg.dct_method = JDCT_ISLOW;
        c->jpeg.do_fancy_upsampling = TRUE;
        if (!jpeg_start_decompress(&c->jpeg)) fail(c, -1);
        sw = (int)c->jpeg.output_width; sh = (int)c->jpeg.output_height;
        components = c->jpeg.output_components;
        adobe = c->jpeg.saw_Adobe_marker;
        if (sw < w || sh < h || components != (cmyk ? 4 : 3)) fail(c, -1);
        raw = (unsigned char *)allocate((j_common_ptr)c, JPOOL_PERMANENT,
                                        (size_t)sw * sh * components);
        stage = (unsigned char *)allocate((j_common_ptr)c, JPOOL_PERMANENT, required);
        while (c->jpeg.output_scanline < c->jpeg.output_height) {
            JSAMPROW row = raw + (size_t)c->jpeg.output_scanline * sw * components;
            if (jpeg_read_scanlines(&c->jpeg, &row, 1) != 1) fail(c, -1);
        }
        if (!jpeg_finish_decompress(&c->jpeg)) fail(c, -1);
        /* Box resample using actual native-scaled dimensions. */
        for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
            int x0 = x * sw / w, x1 = (x + 1) * sw / w;
            int y0 = y * sh / h, y1 = (y + 1) * sh / h;
            unsigned int sums[3] = {0, 0, 0};
            unsigned int count = (unsigned int)((x1 - x0) * (y1 - y0));
            int xx, yy, k;
            unsigned char *dst = stage + ((size_t)y * w + x) * 4;
            for (yy = y0; yy < y1; ++yy) for (xx = x0; xx < x1; ++xx) {
                const unsigned char *p = raw + ((size_t)yy * sw + xx) * components;
                for (k = 0; k < 3; ++k) sums[k] += channel(p, k, cmyk, adobe);
            }
            for (k = 0; k < 3; ++k) dst[k] = (unsigned char)((sums[k] + count / 2) / count);
            dst[3] = 255;
        }
        memcpy(out, stage, required);
        *ow = w; *oh = h;
        status = 0;
    }
#ifdef COVERJPEG_TEST
    test_peak = c->peak;
#endif
    jpeg_destroy_decompress(&c->jpeg);
    free(c);
    return status;
}
