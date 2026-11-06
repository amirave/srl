/* vt.c - small VT100/xterm-subset terminal emulator.
 *
 * Handles: UTF-8, CR/LF/BS/TAB, cursor movement, erase, insert/delete,
 * scroll regions, SGR (16/256/truecolor), alternate screen, DEC line
 * drawing, save/restore cursor, autowrap, cursor visibility, application
 * cursor keys, DSR/DA replies, and a scrollback buffer.
 */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SB_MAX 5000
#define MAXP 32

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET, S_SKIP1 };

typedef struct {
    vt_cell *cells;
    int len;
} sb_line;

typedef struct {
    int cx, cy;
    int16_t fg, bg;
    uint8_t attr;
    bool wrapnext;
    char g[2];
    int gl;
} cursor_state;

struct vt {
    int rows, cols;
    vt_cell *main, *alt, *scr;
    bool on_alt;

    int cx, cy;
    bool wrapnext;
    int16_t fg, bg;
    uint8_t attr;
    char g[2]; /* 'B' ascii, '0' DEC graphics */
    int gl;    /* active charset index */
    int top, bot;
    bool autowrap, cursor_visible, app_cursor, insert;
    cursor_state saved, saved_alt;

    /* parser */
    int state;
    int params[MAXP];
    int np;
    bool have_param;
    char priv, inter;
    int charset_slot;
    uint32_t u8cp;
    int u8need;

    sb_line *sb;
    int sb_start, sb_count;
    int view;

    vt_reply_fn reply;
    void *ctx;
};

/* ------------------------------------------------------------------ */

static vt_cell blank(const vt *t)
{
    vt_cell c = { ' ', -1, t->bg, 0 };
    return c;
}

static vt_cell *row(vt *t, int r) { return t->scr + (size_t)r * t->cols; }

static void clear_cells(vt *t, int r, int c0, int c1) /* [c0, c1) */
{
    vt_cell b = blank(t), *p = row(t, r);
    for (int c = c0; c < c1 && c < t->cols; c++)
        p[c] = b;
}

static void clamp_cursor(vt *t)
{
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void sb_push(vt *t, const vt_cell *cells, int len)
{
    sb_line *slot;
    if (t->sb_count < SB_MAX) {
        slot = &t->sb[(t->sb_start + t->sb_count) % SB_MAX];
        t->sb_count++;
    } else {
        slot = &t->sb[t->sb_start];
        free(slot->cells);
        t->sb_start = (t->sb_start + 1) % SB_MAX;
    }
    /* trim trailing blanks */
    while (len > 0 && cells[len - 1].ch == ' ' && cells[len - 1].bg == -1 && !cells[len - 1].attr)
        len--;
    slot->cells = malloc(sizeof(vt_cell) * (size_t)(len ? len : 1));
    if (len)
        memcpy(slot->cells, cells, sizeof(vt_cell) * (size_t)len);
    slot->len = len;
    if (t->view > 0 && t->view < t->sb_count)
        t->view++; /* keep the user's scrolled-back view anchored */
}

static void scroll_up_h(vt *t, int top, int bot, int n, bool history)
{
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    if (history && !t->on_alt && top == 0)
        for (int i = 0; i < n; i++)
            sb_push(t, row(t, top + i), t->cols);
    memmove(row(t, top), row(t, top + n), sizeof(vt_cell) * (size_t)t->cols * (size_t)(bot - top + 1 - n));
    for (int r = bot - n + 1; r <= bot; r++)
        clear_cells(t, r, 0, t->cols);
}

static void scroll_up(vt *t, int top, int bot, int n) { scroll_up_h(t, top, bot, n, true); }

static void scroll_down(vt *t, int top, int bot, int n)
{
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(row(t, top + n), row(t, top), sizeof(vt_cell) * (size_t)t->cols * (size_t)(bot - top + 1 - n));
    for (int r = top; r < top + n; r++)
        clear_cells(t, r, 0, t->cols);
}

static void linefeed(vt *t)
{
    t->wrapnext = false;
    if (t->cy == t->bot)
        scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1)
        t->cy++;
}

static void reverse_index(vt *t)
{
    t->wrapnext = false;
    if (t->cy == t->top)
        scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0)
        t->cy--;
}

