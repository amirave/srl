/* srl_tui.c - terminal UI: edit *_SRL globals in an ELF binary, then run the
 * patched in-memory image (memfd + fork/fexecve) inside an embedded terminal
 * pane.
 *
 * usage: srl_editor [binary [args...]]
 */
#define _GNU_SOURCE
#define NCURSES_WIDECHAR 1
#include "srl_core.h"
#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <locale.h>
#include <ncursesw/curses.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wordexp.h>

extern char **environ;

enum { F_VARS, F_TERM };
enum { ED_NONE, ED_VALUE, ED_ARGS, ED_OPEN, ED_SAVE };

static struct {
    srl_binary *bin;
    int sel, list_top;
    int focus;

    pid_t child;
    srl_live live;        /* runtime access to the child's memory */
    unsigned char **live_val; /* per variable: bytes read from the child */
    bool *live_ok;
    double next_poll;
    char live_err[300];
    int master;
    double kill_deadline;
    vt *term;
    char args[2048];
    char run_state[160];

    char msg[600];
    bool msg_err;
    bool confirm_quit;
    bool quit;

    int list_y, list_rows, sep_y, term_y, term_rows, term_cols;

    int ed_kind;
    int ed_var;           /* index of the variable being edited */
    wchar_t ed_buf[2048];
    int ed_len, ed_pos;
    char ed_prompt[300];
    char save_confirm[4096]; /* existing file the user was warned about */

    bool color;
    short pairmap[257 * 257];
    int next_pair;
} S = { .master = -1 };

/* ------------------------------------------------------------------ */
/* misc                                                                */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_msg(bool err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_msg(bool err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.msg, sizeof S.msg, fmt, ap);
    va_end(ap);
    S.msg_err = err;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) {
            if (errno == EAGAIN) {
                struct pollfd p = { fd, POLLOUT, 0 };
                poll(&p, 1, 100);
            }
            continue;
        }
        if (w <= 0)
            return;
        buf += w;
        len -= (size_t)w;
    }
}

static void vt_reply(const char *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (S.master >= 0)
        write_all(S.master, buf, len);
}

/* ------------------------------------------------------------------ */
/* colors                                                              */

enum { P_TITLE = 1, P_MOD, P_SEP_FOCUS, P_ERR, P_OK, P_KEY, P_FIRST_DYNAMIC };

static int map16(int c)
{
    if (c < 16)
        return c;
    if (c >= 232) {
        int l = (c - 232) * 10 + 8;
        return l < 60 ? 0 : l < 130 ? 8 : l < 200 ? 7 : 15;
    }
    c -= 16;
    int r = c / 36, g = (c / 6) % 6, b = c % 6;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx < 2)
        return mx ? 8 : 0;
    int thr = mx - 1;
    int bits = (r >= thr) | (g >= thr) << 1 | (b >= thr) << 2;
    return bits + (mx >= 4 ? 8 : 0);
}

/* returns a color pair for (fg, bg); *bold set if we had to fake brightness */
static short get_pair(int fg, int bg, bool *bold)
{
    *bold = false;
    if (!S.color)
        return 0;
    if (fg < 0 && bg < 0)
        return 0;
    if (COLORS < 256) {
        if (fg >= 0) fg = map16(fg);
        if (bg >= 0) bg = map16(bg);
        if (COLORS < 16) {
            if (fg >= 8) { fg -= 8; *bold = true; }
            if (bg >= 8) bg -= 8;
        }
    }
    int key = (fg + 1) * 257 + (bg + 1);
    if (S.pairmap[key])
        return S.pairmap[key];
    int limit = COLOR_PAIRS < 32000 ? COLOR_PAIRS : 32000;
    if (S.next_pair >= limit)
        return 0;
    init_pair((short)S.next_pair, (short)fg, (short)bg);
    S.pairmap[key] = (short)S.next_pair;
    return (short)S.next_pair++;
}

static void init_colors(void)
{
    S.color = has_colors() && start_color() == OK && COLOR_PAIRS > P_FIRST_DYNAMIC + 8;
    if (!S.color)
        return;
    use_default_colors();
    init_pair(P_TITLE, COLOR_BLACK, COLOR_CYAN);
    init_pair(P_MOD, COLOR_YELLOW, -1);
    init_pair(P_SEP_FOCUS, COLOR_BLACK, COLOR_GREEN);
    init_pair(P_ERR, COLOR_RED, -1);
    init_pair(P_OK, COLOR_GREEN, -1);
    init_pair(P_KEY, COLOR_CYAN, -1);
    S.next_pair = P_FIRST_DYNAMIC;
}

static attr_t cp(int pair) { return S.color ? (attr_t)COLOR_PAIR(pair) : 0; }

/* ------------------------------------------------------------------ */
/* text output helpers                                                 */

static int str_width(const char *s)
{
    wchar_t w[2048];
    size_t n = mbstowcs(w, s, 2047);
    if (n == (size_t)-1)
        return (int)strlen(s);
    int width = 0;
    for (size_t i = 0; i < n; i++) {
        int cw = wcwidth(w[i]);
        width += cw < 0 ? 1 : cw;
    }
    return width;
}

