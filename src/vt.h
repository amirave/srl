/* vt.h - small VT100/xterm-subset terminal emulator (screen model only). */
#ifndef VT_H
#define VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VT_WIDE_CONT 0xFFFFFFFFu /* right half of a double-width char */

enum { VT_BOLD = 1, VT_DIM = 2, VT_ITALIC = 4, VT_UNDERLINE = 8, VT_BLINK = 16, VT_REVERSE = 32 };

typedef struct {
    uint32_t ch;
    int16_t fg, bg; /* -1 = default, 0..255 */
    uint8_t attr;
} vt_cell;

typedef struct vt vt;
typedef void (*vt_reply_fn)(const char *buf, size_t len, void *ctx);

vt *vt_new(int rows, int cols, vt_reply_fn reply, void *ctx);
void vt_free(vt *t);
void vt_resize(vt *t, int rows, int cols);
void vt_write(vt *t, const char *buf, size_t len);
void vt_reset(vt *t);

int vt_rows(const vt *t);
int vt_cols(const vt *t);
/* visible row r (0..rows-1), taking the scrollback view offset into account.
 * Returns NULL past the end of a (shorter) scrollback line. */
const vt_cell *vt_cell_at(const vt *t, int r, int c);
bool vt_cursor(const vt *t, int *row, int *col); /* false if hidden or scrolled back */
bool vt_app_cursor_keys(const vt *t);

void vt_scroll_view(vt *t, int delta); /* +up into history, -down */
void vt_scroll_reset(vt *t);
int vt_scroll_offset(const vt *t);

#endif