static uint32_t dec_graphics(uint32_t c)
{
    static const uint32_t map[] = {
        /* 0x60 - 0x7e */
        0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
        0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
        0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
        0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
    };
    if (c >= 0x60 && c <= 0x7e)
        return map[c - 0x60];
    return c;
}

static void put_char(vt *t, uint32_t cp)
{
    if (t->g[t->gl] == '0')
        cp = dec_graphics(cp);
    int w = wcwidth((wchar_t)cp);
    if (w < 0) w = 1;
    if (w == 0) return; /* combining marks: dropped */

    if (t->wrapnext && t->autowrap) {
        t->cx = 0;
        linefeed(t);
    }
    t->wrapnext = false;
    if (w == 2 && t->cx == t->cols - 1) {
        if (t->autowrap) {
            clear_cells(t, t->cy, t->cx, t->cols);
            t->cx = 0;
            linefeed(t);
        } else {
            w = 1;
        }
    }
    vt_cell *p = row(t, t->cy);
    if (t->insert && t->cx + w < t->cols)
        memmove(p + t->cx + w, p + t->cx, sizeof(vt_cell) * (size_t)(t->cols - t->cx - w));
    /* overwriting half of a wide char: blank the other half */
    if (p[t->cx].ch == VT_WIDE_CONT && t->cx > 0)
        p[t->cx - 1].ch = ' ';
    vt_cell c = { cp, t->fg, t->bg, t->attr };
    p[t->cx] = c;
    if (w == 2 && t->cx + 1 < t->cols) {
        c.ch = VT_WIDE_CONT;
        p[t->cx + 1] = c;
    }
    t->cx += w;
    if (t->cx >= t->cols) {
        t->cx = t->cols - 1;
        t->wrapnext = true;
    }
}

static void save_cursor(vt *t, cursor_state *s)
{
    s->cx = t->cx; s->cy = t->cy; s->fg = t->fg; s->bg = t->bg; s->attr = t->attr;
    s->wrapnext = t->wrapnext; s->g[0] = t->g[0]; s->g[1] = t->g[1]; s->gl = t->gl;
}

static void restore_cursor(vt *t, const cursor_state *s)
{
    t->cx = s->cx; t->cy = s->cy; t->fg = s->fg; t->bg = s->bg; t->attr = s->attr;
    t->wrapnext = s->wrapnext; t->g[0] = s->g[0]; t->g[1] = s->g[1]; t->gl = s->gl;
    clamp_cursor(t);
}

static void set_alt(vt *t, bool on, bool save_restore, bool clear)
{
    if (on == t->on_alt) {
        if (on && clear)
            for (int r = 0; r < t->rows; r++) clear_cells(t, r, 0, t->cols);
        return;
    }
    if (on) {
        if (save_restore) save_cursor(t, &t->saved_alt);
        t->scr = t->alt;
        t->on_alt = true;
        if (clear)
            for (int r = 0; r < t->rows; r++) clear_cells(t, r, 0, t->cols);
    } else {
        t->scr = t->main;
        t->on_alt = false;
        if (save_restore) restore_cursor(t, &t->saved_alt);
    }
    t->view = 0;
}

static int16_t rgb_to_256(int r, int g, int b)
{
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 238) return 231;
        return (int16_t)(232 + (r - 8) / 10);
    }
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
    return (int16_t)(16 + 36 * ri + 6 * gi + bi);
}

static int P(const vt *t, int i, int def)
{
    return (i < t->np && t->params[i] > 0) ? t->params[i] : def;
}