/* draw UTF-8 s at (y, x), at most maxw columns; ellipsize if cut. Returns width drawn. */
static int put(int y, int x, int maxw, const char *s)
{
    if (maxw <= 0)
        return 0;
    wchar_t w[2048];
    size_t n = mbstowcs(w, s, 2047);
    if (n == (size_t)-1) {
        mvaddnstr(y, x, s, maxw);
        return (int)strlen(s) < maxw ? (int)strlen(s) : maxw;
    }
    int width = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        int cw = wcwidth(w[i]);
        if (cw < 0) cw = 1;
        if (width + cw > maxw)
            break;
        width += cw;
    }
    if (i < n && i > 0) { /* cut: replace the last char with an ellipsis */
        i--;
        width -= wcwidth(w[i]) < 0 ? 1 : wcwidth(w[i]);
        w[i++] = L'…';
        width++;
    }
    w[i] = 0;
    mvaddwstr(y, x, w);
    return width;
}

/* help line: alternating "key", "description" pairs, NULL terminated */
static void put_help(int y, const char *const *items)
{
    int x = 1, W = COLS;
    for (int i = 0; items[i] && items[i + 1] && x < W - 1; i += 2) {
        attron(cp(P_KEY) | A_BOLD);
        x += put(y, x, W - x - 1, items[i]);
        attroff(cp(P_KEY) | A_BOLD);
        x += put(y, x, W - x - 1, " ");
        x += put(y, x, W - x - 1, items[i + 1]);
        x += put(y, x, W - x - 1, "   ");
    }
}

/* ------------------------------------------------------------------ */
/* layout                                                              */

static void set_pty_size(void)
{
    if (S.master < 0)
        return;
    struct winsize ws = { .ws_row = (unsigned short)S.term_rows, .ws_col = (unsigned short)S.term_cols };
    ioctl(S.master, TIOCSWINSZ, &ws); /* kernel sends SIGWINCH to the child */
}

static void layout(void)
{
    int H = LINES, W = COLS;
    int n = S.bin ? (int)S.bin->nvars : 0;
    int maxlist = (H - 6) * 2 / 5;
    if (maxlist < 3) maxlist = 3;
    S.list_rows = n < 3 ? 3 : (n > maxlist ? maxlist : n);
    S.list_y = 2;
    S.sep_y = S.list_y + S.list_rows;
    S.term_y = S.sep_y + 1;
    S.term_rows = H - 1 - S.term_y;
    if (S.term_rows < 1) S.term_rows = 1;
    S.term_cols = W > 1 ? W : 1;
    if (S.term)
        vt_resize(S.term, S.term_rows, S.term_cols);
    else
        S.term = vt_new(S.term_rows, S.term_cols, vt_reply, NULL);
    set_pty_size();
}

