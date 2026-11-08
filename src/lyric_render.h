#ifndef MELODY_LYRIC_RENDER_H
#define MELODY_LYRIC_RENDER_H

/* Returns the real YRC highlighted pixel width, or -1 without word timing. */
int lyric_highlight_width(int font, const char *text, int index, int position_ms);
/* byte_offset is the UTF-8 byte offset of this wrapped fragment in the full line.
 * Untimed lines use active; timed lines use base plus a pixel-clipped overlay. */
void lyric_draw_text(int x, int y, int width, const char *text, int font,
                     unsigned short base, unsigned short active, int line_index,
                     int position_ms, int byte_offset);

#endif