static void sgr(vt *t)
{
    if (t->np == 0) {
        t->fg = t->bg = -1;
        t->attr = 0;
        return;
    }
    for (int i = 0; i < t->np; i++) {
        int p = t->params[i];
        switch (p) {
        case 0: t->fg = t->bg = -1; t->attr = 0; break;
        case 1: t->attr |= VT_BOLD; break;
        case 2: t->attr |= VT_DIM; break;
        case 3: t->attr |= VT_ITALIC; break;
        case 4: t->attr |= VT_UNDERLINE; break;
        case 5: case 6: t->attr |= VT_BLINK; break;
        case 7: t->attr |= VT_REVERSE; break;
        case 21: case 22: t->attr &= (uint8_t)~(VT_BOLD | VT_DIM); break;
        case 23: t->attr &= (uint8_t)~VT_ITALIC; break;
        case 24: t->attr &= (uint8_t)~VT_UNDERLINE; break;
        case 25: t->attr &= (uint8_t)~VT_BLINK; break;
        case 27: t->attr &= (uint8_t)~VT_REVERSE; break;
        case 39: t->fg = -1; break;
        case 49: t->bg = -1; break;
        case 38: case 48: {
            int16_t col = -1;
            if (i + 2 < t->np && t->params[i + 1] == 5) {
                col = (int16_t)(t->params[i + 2] & 255);
                i += 2;
            } else if (i + 4 < t->np && t->params[i + 1] == 2) {
                col = rgb_to_256(t->params[i + 2], t->params[i + 3], t->params[i + 4]);
                i += 4;
            } else {
                i = t->np;
                break;
            }
            if (p == 38) t->fg = col; else t->bg = col;
            break;
        }
        default:
            if (p >= 30 && p <= 37) t->fg = (int16_t)(p - 30);
            else if (p >= 40 && p <= 47) t->bg = (int16_t)(p - 40);
            else if (p >= 90 && p <= 97) t->fg = (int16_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) t->bg = (int16_t)(p - 100 + 8);
        }
    }
}

static void reply(vt *t, const char *s)
{
    if (t->reply)
        t->reply(s, strlen(s), t->ctx);
}

static void set_mode(vt *t, bool on)
{
    for (int i = 0; i < (t->np ? t->np : 1); i++) {
        int m = t->np ? t->params[i] : 0;
        if (t->priv == '?') {
            switch (m) {
            case 1: t->app_cursor = on; break;
            case 7: t->autowrap = on; break;
            case 25: t->cursor_visible = on; break;
            case 47: case 1047: set_alt(t, on, false, on && m == 1047); break;
            case 1048: if (on) save_cursor(t, &t->saved); else restore_cursor(t, &t->saved); break;
            case 1049: set_alt(t, on, true, true); break;
            default: break; /* mouse, bracketed paste, ... ignored */
            }
        } else if (!t->priv && m == 4) {
            t->insert = on;
        }
    }
}

