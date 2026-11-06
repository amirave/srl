/* srl_gui.c - GTK3 + VTE front end: edit *_SRL globals, run the patched
 * in-memory image inside an embedded terminal.
 *
 * usage: srl_editor [binary]
 */
#define _GNU_SOURCE
#include "srl_core.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <vte/vte.h>

enum { COL_NAME, COL_TYPE, COL_SECTION, COL_VALUE, COL_EDITABLE, COL_WEIGHT, COL_FG, COL_IDX, NCOLS };

static struct {
    GtkWidget *win, *path_label, *tree, *args_entry;
    GtkWidget *run_btn, *stop_btn, *revert_btn, *reload_btn, *status;
    VteTerminal *term;
    GtkListStore *store;
    srl_binary *bin;
    GPid child;
    guint kill_timer;
} app;

static void error_dialog(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void error_dialog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(msg);
}

static void update_ui(void)
{
    bool have = app.bin != NULL, running = app.child != 0;
    gtk_widget_set_sensitive(app.run_btn, have && !running);
    gtk_widget_set_sensitive(app.stop_btn, running);
    gtk_widget_set_sensitive(app.revert_btn, have && srl_count_modified(app.bin) > 0);
    gtk_widget_set_sensitive(app.reload_btn, have);

    char *s;
    if (!have)
        s = g_strdup("Open a binary to begin.");
    else
        s = g_strdup_printf("%zu %s global%s, %zu modified  ·  %s  ·  %s",
                            app.bin->nvars, SRL_SUFFIX, app.bin->nvars == 1 ? "" : "s",
                            srl_count_modified(app.bin),
                            app.bin->have_types ? "all tagged" : "untagged markers: sizes only",
                            running ? "running" : "idle");
    gtk_label_set_text(GTK_LABEL(app.status), s);
    g_free(s);
}

static void fill_row(GtkTreeIter *it, size_t i)
{
    srl_var *v = &app.bin->vars[i];
    bool ed = srl_var_editable(v), mod = srl_var_modified(v);
    char *val = ed ? srl_format(v) : strdup("0  (zero-initialised .bss, not patchable)");
    gtk_list_store_set(app.store, it,
                       COL_NAME, v->name, COL_TYPE, v->type_disp, COL_SECTION, v->section,
                       COL_VALUE, val, COL_EDITABLE, ed,
                       COL_WEIGHT, mod ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                       COL_FG, mod ? "#e67e22" : (ed ? NULL : "gray"),
                       COL_IDX, (int)i, -1);
    free(val);
}

static void refill(void)
{
    gtk_list_store_clear(app.store);
    if (!app.bin)
        return;
    for (size_t i = 0; i < app.bin->nvars; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app.store, &it);
        fill_row(&it, i);
    }
}

static void load_binary(const char *path)
{
    char err[512];
    srl_binary *b = srl_load(path, err, sizeof err);
    if (!b) {
        error_dialog("Could not load binary:\n%s", err);
        return;
    }
    srl_free(app.bin);
    app.bin = b;
    refill();
    gtk_label_set_text(GTK_LABEL(app.path_label), b->path);
    char *t = g_strdup_printf("SRL Editor: %s", g_path_get_basename(b->path));
    gtk_window_set_title(GTK_WINDOW(app.win), t);
    g_free(t);
    if (b->nvars == 0)
        error_dialog("No globals ending in %s were found in\n%s", SRL_SUFFIX, b->path);
    update_ui();
}

/* ---- callbacks ---------------------------------------------------- */

static void on_value_edited(GtkCellRendererText *r, gchar *path, gchar *text, gpointer u)
{
    (void)r; (void)u;
    GtkTreeIter it;
    if (!app.bin || !gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(app.store), &it, path))
        return;
    int idx;
    gtk_tree_model_get(GTK_TREE_MODEL(app.store), &it, COL_IDX, &idx, -1);
    srl_var *v = &app.bin->vars[idx];
    unsigned char *tmp = g_malloc(v->size);
    char err[512];
    if (srl_parse(v, text, tmp, err, sizeof err)) {
        memcpy(v->cur, tmp, v->size);
        fill_row(&it, (size_t)idx);
        update_ui();
    } else {
        error_dialog("Invalid value for %s (%s):\n%s", v->name, v->type_disp, err);
    }
    g_free(tmp);
}

