#include "lyric_render.h"
#include "api.h"
#include "gfx.h"
#include <string.h>

int lyric_highlight_width(int font, const char *text, int index, int position_ms)
{
    int done = 0, end = 0, len, prefix, word_end;
    float fraction = 0;
    if (!text || !api_lyric_word_span(index, position_ms, &done, &end, &fraction))
        return -1;
    len = (int)strlen(text);
    if (done < 0) done = 0;
    if (done > len) done = len;
    if (end < done) end = done;
    if (end > len) end = len;
    if (!(fraction >= 0)) fraction = 0;
    if (fraction > 1) fraction = 1;
    prefix = gfx_text_w_n(font, text, done);
    word_end = gfx_text_w_n(font, text, end);
    return prefix + (int)((word_end - prefix) * fraction);
}

void lyric_draw_text(int x, int y, int width, const char *text, int font,
                     unsigned short base, unsigned short active, int line_index,
                     int position_ms, int byte_offset)
{
    const Lyric *ly = api_lyric();
    const char *original = text;
    int highlight, fragment_width, height = gfx_font_h(font);
    if (!text || width <= 0) return;
    if (ly && line_index >= 0 && line_index < ly->count)
        original = ly->text[line_index];
    highlight = lyric_highlight_width(font, original, line_index, position_ms);
    if (byte_offset < 0) byte_offset = 0;
    if (byte_offset > (int)strlen(original)) byte_offset = (int)strlen(original);
    if (highlight >= 0) {
        highlight -= gfx_text_w_n(font, original, byte_offset);
        if (highlight < 0) highlight = 0;
    }
    /* Base remains drawable even when an enclosing component exhausts clip scopes. */
    if (gfx_push_clip(x, y, x + width, y + height) < 0) {
        gfx_text_clip(font, x, y, width, text, highlight < 0 ? active : base);
        return;
    }
    gfx_text(font, x, y, text, highlight < 0 ? active : base);
    gfx_pop_clip();
    if (highlight <= 0) return;
    fragment_width = gfx_text_w(font, text);
    if (highlight > fragment_width) highlight = fragment_width;
    if (highlight > width) highlight = width;
    if (highlight <= 0 || gfx_push_clip(x, y, x + highlight, y + height) < 0) return;
    /* Redraw whole UTF-8 glyphs; the pixel clip reveals even half a glyph. */
    gfx_text(font, x, y, text, active);
    gfx_pop_clip();
}