static void csi_dispatch(vt *t, char f)
{
    if (t->inter) {
        if (t->inter == '!' && f == 'p') { /* DECSTR soft reset */
            t->fg = t->bg = -1; t->attr = 0; t->insert = false; t->autowrap = true;
            t->cursor_visible = true; t->app_cursor = false; t->top = 0; t->bot = t->rows - 1;
        }
        return;
    }
    if (t->priv && f != 'h' && f != 'l' && f != 'c' && f != 'n')
        return;

    int n = P(t, 0, 1);
    vt_cell *p = row(t, t->cy);
    t->wrapnext = false;
    switch (f) {
    case '@': {
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(p + t->cx + n, p + t->cx, sizeof(vt_cell) * (size_t)(t->cols - t->cx - n));
        clear_cells(t, t->cy, t->cx, t->cx + n);
        break;
    }
    case 'A': { int lim = t->cy >= t->top ? t->top : 0; t->cy -= n; if (t->cy < lim) t->cy = lim; break; }
    case 'B': case 'e': { int lim = t->cy <= t->bot ? t->bot : t->rows - 1; t->cy += n; if (t->cy > lim) t->cy = lim; break; }
    case 'C': case 'a': t->cx += n; break;
    case 'D': t->cx -= n; break;
    case 'E': { int lim = t->cy <= t->bot ? t->bot : t->rows - 1; t->cy += n; if (t->cy > lim) t->cy = lim; t->cx = 0; break; }
    case 'F': { int lim = t->cy >= t->top ? t->top : 0; t->cy -= n; if (t->cy < lim) t->cy = lim; t->cx = 0; break; }
    case 'G': case '`': t->cx = n - 1; break;
    case 'd': t->cy = n - 1; break;
    case 'H': case 'f': t->cy = P(t, 0, 1) - 1; t->cx = P(t, 1, 1) - 1; break;
    case 'I': for (int i = 0; i < n; i++) t->cx = (t->cx / 8 + 1) * 8; break;
    case 'Z': for (int i = 0; i < n; i++) t->cx = t->cx > 0 ? ((t->cx - 1) / 8) * 8 : 0; break;
    case 'J': {
        int m = t->np ? t->params[0] : 0;
        if (m == 0) {
            clear_cells(t, t->cy, t->cx, t->cols);
            for (int r = t->cy + 1; r < t->rows; r++) clear_cells(t, r, 0, t->cols);
        } else if (m == 1) {
            for (int r = 0; r < t->cy; r++) clear_cells(t, r, 0, t->cols);
            clear_cells(t, t->cy, 0, t->cx + 1);
        } else if (m == 2) {
            for (int r = 0; r < t->rows; r++) clear_cells(t, r, 0, t->cols);
        } else if (m == 3 && !t->on_alt) {
            for (int i = 0; i < t->sb_count; i++) free(t->sb[(t->sb_start + i) % SB_MAX].cells);
            t->sb_count = t->sb_start = t->view = 0;
        }
        break;
    }
    case 'K': {
        int m = t->np ? t->params[0] : 0;
        if (m == 0) clear_cells(t, t->cy, t->cx, t->cols);
        else if (m == 1) clear_cells(t, t->cy, 0, t->cx + 1);
        else clear_cells(t, t->cy, 0, t->cols);
        break;
    }
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) { scroll_down(t, t->cy, t->bot, n); t->cx = 0; } break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) { scroll_up_h(t, t->cy, t->bot, n, false); t->cx = 0; } break;
    case 'P': {
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(p + t->cx, p + t->cx + n, sizeof(vt_cell) * (size_t)(t->cols - t->cx - n));
        clear_cells(t, t->cy, t->cols - n, t->cols);
        break;
    }
    case 'X': clear_cells(t, t->cy, t->cx, t->cx + n); break;
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'b': /* REP: repeat previous char - approximate with the cell to the left */
        if (t->cx > 0) { uint32_t ch = p[t->cx - 1].ch; for (int i = 0; i < n && ch != VT_WIDE_CONT; i++) put_char(t, ch); }
        break;
    case 'm': sgr(t); break;
    case 'r': {
        int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) { t->top = top; t->bot = bot; t->cx = 0; t->cy = 0; }
        break;
    }
    case 's': save_cursor(t, &t->saved); break;
    case 'u': restore_cursor(t, &t->saved); break;
    case 'h': set_mode(t, true); break;
    case 'l': set_mode(t, false); break;
    case 'n':
        if (!t->priv && t->np && t->params[0] == 6) {
            char buf[32];
            snprintf(buf, sizeof buf, "\033[%d;%dR", t->cy + 1, t->cx + 1);
            reply(t, buf);
        } else if (!t->priv && t->np && t->params[0] == 5) {
            reply(t, "\033[0n");
        }
        break;
    case 'c':
        if (!t->priv && P(t, 0, 0) == 0) reply(t, "\033[?62;22c");
        else if (t->priv == '>') reply(t, "\033[>1;10;0c");
        break;
    default: break;
    }
    clamp_cursor(t);
}

static void do_reset(vt *t)
{
    t->scr = t->main;
    t->on_alt = false;
    t->cx = t->cy = 0;
    t->wrapnext = false;
    t->fg = t->bg = -1;
    t->attr = 0;
    t->g[0] = t->g[1] = 'B';
    t->gl = 0;
    t->top = 0;
    t->bot = t->rows - 1;
    t->autowrap = t->cursor_visible = true;
    t->app_cursor = t->insert = false;
    t->state = S_GROUND;
    t->u8need = 0;
    save_cursor(t, &t->saved);
    save_cursor(t, &t->saved_alt);
    for (int r = 0; r < t->rows; r++) {
        t->scr = t->alt; clear_cells(t, r, 0, t->cols);
        t->scr = t->main; clear_cells(t, r, 0, t->cols);
    }
}