static void on_open(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    GtkWidget *d = gtk_file_chooser_dialog_new("Open binary", GTK_WINDOW(app.win),
                                               GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "_Cancel", GTK_RESPONSE_CANCEL,
                                               "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (app.bin) {
        char *dir = g_path_get_dirname(app.bin->path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), dir);
        g_free(dir);
    }
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        gtk_widget_destroy(d);
        load_binary(f);
        g_free(f);
        return;
    }
    gtk_widget_destroy(d);
}

static void on_reload(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (app.bin) {
        char *p = g_strdup(app.bin->path);
        load_binary(p);
        g_free(p);
    }
}

static void on_revert(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (!app.bin)
        return;
    srl_revert(app.bin);
    refill();
    update_ui();
}

static gboolean feed_exit_msg(gpointer data)
{
    vte_terminal_feed(app.term, data, -1);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void on_child_exit(GPid pid, gint status, gpointer u)
{
    (void)u;
    char *msg;
    if (WIFEXITED(status))
        msg = g_strdup_printf("\r\n\033[1;33m[process exited with code %d]\033[0m\r\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        msg = g_strdup_printf("\r\n\033[1;31m[process killed by signal %d (%s)]\033[0m\r\n",
                              WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        msg = g_strdup("\r\n[process ended]\r\n");
    /* let VTE drain the last output from the pty first */
    g_timeout_add(150, feed_exit_msg, msg);
    g_spawn_close_pid(pid);
    app.child = 0;
    if (app.kill_timer) {
        g_source_remove(app.kill_timer);
        app.kill_timer = 0;
    }
    update_ui();
}

static gboolean force_kill(gpointer u)
{
    (void)u;
    app.kill_timer = 0;
    if (app.child)
        kill(-app.child, SIGKILL);
    return G_SOURCE_REMOVE;
}

static void on_stop(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (!app.child)
        return;
    kill(-app.child, SIGTERM);
    if (!app.kill_timer)
        app.kill_timer = g_timeout_add(1500, force_kill, NULL);
}

static void on_run(GtkWidget *w, gpointer u)
{
    (void)w; (void)u;
    if (!app.bin || app.child)
        return;

    /* argv: original path as argv[0], then user args */
    const char *argtext = gtk_entry_get_text(GTK_ENTRY(app.args_entry));
    gchar **uargv = NULL;
    gint uargc = 0;
    GError *gerr = NULL;
    char *stripped = g_strstrip(g_strdup(argtext));
    if (*stripped && !g_shell_parse_argv(stripped, &uargc, &uargv, &gerr)) {
        error_dialog("Bad arguments: %s", gerr->message);
        g_error_free(gerr);
        g_free(stripped);
        return;
    }
    g_free(stripped);
    char **argv = g_new0(char *, uargc + 2);
    argv[0] = app.bin->path;
    for (int i = 0; i < uargc; i++)
        argv[i + 1] = uargv[i];

    gchar **envp = g_get_environ();
    envp = g_environ_setenv(envp, "TERM", "xterm-256color", TRUE);
    char *cwd = g_path_get_dirname(app.bin->path);

    char err[512];
    int memfd = -1, master = -1, slave = -1;
    memfd = srl_make_memfd(app.bin, err, sizeof err);
    if (memfd < 0) {
        error_dialog("Could not build patched image:\n%s", err);
        goto out;
    }
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        error_dialog("openpty: %s", strerror(errno));
        goto out;
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(slave, F_SETFD, FD_CLOEXEC);

    VtePty *pty = vte_pty_new_foreign_sync(master, NULL, &gerr); /* takes ownership of master */
    master = -1;
    if (!pty) {
        error_dialog("VTE pty: %s", gerr->message);
        g_error_free(gerr);
        goto out;
    }
    vte_terminal_reset(app.term, TRUE, TRUE);
    vte_terminal_set_pty(app.term, pty); /* also sets the pty's window size */
    g_object_unref(pty);

    pid_t pid = srl_spawn(memfd, slave, argv, envp, cwd);
    if (pid < 0) {
        error_dialog("fork: %s", strerror(errno));
        goto out;
    }
    app.child = pid;
    g_child_watch_add(pid, on_child_exit, NULL);
    gtk_widget_grab_focus(GTK_WIDGET(app.term));
    update_ui();

out:
    if (slave >= 0) close(slave);
    if (master >= 0) close(master);
    if (memfd >= 0) close(memfd);
    g_free(cwd);
    g_strfreev(envp);
    g_free(argv);
    g_strfreev(uargv);
}

static void on_destroy(GtkWidget *w, gpointer u)
{
    (void)w; (void)u;
    if (app.child)
        kill(-app.child, SIGKILL);
    gtk_main_quit();
}

/* ---- UI construction ---------------------------------------------- */

static void add_col(const char *title, int col, bool value)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c;
    if (value) {
        g_object_set(r, "family", "monospace", NULL);
        g_signal_connect(r, "edited", G_CALLBACK(on_value_edited), NULL);
        c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
                                                     "editable", COL_EDITABLE,
                                                     "weight", COL_WEIGHT,
                                                     "foreground", COL_FG, NULL);
        gtk_tree_view_column_set_expand(c, TRUE);
    } else {
        c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
                                                     "weight", COL_WEIGHT, NULL);
        gtk_tree_view_column_set_sort_column_id(c, col);
    }
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree), c);
}

