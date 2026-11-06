/* dodger.c - tiny rock dodger that showcases SRL.
 * Every SRL variable below can be patched before a run or edited live
 * from srl_editor while the game is playing:  ./srl_editor examples/dodger
 */
#include "srl.h"

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

SRL_FLOAT_RANGE(g_rock_rate, 0.15f, 0.0f, 1.0f);  /* chance of a new rock every 50 ms */
SRL_FLOAT_RANGE(g_fall_speed, 8.0f, 1.0f, 40.0f); /* rows per second                  */
SRL_INT_RANGE(g_lives, 3, 1, 9);                  /* current lives (set it live to revive) */
SRL_INT(g_score, 0);                              /* rocks dodged                     */
SRL_BOOL(g_god_mode, false);                      /* rocks can't hurt you             */
SRL_STR_LEN(g_ship, "<A>", 8);                    /* what the ship looks like         */
SRL_STR(g_player, "Pilot");                       /* name on the HUD                  */

#define FIELD_W 40
#define MAX_ROCKS 256

typedef struct {
    int x;
    float y;
    bool alive;
} rock;

static rock rocks[MAX_ROCKS];

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void spawn_rock(void)
{
    for (int i = 0; i < MAX_ROCKS; i++)
        if (!rocks[i].alive) {
            rocks[i] = (rock){ rand() % FIELD_W, 0.0f, true };
            return;
        }
}

int main(void)
{
    int start_lives = g_lives;
    srand((unsigned)time(NULL));
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(30);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);   /* ship    */
        init_pair(2, COLOR_YELLOW, -1); /* rocks   */
        init_pair(3, COLOR_RED, -1);    /* hit     */
        init_pair(4, COLOR_GREEN, -1);  /* god     */
    }

    int ship_x = FIELD_W / 2;
    double last = now(), spawn_acc = 0, hit_flash = 0;

    for (;;) {
        int h = LINES - 4; /* playfield rows */
        if (h > 20) h = 20;
        if (h < 5) h = 5;
        int ship_len = (int)strnlen(g_ship, sizeof g_ship);
        if (ship_len < 1) ship_len = 1;

        bool over = g_lives <= 0; /* re-checked every frame, so a live edit revives you */

        /* input */
        int ch = getch();
        if (ch == 'q')
            break;
        if (over && ch == 'r') {
            memset(rocks, 0, sizeof rocks);
            g_lives = start_lives;
            g_score = 0;
            over = false;
        }
        if (ch == KEY_LEFT || ch == 'a') ship_x -= 2;
        if (ch == KEY_RIGHT || ch == 'd') ship_x += 2;
        if (ship_x < 0) ship_x = 0;
        if (ship_x > FIELD_W - ship_len) ship_x = FIELD_W - ship_len;

        /* update */
        double t = now(), dt = t - last;
        last = t;
        if (!over) {
            for (spawn_acc += dt; spawn_acc >= 0.05; spawn_acc -= 0.05)
                if ((float)rand() / (float)RAND_MAX < g_rock_rate)
                    spawn_rock();
            for (int i = 0; i < MAX_ROCKS; i++) {
                rock *r = &rocks[i];
                if (!r->alive)
                    continue;
                r->y += g_fall_speed * (float)dt;
                if ((int)r->y == h - 1 && r->x >= ship_x && r->x < ship_x + ship_len) {
                    r->alive = false;
                    if (!g_god_mode) {
                        g_lives--;
                        hit_flash = t + 0.3;
                        if (g_lives <= 0)
                            g_lives = 0;
                    }
                } else if (r->y >= h) {
                    r->alive = false;
                    g_score++;
                }
            }
        }

        /* draw */
        erase();
        mvprintw(0, 1, "PILOT: %-12.31s LIVES: %d   SCORE: %d", g_player, g_lives, g_score);
        if (g_god_mode) {
            attron(COLOR_PAIR(4) | A_BOLD);
            printw("   [GOD MODE]");
            attroff(COLOR_PAIR(4) | A_BOLD);
        }
        mvaddch(1, 0, '+');
        for (int x = 0; x < FIELD_W; x++) addch('-');
        addch('+');
        for (int y = 0; y < h; y++) {
            mvaddch(2 + y, 0, '|');
            mvaddch(2 + y, FIELD_W + 1, '|');
        }
        mvaddch(2 + h, 0, '+');
        for (int x = 0; x < FIELD_W; x++) addch('-');
        addch('+');

        attron(COLOR_PAIR(2) | A_BOLD);
        for (int i = 0; i < MAX_ROCKS; i++)
            if (rocks[i].alive && (int)rocks[i].y < h)
                mvaddch(2 + (int)rocks[i].y, 1 + rocks[i].x, (i & 1) ? '*' : 'o');
        attroff(COLOR_PAIR(2) | A_BOLD);

        int pair = t < hit_flash ? 3 : 1;
        attron(COLOR_PAIR(pair) | A_BOLD);
        mvaddnstr(2 + h - 1, 1 + ship_x, g_ship, ship_len);
        attroff(COLOR_PAIR(pair) | A_BOLD);

        if (over) {
            attron(A_BOLD | A_REVERSE);
            mvprintw(2 + h / 2, FIELD_W / 2 - 13, " GAME OVER  r: retry  q: quit ");
            attroff(A_BOLD | A_REVERSE);
        }
        mvprintw(3 + h, 1, "<-/-> move   q quit   edit me live: ./srl_editor examples/dodger");
        refresh();
    }
    endwin();
    printf("%s dodged %d rocks.\n", g_player, g_score);
    return 0;
}