static void esc_dispatch(vt *t, char c)
{
    t->state = S_GROUND;
    switch (c) {
    case '[': t->state = S_CSI; t->np = 0; t->have_param = false; t->priv = 0; t->inter = 0;
              memset(t->params, 0, sizeof t->params); break;
    case ']': case 'P': case '_': case '^': case 'X': t->state = S_OSC; break;
    case '(': t->state = S_CHARSET; t->charset_slot = 0; break;
    case ')': t->state = S_CHARSET; t->charset_slot = 1; break;
    case '*': case '+': case '-': case '.': t->state = S_CHARSET; t->charset_slot = -1; break;
    case '#': case '%': case ' ': t->state = S_SKIP1; break;
    case '7': save_cursor(t, &t->saved); break;
    case '8': restore_cursor(t, &t->saved); break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M': reverse_index(t); break;
    case 'c': do_reset(t); break;
    default: break; /* '=', '>', etc */
    }
}

static void control(vt *t, unsigned char c)
{
    switch (c) {
    case 0x08: if (t->cx > 0) t->cx--; t->wrapnext = false; break;
    case 0x09: t->cx = (t->cx / 8 + 1) * 8; if (t->cx >= t->cols) t->cx = t->cols - 1; break;
    case 0x0a: case 0x0b: case 0x0c: linefeed(t); break;
    case 0x0d: t->cx = 0; t->wrapnext = false; break;
    case 0x0e: t->gl = 1; break;
    case 0x0f: t->gl = 0; break;
    case 0x18: case 0x1a: t->state = S_GROUND; break;
    case 0x1b: t->state = S_ESC; break;
    default: break;
    }
}

static void feed_cp(vt *t, uint32_t cp)
{
    switch (t->state) {
    case S_GROUND:
        if (cp < 0x20 || cp == 0x7f) control(t, (unsigned char)cp);
        else if (cp >= 0x80 && cp < 0xa0) { /* C1: ignore */ }
        else put_char(t, cp);
        break;
    case S_ESC:
        if (cp < 0x20) control(t, (unsigned char)cp);
        else esc_dispatch(t, (char)cp);
        break;
    case S_CSI:
        if (cp == 0x1b) { t->state = S_ESC; break; }
        if (cp < 0x20) { control(t, (unsigned char)cp); break; }
        if (cp >= '0' && cp <= '9') {
            if (t->np == 0) t->np = 1;
            int *v = &t->params[t->np - 1];
            if (*v < 100000) *v = *v * 10 + (int)(cp - '0');
        } else if (cp == ';' || cp == ':') {
            if (t->np == 0) t->np = 1;
            if (t->np < MAXP) t->params[t->np++] = 0;
        } else if (cp >= '<' && cp <= '?') {
            t->priv = (char)cp;
        } else if (cp >= 0x20 && cp <= 0x2f) {
            t->inter = (char)cp;
        } else if (cp >= 0x40 && cp <= 0x7e) {
            t->state = S_GROUND;
            csi_dispatch(t, (char)cp);
        } else {
            t->state = S_GROUND;
        }
        break;
    case S_OSC:
        if (cp == 0x07) t->state = S_GROUND;
        else if (cp == 0x1b) t->state = S_OSC_ESC;
        break;
    case S_OSC_ESC:
        t->state = cp == '\\' ? S_GROUND : S_OSC;
        break;
    case S_CHARSET:
        if (t->charset_slot >= 0) t->g[t->charset_slot] = cp == '0' ? '0' : 'B';
        t->state = S_GROUND;
        break;
    case S_SKIP1:
        t->state = S_GROUND;
        break;
    }
}

void vt_write(vt *t, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)buf[i];
        if (t->u8need) {
            if ((b & 0xc0) == 0x80) {
                t->u8cp = (t->u8cp << 6) | (b & 0x3f);
                if (--t->u8need == 0)
                    feed_cp(t, t->u8cp);
                continue;
            }
            t->u8need = 0;
            feed_cp(t, 0xfffd);
        }
        if (b < 0x80) feed_cp(t, b);
        else if ((b & 0xe0) == 0xc0) { t->u8cp = b & 0x1f; t->u8need = 1; }
        else if ((b & 0xf0) == 0xe0) { t->u8cp = b & 0x0f; t->u8need = 2; }
        else if ((b & 0xf8) == 0xf0) { t->u8cp = b & 0x07; t->u8need = 3; }
        else feed_cp(t, 0xfffd);
    }
}

