#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "comment_view.h"
static int measured(const char *s, int n) { (void)s; return n == 1 ? 6 : 12; }
static int glyphs, max_y;
static void draw(int x, int y, const char *s, int n, void *ud)
{ (void)x; (void)s; (void)n; (void)ud; ++glyphs; if(y > max_y) max_y = y; }
int main(void)
{
    CommentList list = {0}; Comment reply_items[COMMENT_REPLY_MAX] = {{0}};
    CommentReplies replies = { .items = reply_items }; CommentView view = {0};
    int h;
    assert(comment_wrap("", 12, measured, NULL, NULL) == 1);
    assert(comment_wrap("ab\n\n", 12, measured, NULL, NULL) == 3);
    assert(comment_wrap("\xe4\xb8\xad\xe6\x96\x87" "ab", 18, measured, draw, NULL) == 3);
    assert(glyphs == 4 && max_y == 30);
    assert(comment_utf8_size("\xf0\x9f\x98\x80") == 4);
    assert(comment_utf8_size("\xe4") == 1);
    list.count = 2; list.items[0].id = 123;
    memset(list.items[0].text, 'a', sizeof(list.items[0].text)-1);
    strcpy(list.items[1].text, "short");
    h = comment_body_height(&list.items[0], 408, measured);
    assert(h > 200); /* Long content no longer disappears after two lines. */
    view.expanded = 123; replies.count = 1;
    strcpy(reply_items[0].text, "reply\nline2");
    assert(comment_row_height(&list.items[0], 123, &replies, 408, measured) == h + 20 + 66);
    assert(comment_at_pixel(&list, &view, &replies, 408, measured, 0) == 0);
    assert(comment_at_pixel(&list, &view, &replies, 408, measured, h + 86) == 1);
    assert(comment_scroll_clamp(-1, 500, 140) == 0);
    assert(comment_scroll_clamp(1000, 500, 140) == 360);
    puts("comments UTF-8 layout, height, pixel scrolling: PASS");
    return 0;
}