static void clamp_sel(void)
{
    int n = S.bin ? (int)S.bin->nvars : 0;
    if (S.sel >= n) S.sel = n - 1;
    if (S.sel < 0) S.sel = 0;
    if (S.sel < S.list_top) S.list_top = S.sel;
    if (S.sel >= S.list_top + S.list_rows) S.list_top = S.sel - S.list_rows + 1;
    if (S.list_top < 0) S.list_top = 0;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

static void draw_title(void)
{
    int W = COLS;
    attron(cp(P_TITLE));
    mvhline(0, 0, ' ', W);
    attron(A_BOLD);
    int x = 1 + put(0, 1, W - 2, "SRL Editor");
    attroff(A_BOLD);
    char right[128] = "";
    if (S.bin)
        snprintf(right, sizeof right, " %zu var%s · %zu modified ", S.bin->nvars,
                 S.bin->nvars == 1 ? "" : "s", srl_count_modified(S.bin));
    int rw = str_width(right);
    if (S.bin) {
        x += put(0, x, W - x - rw - 1, "  │  ");
        const char *p = S.bin->path;
        int avail = W - x - rw - 2;
        int pw = str_width(p);
        if (pw > avail && avail > 1) { /* show the end of long paths */
            p += pw - avail + 1;
            x += put(0, x, 1, "…");
            avail--;
        }
        put(0, x, avail, p);
    }
    if (rw < W - 2)
        put(0, W - rw - 1, rw, right);
    attroff(cp(P_TITLE));
}

static bool live_on(void);
static const unsigned char *shown_bytes(size_t i);

static void draw_vars(void)
{
    int W = COLS;
    if (!S.bin || S.bin->nvars == 0) {
        attron(A_DIM);
        put(S.list_y, 2, W - 3, S.bin ? "No " SRL_SUFFIX " markers were found in this binary."
                                      : "No binary loaded. Press o to open one.");
        attroff(A_DIM);
        return;
    }
    int name_w = 4, type_w = 4, sec_w = 7;
    for (size_t i = 0; i < S.bin->nvars; i++) {
        int a = str_width(S.bin->vars[i].name), b = str_width(S.bin->vars[i].type_disp),
            c = str_width(S.bin->vars[i].section);
        if (a > name_w) name_w = a;
        if (b > type_w) type_w = b;
        if (c > sec_w) sec_w = c;
    }
    if (name_w > W / 3) name_w = W / 3;
    if (type_w > 24) type_w = 24;
    if (sec_w > 12) sec_w = 12;
    int x_name = 2, x_type = x_name + name_w + 2, x_sec = x_type + type_w + 2, x_val = x_sec + sec_w + 2;

    attron(A_BOLD | A_UNDERLINE);
    mvhline(1, 0, ' ', W);
    put(1, x_name, name_w, "Name");
    put(1, x_type, type_w, "Type");
    put(1, x_sec, sec_w, "Section");
    put(1, x_val, W - x_val - 1, "Value");
    attroff(A_BOLD | A_UNDERLINE);
    if ((int)S.bin->nvars > S.list_rows) {
        char pos[32];
        snprintf(pos, sizeof pos, " %d/%zu ", S.sel + 1, S.bin->nvars);
        attron(A_DIM);
        put(1, W - str_width(pos) - 1, str_width(pos), pos);
        attroff(A_DIM);
    }

    for (int r = 0; r < S.list_rows; r++) {
        int i = S.list_top + r, y = S.list_y + r;
        if (i >= (int)S.bin->nvars)
            break;
        srl_var *v = &S.bin->vars[i];
        const unsigned char *bytes = shown_bytes((size_t)i);
        bool mod = srl_var_modified(v), ed = bytes != NULL, sel = i == S.sel;
        /* while running, highlight values the program changed by itself */
        bool drift = bytes && bytes != v->cur && memcmp(bytes, v->cur, v->size) && srl_var_editable(v);
        attr_t a = 0;
        if (sel) a |= S.focus == F_VARS && S.ed_kind == ED_NONE ? A_REVERSE : (S.focus == F_VARS ? A_REVERSE : A_UNDERLINE);
        if (mod) a |= cp(P_MOD) | A_BOLD;
        if (!ed) a |= A_DIM;
        attron(a);
        mvhline(y, 0, ' ', W);
        if (mod) put(y, 0, 1, "*");
        put(y, x_name, name_w, v->name);
        put(y, x_type, type_w, v->type_disp);
        put(y, x_sec, sec_w, v->section);
        char *val = ed ? srl_format_bytes(v, bytes)
                       : strdup(S.child ? "(waiting for the program...)"
                                        : "(zero-initialised, not in the file: editable while running)");
        int vw = put(y, x_val, W - x_val - 1, val);
        free(val);
        if (drift && x_val + vw + 12 < W) {
            attron(A_DIM);
            put(y, x_val + vw + 2, W - x_val - vw - 3, "(changed by program)");
            attroff(A_DIM);
        }
        attroff(a);
    }
    /* scroll hints */
    if (S.list_top > 0) mvaddwstr(S.list_y, W - 1, L"↑");
    if (S.list_top + S.list_rows < (int)S.bin->nvars) mvaddwstr(S.list_y + S.list_rows - 1, W - 1, L"↓");
}

static void draw_sep(void)
{
    int W = COLS, y = S.sep_y;
    attr_t a = S.focus == F_TERM ? cp(P_SEP_FOCUS) | A_BOLD : A_BOLD;
    attron(a);
    if (S.focus == F_TERM)
        mvhline(y, 0, ' ', W);
    else
        mvhline(y, 0, ACS_HLINE, W);
    char buf[2400];
    int off = vt_scroll_offset(S.term);
    snprintf(buf, sizeof buf, " Output: %s%s ", S.run_state,
             live_on() ? (S.live_err[0] ? ", live read failed" : ", live editing") : "");
    int x = 2 + put(y, 2, W - 3, buf);
    if (S.args[0]) {
        snprintf(buf, sizeof buf, " args: %s ", S.args);
        x += put(y, x, W - x - 1, buf);
    }
    if (off) {
        snprintf(buf, sizeof buf, " scrolled back %d line%s ", off, off == 1 ? "" : "s");
        x += put(y, x, W - x - 1, buf);
    }
    if (S.focus == F_TERM) {
        const char *h = " keyboard → program  (Ctrl-] to leave) ";
        int hw = str_width(h);
        if (x + hw < W)
            put(y, W - hw - 1, hw, h);
    }
    attroff(a);
}

static void draw_term(void)
{
    int rows = vt_rows(S.term), cols = vt_cols(S.term);
    for (int r = 0; r < rows && r < S.term_rows; r++) {
        int y = S.term_y + r;
        move(y, 0);
        for (int c = 0; c < cols && c < COLS; c++) {
            const vt_cell *cell = vt_cell_at(S.term, r, c);
            cchar_t cc;
            wchar_t wc[2] = { L' ', 0 };
            attr_t a = 0;
            short pair = 0;
            if (cell) {
                if (cell->ch == VT_WIDE_CONT)
                    continue;
                wc[0] = (wchar_t)cell->ch;
                if (cell->attr & VT_BOLD) a |= A_BOLD;
                if (cell->attr & VT_DIM) a |= A_DIM;
                if (cell->attr & VT_UNDERLINE) a |= A_UNDERLINE;
                if (cell->attr & VT_REVERSE) a |= A_REVERSE;
                if (cell->attr & VT_BLINK) a |= A_BLINK;
#ifdef A_ITALIC
                if (cell->attr & VT_ITALIC) a |= A_ITALIC;
#endif
                bool fake_bold;
                pair = get_pair(cell->fg, cell->bg, &fake_bold);
                if (fake_bold) a |= A_BOLD;
                /* a wide char that would not fit in the last column */
                if (wcwidth(wc[0]) == 2 && c == cols - 1) wc[0] = L' ';
            }
            setcchar(&cc, wc, a, pair, NULL);
            mvadd_wch(y, c, &cc);
        }
    }
}

static void draw_bottom(int *cur_y, int *cur_x)
{
    int W = COLS, y = LINES - 1;
    move(y, 0);
    clrtoeol();
    if (S.ed_kind != ED_NONE) {
        if (S.msg[0] && S.msg_err && y > 0) { /* e.g. a rejected value: show it above the editor */
            move(y - 1, 0);
            clrtoeol();
            attron(cp(P_ERR) | A_BOLD | A_REVERSE);
            mvhline(y - 1, 0, ' ', W);
            put(y - 1, 1, W - 2, S.msg);
            attroff(cp(P_ERR) | A_BOLD | A_REVERSE);
        }
        attron(A_BOLD);
        int x = put(y, 0, W / 2, S.ed_prompt);
        attroff(A_BOLD);
        int avail = W - x - 1;
        if (avail < 1) avail = 1;
        int start = 0;
        if (S.ed_pos > avail - 1) start = S.ed_pos - (avail - 1);
        int n = S.ed_len - start;
        if (n > avail) n = avail;
        wchar_t tmp[2048];
        wmemcpy(tmp, S.ed_buf + start, (size_t)n);
        tmp[n] = 0;
        attron(A_UNDERLINE);
        mvhline(y, x, ' ', avail);
        mvaddwstr(y, x, tmp);
        attroff(A_UNDERLINE);
        *cur_y = y;
        *cur_x = x + (S.ed_pos - start);
        return;
    }
    if (S.msg[0]) {
        attron(cp(S.msg_err ? P_ERR : P_OK) | A_BOLD);
        put(y, 1, W - 2, S.msg);
        attroff(cp(S.msg_err ? P_ERR : P_OK) | A_BOLD);
        return;
    }
    if (S.focus == F_TERM) {
        static const char *const h[] = { "Ctrl-]", "back to variables", "Shift-PgUp/PgDn", "scroll output",
                                         NULL, NULL };
        put_help(y, h);
    } else {
        static const char *const h[] = { "↑↓", "select", "Enter", "edit", "r", "run", "a", "args", "s", "stop",
                                         "u/U", "undo/undo all", "Tab", "to program", "[ ]", "scroll output",
                                         "w", "save as", "o", "open", "l", "reload", "q", "quit", NULL, NULL };
        put_help(y, h);
    }
}

static void draw(void)
{
    erase();
    clamp_sel();
    draw_title();
    draw_vars();
    draw_sep();
    draw_term();
    int cy = -1, cx = -1;
    draw_bottom(&cy, &cx);

    int tr, tc;
    if (cy >= 0) {
        curs_set(1);
        move(cy, cx);
    } else if (S.focus == F_TERM && S.child && vt_cursor(S.term, &tr, &tc)) {
        curs_set(1);
        move(S.term_y + tr, tc);
    } else {
        curs_set(0);
    }
    refresh();
}

/* ------------------------------------------------------------------ */
/* loading                                                             */

static void live_free(void);

static bool load(const char *path)
{
    char err[600];
    srl_binary *b = srl_load(path, err, sizeof err);
    if (!b) {
        set_msg(true, "Could not load: %s", err);
        return false;
    }
    live_free(); /* a running child keeps going, but live editing stops */
    srl_free(S.bin);
    S.bin = b;
    S.sel = S.list_top = 0;
    layout();
    if (b->nvars == 0)
        set_msg(true, "No %s markers in %s", SRL_SUFFIX, b->path);
    else
        set_msg(false, "Loaded %zu variable%s%s", b->nvars, b->nvars == 1 ? "" : "s",
                b->have_types ? "" : "  (some types unknown: guessed from size)");
    return true;
}

/* ------------------------------------------------------------------ */
/* running the child                                                   */

static char **make_env(void)
{
    size_t n = 0;
    while (environ[n]) n++;
    char **env = calloc(n + 2, sizeof *env);
    if (!env) abort();
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(environ[i], "TERM=", 5) || !strncmp(environ[i], "COLUMNS=", 8) ||
            !strncmp(environ[i], "LINES=", 6))
            continue;
        env[k++] = environ[i];
    }
    env[k++] = "TERM=xterm-256color";
    env[k] = NULL;
    return env;
}