/* ------------------------------------------------------------------ */

static vt_cell *alloc_grid(int rows, int cols)
{
    vt_cell *g = malloc(sizeof(vt_cell) * (size_t)rows * (size_t)cols);
    if (!g) abort();
    vt_cell b = { ' ', -1, -1, 0 };
    for (size_t i = 0; i < (size_t)rows * (size_t)cols; i++) g[i] = b;
    return g;
}

vt *vt_new(int rows, int cols, vt_reply_fn reply_fn, void *ctx)
{
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    vt *t = calloc(1, sizeof *t);
    if (!t) abort();
    t->rows = rows;
    t->cols = cols;
    t->main = alloc_grid(rows, cols);
    t->alt = alloc_grid(rows, cols);
    t->sb = calloc(SB_MAX, sizeof *t->sb);
    if (!t->sb) abort();
    t->reply = reply_fn;
    t->ctx = ctx;
    do_reset(t);
    return t;
}

void vt_reset(vt *t)
{
    for (int i = 0; i < t->sb_count; i++) free(t->sb[(t->sb_start + i) % SB_MAX].cells);
    t->sb_count = t->sb_start = t->view = 0;
    do_reset(t);
}

void vt_free(vt *t)
{
    if (!t) return;
    for (int i = 0; i < t->sb_count; i++) free(t->sb[(t->sb_start + i) % SB_MAX].cells);
    free(t->sb);
    free(t->main);
    free(t->alt);
    free(t);
}

static vt_cell *resize_grid(vt *t, vt_cell *old, int rows, int cols, int shift, bool push)
{
    vt_cell *g = alloc_grid(rows, cols);
    if (push)
        for (int r = 0; r < shift; r++)
            sb_push(t, old + (size_t)r * t->cols, t->cols);
    for (int r = 0; r < rows; r++) {
        int src = r + shift;
        if (src >= t->rows) break;
        int n = cols < t->cols ? cols : t->cols;
        memcpy(g + (size_t)r * cols, old + (size_t)src * t->cols, sizeof(vt_cell) * (size_t)n);
    }
    free(old);
    return g;
}

void vt_resize(vt *t, int rows, int cols)
{
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == t->rows && cols == t->cols) return;
    /* keep the cursor row on screen when shrinking */
    int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
    t->main = resize_grid(t, t->main, rows, cols, t->on_alt ? 0 : shift, !t->on_alt);
    t->alt = resize_grid(t, t->alt, rows, cols, t->on_alt ? shift : 0, false);
    t->scr = t->on_alt ? t->alt : t->main;
    t->cy -= shift;
    t->rows = rows;
    t->cols = cols;
    t->top = 0;
    t->bot = rows - 1;
    t->wrapnext = false;
    clamp_cursor(t);
    restore_cursor(t, &t->saved); save_cursor(t, &t->saved); /* clamp saved too */
}

int vt_rows(const vt *t) { return t->rows; }
int vt_cols(const vt *t) { return t->cols; }

const vt_cell *vt_cell_at(const vt *t, int r, int c)
{
    if (r < 0 || r >= t->rows || c < 0 || c >= t->cols) return NULL;
    int idx = t->sb_count - t->view + r; /* index into history+screen */
    if (idx < t->sb_count) {
        const sb_line *l = &t->sb[(t->sb_start + idx) % SB_MAX];
        return c < l->len ? &l->cells[c] : NULL;
    }
    return &t->scr[(size_t)(idx - t->sb_count) * t->cols + c];
}

bool vt_cursor(const vt *t, int *r, int *c)
{
    *r = t->cy;
    *c = t->cx;
    return t->cursor_visible && t->view == 0;
}

bool vt_app_cursor_keys(const vt *t) { return t->app_cursor; }

void vt_scroll_view(vt *t, int delta)
{
    if (t->on_alt) return;
    t->view += delta;
    if (t->view < 0) t->view = 0;
    if (t->view > t->sb_count) t->view = t->sb_count;
}

void vt_scroll_reset(vt *t) { t->view = 0; }
int vt_scroll_offset(const vt *t) { return t->view; }