static GtkWidget *button(const char *label, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void build_ui(void)
{
    app.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.win), "SRL Editor");
    gtk_window_set_default_size(GTK_WINDOW(app.win), 1000, 760);
    g_signal_connect(app.win, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(app.win), vbox);

    /* top: file */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(top), button("_Open…", G_CALLBACK(on_open)), FALSE, FALSE, 0);
    app.reload_btn = button("Re_load", G_CALLBACK(on_reload));
    gtk_box_pack_start(GTK_BOX(top), app.reload_btn, FALSE, FALSE, 0);
    app.path_label = gtk_label_new("(no binary)");
    gtk_label_set_ellipsize(GTK_LABEL(app.path_label), PANGO_ELLIPSIZE_START);
    gtk_label_set_xalign(GTK_LABEL(app.path_label), 0);
    gtk_box_pack_start(GTK_BOX(top), app.path_label, TRUE, TRUE, 6);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    /* variables table */
    app.store = gtk_list_store_new(NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT);
    app.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.store));
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(app.tree), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    add_col("Name", COL_NAME, false);
    add_col("Type", COL_TYPE, false);
    add_col("Section", COL_SECTION, false);
    add_col("Value  (click a selected value to edit)", COL_VALUE, true);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), app.tree);
    gtk_widget_set_size_request(sw, -1, 200);
    gtk_paned_pack1(GTK_PANED(paned), sw, TRUE, FALSE);

    /* run controls + terminal */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(ctl), gtk_label_new("Arguments:"), FALSE, FALSE, 0);
    app.args_entry = gtk_entry_new();
    g_signal_connect(app.args_entry, "activate", G_CALLBACK(on_run), NULL);
    gtk_box_pack_start(GTK_BOX(ctl), app.args_entry, TRUE, TRUE, 0);
    app.run_btn = button("_Run ▶", G_CALLBACK(on_run));
    app.stop_btn = button("_Stop ■", G_CALLBACK(on_stop));
    app.revert_btn = button("Re_vert edits", G_CALLBACK(on_revert));
    gtk_box_pack_start(GTK_BOX(ctl), app.run_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl), app.stop_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl), app.revert_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ctl, FALSE, FALSE, 4);

    app.term = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(app.term, 10000);
    vte_terminal_set_size(app.term, 100, 24);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace 11");
    vte_terminal_set_font(app.term, fd);
    pango_font_description_free(fd);
    GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(tbox), GTK_WIDGET(app.term), TRUE, TRUE, 0);
    GtkWidget *sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
                                      gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(app.term)));
    gtk_box_pack_start(GTK_BOX(tbox), sb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), tbox, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), bottom, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 300);

    app.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app.status), 0);
    gtk_box_pack_start(GTK_BOX(vbox), app.status, FALSE, FALSE, 0);

    gtk_widget_show_all(app.win);
    update_ui();
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    build_ui();
    if (argc > 1)
        load_binary(argv[1]);
    gtk_main();
    srl_free(app.bin);
    return 0;
}