/* ---- runtime editing ---------------------------------------------- */

static void live_free(void)
{
    if (S.live_val && S.bin)
        for (size_t i = 0; i < S.bin->nvars; i++)
            free(S.live_val[i]);
    free(S.live_val);
    free(S.live_ok);
    S.live_val = NULL;
    S.live_ok = NULL;
    S.live_err[0] = 0;
    memset(&S.live, 0, sizeof S.live);
}

static void live_start(pid_t pid, int exe_fd)
{
    live_free();
    srl_live_init(&S.live, pid, exe_fd);
    S.live_val = calloc(S.bin->nvars + 1, sizeof *S.live_val);
    S.live_ok = calloc(S.bin->nvars + 1, sizeof *S.live_ok);
    if (!S.live_val || !S.live_ok) abort();
    for (size_t i = 0; i < S.bin->nvars; i++)
        if (!(S.live_val[i] = malloc(S.bin->vars[i].size))) abort();
    S.next_poll = 0;
}

static bool live_on(void) { return S.child && S.live.ready; }

/* Read every variable's current value from the running child. */
static void live_refresh(void)
{
    if (!S.child || !S.live_val || now() < S.next_poll)
        return;
    S.next_poll = now() + 0.2;
    char err[300];
    if (!srl_live_locate(&S.live, S.bin, err, sizeof err))
        return; /* not exec'd yet: try again shortly */
    S.live_err[0] = 0;
    for (size_t i = 0; i < S.bin->nvars; i++) {
        S.live_ok[i] = srl_live_read(&S.live, S.bin, &S.bin->vars[i], S.live_val[i], err, sizeof err);
        if (!S.live_ok[i])
            snprintf(S.live_err, sizeof S.live_err, "%s", err);
    }
}

