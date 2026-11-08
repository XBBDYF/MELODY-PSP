#ifndef MELODY_SKIN_ASSETS_H
#define MELODY_SKIN_ASSETS_H

/* Manifest: assets:[{"id":"wallpaper","file":"assets/wallpaper.png","type":"image"}].
 * Root is a trusted local skin directory supplied by the caller.
 * Validation/probe return 1 on success and 0 on failure; no assets is valid.
 * Image returns 1 drawn, 0 missing/deferred, -1 invalid argument/load failure.
 * Call begin_frame once before skin rendering; at most one new decode per frame.
 */
int skin_assets_validate_manifest(const char *root, const char *json, char *err, int n);
int skin_assets_probe(const char *root, char *err, int cap);
void skin_assets_set_root(const char *root);
void skin_assets_begin_frame(void);
int skin_assets_image(const char *id, int x, int y, int w, int h, int alpha);
void skin_assets_release(void);
unsigned skin_assets_decode_us(void);
unsigned skin_assets_memory_bytes(void);

/* Implemented by gfx: clipped RGBA source scaling with per-pixel/global alpha. */
void gfx_blit_rgba_scaled(int x, int y, int w, int h,
                          const unsigned char *rgba, int source_w, int source_h,
                          int alpha);
#endif