/* Push bytes into the running child. Returns false (and sets msg) on failure. */
static bool live_push(size_t i, const unsigned char *bytes, char *err, size_t errlen)
{
    srl_var *v = &S.bin->vars[i];
    if (!srl_live_write(&S.live, S.bin, v, bytes, err, errlen))
        return false;
    memcpy(S.live_val[i], bytes, v->size);
    S.live_ok[i] = true;
    return true;
}

/* What the Value column / editor should start from for variable i. */
static const unsigned char *shown_bytes(size_t i)
{
    if (live_on() && S.live_ok && S.live_ok[i])
        return S.live_val[i];
    return srl_var_editable(&S.bin->vars[i]) ? S.bin->vars[i].cur : NULL;
}

/* Undo variable i (back to the file's value), also in the running child. */
static void revert_var(size_t i)
{
    srl_var *v = &S.bin->vars[i];
    char err[300];
    memcpy(v->cur, v->orig, v->size);
    if (live_on() && srl_var_editable(v))
        live_push(i, v->orig, err, sizeof err);
}

static void run_child(void)
{
    if (!S.bin) {
        set_msg(true, "Open a binary first (o)");
        return;
    }
    if (S.child) {
        set_msg(true, "Already running: press s to stop it first");
        return;
    }

    wordexp_t we;
    memset(&we, 0, sizeof we);
    int wr = wordexp(S.args, &we, WRDE_NOCMD | WRDE_SHOWERR);
    if (wr != 0) {
        set_msg(true, "Could not parse the arguments (unbalanced quotes, or $(...) which isn't allowed)");
        return;
    }
    char **argv = calloc(we.we_wordc + 2, sizeof *argv);
    if (!argv) abort();
    argv[0] = S.bin->path;
    for (size_t i = 0; i < we.we_wordc; i++)
        argv[i + 1] = we.we_wordv[i];

    char err[600];
    char **env = make_env();
    char *dircopy = strdup(S.bin->path);
    const char *cwd = dirname(dircopy);
    int memfd = -1, m = -1, s = -1;

    memfd = srl_make_memfd(S.bin, err, sizeof err);
    if (memfd < 0) {
        set_msg(true, "Could not build the patched image: %s", err);
        goto out;
    }
    struct winsize ws = { .ws_row = (unsigned short)S.term_rows, .ws_col = (unsigned short)S.term_cols };
    if (openpty(&m, &s, NULL, NULL, &ws) < 0) {
        set_msg(true, "openpty: %s", strerror(errno));
        goto out;
    }
    fcntl(m, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);

    vt_reset(S.term);
    pid_t pid = srl_spawn(memfd, s, argv, env, cwd);
    if (pid < 0) {
        set_msg(true, "fork: %s", strerror(errno));
        close(m);
        m = -1;
        goto out;
    }
    S.child = pid;
    live_start(pid, memfd);
    S.master = m;
    m = -1;
    S.kill_deadline = 0;
    S.focus = F_TERM;
    snprintf(S.run_state, sizeof S.run_state, "running (pid %d)", (int)pid);
    size_t nm = srl_count_modified(S.bin);
    set_msg(false, "Started %s with %zu patched value%s (in memory only: the file on disk is untouched)",
            basename(S.bin->path), nm, nm == 1 ? "" : "s");

out:
    if (s >= 0) close(s);
    if (m >= 0) close(m);
    if (memfd >= 0) close(memfd);
    free(dircopy);
    free(env);
    free(argv);
    wordfree(&we);
}

static void read_child(size_t budget)
{
    char buf[16384];
    while (S.master >= 0 && budget > 0) {
        ssize_t r = read(S.master, buf, sizeof buf);
        if (r > 0) {
            vt_write(S.term, buf, (size_t)r);
            budget = (size_t)r > budget ? 0 : budget - (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == EAGAIN)
            return;
        /* EOF / EIO: every slave fd is closed */
        close(S.master);
        S.master = -1;
    }
}

static void check_child(void)
{
    if (!S.child)
        return;
    int st;
    pid_t r = waitpid(S.child, &st, WNOHANG);
    if (r == 0) {
        if (S.kill_deadline && now() > S.kill_deadline) {
            kill(-S.child, SIGKILL);
            S.kill_deadline = 0;
        }
        return;
    }
    read_child((size_t)-1); /* drain what is left */
    if (S.master >= 0) {
        close(S.master);
        S.master = -1;
    }
    char note[256];
    if (r < 0)
        snprintf(S.run_state, sizeof S.run_state, "ended");
    else if (WIFEXITED(st))
        snprintf(S.run_state, sizeof S.run_state, "exited with code %d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        snprintf(S.run_state, sizeof S.run_state, "killed by signal %d (%s)", WTERMSIG(st), strsignal(WTERMSIG(st)));
    else
        snprintf(S.run_state, sizeof S.run_state, "ended");
    snprintf(note, sizeof note, "\r\n\033[0;1;33m[process %s]\033[0m\r\n", S.run_state);
    vt_write(S.term, note, strlen(note));
    S.child = 0;
    live_free();
    S.focus = F_VARS;
    S.confirm_quit = false;
}

static void stop_child(void)
{
    if (!S.child) {
        set_msg(true, "Nothing is running");
        return;
    }
    kill(-S.child, SIGTERM);
    S.kill_deadline = now() + 1.5;
    set_msg(false, "Sent SIGTERM (SIGKILL in 1.5s if it doesn't exit)");
}

/* ------------------------------------------------------------------ */
/* line editor                                                         */

static void start_edit(int kind, const char *prompt, const char *initial)
{
    S.ed_kind = kind;
    snprintf(S.ed_prompt, sizeof S.ed_prompt, "%s", prompt);
    size_t n = mbstowcs(S.ed_buf, initial, 2047);
    if (n == (size_t)-1) n = 0;
    S.ed_len = S.ed_pos = (int)n;
    S.msg[0] = 0;
}

static void commit_edit(void)
{
    char text[8192];
    S.ed_buf[S.ed_len] = 0;
    if (wcstombs(text, S.ed_buf, sizeof text) == (size_t)-1) {
        set_msg(true, "Invalid characters in input");
        return;
    }
    switch (S.ed_kind) {
    case ED_VALUE: {
        srl_var *v = &S.bin->vars[S.ed_var];
        unsigned char *tmp = malloc(v->size);
        char err[600];
        if (!tmp) abort();
        if (srl_parse(v, text, tmp, err, sizeof err)) {
            bool in_file = srl_var_editable(v);
            if (in_file)
                memcpy(v->cur, tmp, v->size);
            S.ed_kind = ED_NONE;
            char *val = srl_format_bytes(v, tmp);
            if (live_on()) {
                if (live_push((size_t)S.ed_var, tmp, err, sizeof err))
                    set_msg(false, "%s = %s   (live%s)", v->name, val, in_file ? ", and on the next run" : " only: not in the file");
                else
                    set_msg(true, "%s = %s, but the live write failed: %s%s", v->name, val, err,
                            in_file ? " (applies on the next run)" : "");
            } else {
                set_msg(false, "%s = %s%s", v->name, val,
                        S.child ? "   (program not loaded yet: applies on the next run)" : "");
            }
            free(val);
        } else {
            set_msg(true, "%s", err); /* keep the editor open so it can be fixed */
        }
        free(tmp);
        break;
    }
    case ED_ARGS:
        snprintf(S.args, sizeof S.args, "%s", text);
        S.ed_kind = ED_NONE;
        set_msg(false, S.args[0] ? "Arguments set: %s" : "Arguments cleared%s", S.args);
        break;
    case ED_SAVE: {
        char path[4096], err[600];
        struct stat st;
        if (!text[0]) {
            set_msg(true, "Enter a file name");
            break;
        }
        if (text[0] == '~' && (text[1] == '/' || !text[1]) && getenv("HOME"))
            snprintf(path, sizeof path, "%s%s", getenv("HOME"), text + 1);
        else
            snprintf(path, sizeof path, "%s", text);
        if (stat(path, &st) == 0 && strcmp(S.save_confirm, path) != 0) {
            snprintf(S.save_confirm, sizeof S.save_confirm, "%s", path);
            set_msg(true, "%s already exists: press Enter again to overwrite it", path);
            break;
        }
        S.save_confirm[0] = 0;
        if (srl_save(S.bin, path, err, sizeof err)) {
            size_t nm = 0;
            for (size_t i = 0; i < S.bin->nvars; i++)
                nm += srl_var_editable(&S.bin->vars[i]) && srl_var_modified(&S.bin->vars[i]);
            S.ed_kind = ED_NONE;
            set_msg(false, "Saved %s with %zu patched value%s", path, nm, nm == 1 ? "" : "s");
        } else {
            set_msg(true, "Could not save: %s", err);
        }
        break;
    }
    case ED_OPEN: {
        char path[4096];
        if (text[0] == '~' && (text[1] == '/' || !text[1]) && getenv("HOME"))
            snprintf(path, sizeof path, "%s%s", getenv("HOME"), text + 1);
        else
            snprintf(path, sizeof path, "%s", text);
        if (load(path))
            S.ed_kind = ED_NONE;
        break;
    }
    }
}

static void edit_key(bool is_key, wint_t k)
{
    if (!is_key) {
        switch (k) {
        case '\r': case '\n': commit_edit(); return;
        case 27: S.ed_kind = ED_NONE; S.msg[0] = 0; return;
        case 127: case 8: k = KEY_BACKSPACE; is_key = true; break;
        case 1: S.ed_pos = 0; return;
        case 5: S.ed_pos = S.ed_len; return;
        case 2: k = KEY_LEFT; is_key = true; break;
        case 6: k = KEY_RIGHT; is_key = true; break;
        case 21: S.ed_len = S.ed_pos = 0; return;
        case 11: S.ed_len = S.ed_pos; return;
        default:
            if (k >= 32 && S.ed_len < 2046) {
                wmemmove(S.ed_buf + S.ed_pos + 1, S.ed_buf + S.ed_pos, (size_t)(S.ed_len - S.ed_pos));
                S.ed_buf[S.ed_pos++] = (wchar_t)k;
                S.ed_len++;
            }
            return;
        }
    }
    switch (k) {
    case KEY_ENTER: commit_edit(); break;
    case KEY_LEFT: if (S.ed_pos > 0) S.ed_pos--; break;
    case KEY_RIGHT: if (S.ed_pos < S.ed_len) S.ed_pos++; break;
    case KEY_HOME: S.ed_pos = 0; break;
    case KEY_END: S.ed_pos = S.ed_len; break;
    case KEY_BACKSPACE:
        if (S.ed_pos > 0) {
            wmemmove(S.ed_buf + S.ed_pos - 1, S.ed_buf + S.ed_pos, (size_t)(S.ed_len - S.ed_pos));
            S.ed_pos--;
            S.ed_len--;
        }
        break;
    case KEY_DC:
        if (S.ed_pos < S.ed_len) {
            wmemmove(S.ed_buf + S.ed_pos, S.ed_buf + S.ed_pos + 1, (size_t)(S.ed_len - S.ed_pos - 1));
            S.ed_len--;
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* keys                                                                */

static void send_to_child(bool is_key, wint_t k)
{
    char buf[16];
    const char *s = NULL;
    size_t n = 0;
    if (!is_key) {
        if (k < 0x80) {
            buf[0] = (char)k;
            n = 1;
        } else {
            mbstate_t st;
            memset(&st, 0, sizeof st);
            n = wcrtomb(buf, (wchar_t)k, &st);
            if (n == (size_t)-1) return;
        }
        s = buf;
    } else {
        bool app = vt_app_cursor_keys(S.term);
        switch (k) {
        case KEY_UP: s = app ? "\033OA" : "\033[A"; break;
        case KEY_DOWN: s = app ? "\033OB" : "\033[B"; break;
        case KEY_RIGHT: s = app ? "\033OC" : "\033[C"; break;
        case KEY_LEFT: s = app ? "\033OD" : "\033[D"; break;
        case KEY_HOME: s = app ? "\033OH" : "\033[H"; break;
        case KEY_END: s = app ? "\033OF" : "\033[F"; break;
        case KEY_BACKSPACE: s = "\177"; break;
        case KEY_DC: s = "\033[3~"; break;
        case KEY_IC: s = "\033[2~"; break;
        case KEY_PPAGE: s = "\033[5~"; break;
        case KEY_NPAGE: s = "\033[6~"; break;
        case KEY_ENTER: s = "\r"; break;
        case KEY_BTAB: s = "\033[Z"; break;
        default:
            if (k >= KEY_F(1) && k <= KEY_F(12)) {
                static const char *fk[] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
                                            "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~" };
                s = fk[k - KEY_F(1)];
            }
        }
        if (!s) return;
        n = strlen(s);
    }
    if (S.master >= 0) {
        vt_scroll_reset(S.term);
        write_all(S.master, s, n);
    }
}

static void edit_selected(void)
{
    clamp_sel();
    if (!S.bin || !S.bin->nvars) {
        set_msg(true, S.bin ? "No variables to edit" : "Open a binary first (o)");
        return;
    }
    srl_var *v = &S.bin->vars[S.sel];
    const unsigned char *bytes = shown_bytes((size_t)S.sel);
    if (!bytes) {
        set_msg(true, "%s is zero-initialised (%s), so it has no bytes in the file to patch. "
                      "Run the program to edit it live, or give it a non-zero initialiser.",
                v->name, v->section);
        return;
    }
    char prompt[300], *val = srl_format_bytes(v, bytes);
    snprintf(prompt, sizeof prompt, " %s (%s) = ", v->name, v->type_disp);
    start_edit(ED_VALUE, prompt, val);
    S.ed_var = S.sel;
    free(val);
}

static void vars_key(bool is_key, wint_t k)
{
    int n = S.bin ? (int)S.bin->nvars : 0;
    if (is_key || k != 'q')
        S.confirm_quit = false;
    if (is_key) {
        switch (k) {
        case KEY_UP: S.sel--; break;
        case KEY_DOWN: S.sel++; break;
        case KEY_PPAGE: S.sel -= S.list_rows; break;
        case KEY_NPAGE: S.sel += S.list_rows; break;
        case KEY_HOME: S.sel = 0; break;
        case KEY_END: S.sel = n - 1; break;
        case KEY_ENTER: edit_selected(); break;
        case KEY_F(5): run_child(); break;
        case KEY_F(10): S.quit = true; break;
        default: break;
        }
        return;
    }
    switch (k) {
    case 'k': S.sel--; break;
    case 'j': S.sel++; break;
    case 'g': S.sel = 0; break;
    case 'G': S.sel = n - 1; break;
    case '\r': case '\n': case 'e': edit_selected(); break;
    case 'r': run_child(); break;
    case 'a': start_edit(ED_ARGS, " Arguments: ", S.args); break;
    case 's': stop_child(); break;
    case 'u':
        if (n) {
            revert_var((size_t)S.sel);
            set_msg(false, "Reverted %s%s", S.bin->vars[S.sel].name, live_on() ? " (also live)" : "");
        }
        break;
    case 'U':
        if (S.bin) {
            for (size_t i = 0; i < S.bin->nvars; i++)
                revert_var(i);
            set_msg(false, "Reverted all edits%s", live_on() ? " (also live)" : "");
        }
        break;
    case '\t': S.focus = F_TERM; break;
    case '[': vt_scroll_view(S.term, S.term_rows / 2); break;
    case ']': vt_scroll_view(S.term, -(S.term_rows / 2)); break;
    case 'o': start_edit(ED_OPEN, " Open binary: ", S.bin ? S.bin->path : ""); break;
    case 'w':
        if (!S.bin) {
            set_msg(true, "Open a binary first (o)");
        } else {
            char def[4200];
            snprintf(def, sizeof def, "%s.pat", S.bin->path);
            S.save_confirm[0] = 0;
            start_edit(ED_SAVE, " Save patched binary as: ", def);
        }
        break;
    case 'l':
        if (S.bin) {
            char *p = strdup(S.bin->path);
            if (srl_count_modified(S.bin)) {
                load(p);
                set_msg(false, "Reloaded from disk (edits discarded)");
            } else {
                load(p);
            }
            free(p);
        }
        break;
    case 'q':
        if (S.child && !S.confirm_quit) {
            S.confirm_quit = true;
            set_msg(true, "The program is still running. Press q again to kill it and quit.");
        } else {
            S.quit = true;
        }
        break;
    default: break;
    }
}

static void handle_key(bool is_key, wint_t k)
{
    if (is_key && k == KEY_RESIZE) {
        layout();
        return;
    }
    if (S.ed_kind != ED_NONE) {
        edit_key(is_key, k);
        return;
    }
    if (is_key && (k == KEY_SPREVIOUS || k == KEY_SNEXT)) {
        vt_scroll_view(S.term, k == KEY_SPREVIOUS ? S.term_rows / 2 : -(S.term_rows / 2));
        return;
    }
    if (S.focus == F_TERM) {
        if (!is_key && k == 0x1d) { /* Ctrl-] */
            S.focus = F_VARS;
            return;
        }
        if (S.child) {
            send_to_child(is_key, k);
            return;
        }
        /* nothing running: fall through and treat it as a normal key */
        S.focus = F_VARS;
        if (!is_key && (k == '\t' || k == 27))
            return;
    }
    S.msg[0] = 0;
    vars_key(is_key, k);
    clamp_sel();
}

/* ------------------------------------------------------------------ */

static char *join_args(int argc, char **argv)
{
    static char out[2048];
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < argc; i++) {
        bool q = strpbrk(argv[i], " \t\"'\\$`*?[]{}()<>|&;~") != NULL || !argv[i][0];
        char piece[1024];
        if (q) {
            size_t p = 0;
            piece[p++] = '\'';
            for (const char *s = argv[i]; *s && p < sizeof piece - 6; s++) {
                if (*s == '\'') { memcpy(piece + p, "'\\''", 4); p += 4; }
                else piece[p++] = *s;
            }
            piece[p++] = '\'';
            piece[p] = 0;
        } else {
            snprintf(piece, sizeof piece, "%s", argv[i]);
        }
        o += (size_t)snprintf(out + o, sizeof out - o, "%s%s", i ? " " : "", piece);
        if (o >= sizeof out) break;
    }
    return out;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: %s [binary [args...]]\n"
               "Edit globals marked with " SRL_SUFFIX "[_INT|_FLOAT|_STR] pointers and run the patched binary (in memory only).\n", argv[0]);
        return 0;
    }
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    snprintf(S.run_state, sizeof S.run_state, "not started");

    initscr();
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    set_escdelay(25);
    init_colors();
    layout();

    if (argc > 1) {
        snprintf(S.args, sizeof S.args, "%s", join_args(argc - 2, argv + 2));
        load(argv[1]);
    } else {
        start_edit(ED_OPEN, " Open binary: ", "");
    }

    while (!S.quit) {
        draw();
        struct pollfd fds[2] = { { 0, POLLIN, 0 }, { S.master, POLLIN, 0 } };
        int nfds = S.master >= 0 ? 2 : 1;
        poll(fds, (nfds_t)nfds, S.child ? 50 : 500);
        if (S.master >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
            read_child(1 << 20);
        wint_t k;
        int r;
        while ((r = get_wch(&k)) != ERR) {
            handle_key(r == KEY_CODE_YES, k);
            if (S.quit) break;
        }
        check_child();
        live_refresh();
    }

    if (S.child) {
        kill(-S.child, SIGKILL);
        waitpid(S.child, NULL, 0);
    }
    endwin();
    srl_free(S.bin);
    vt_free(S.term);
    return 0;
}
