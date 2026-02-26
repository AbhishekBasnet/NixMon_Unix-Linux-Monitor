#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include "protocol.h"

#define SERVER_IP "127.0.0.1"

/* ── color pair IDs ───────────────────────────────────────────────────────── */
#define COLOR_LOW    1   /* green  — 0–50%   */
#define COLOR_MED    2   /* yellow — 50–80%  */
#define COLOR_HIGH   3   /* red    — 80–100% */
#define COLOR_TITLE  4   /* cyan   — headers */
#define COLOR_BORDER 5   /* white  — borders */

/* ── JSON parsers (same as commit 5) ─────────────────────────────────────── */
static double parse_double(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0.0;
    p += strlen(search);
    return atof(p);
}

static long parse_long(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    return atol(p);
}

static void parse_cpu_cores(const char *json, SystemMetrics *m) {
    const char *p = strstr(json, "\"cpu_cores\":[");
    if (!p) return;
    p += strlen("\"cpu_cores\":[");
    m->core_count = 0;
    while (*p && *p != ']' && m->core_count < 16) {
        m->cpu_cores[m->core_count++] = atof(p);
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
}

static void parse_metrics(const char *json, SystemMetrics *m) {
    m->cpu_pct      = parse_double(json, "cpu_pct");
    m->mem_total_kb = parse_long  (json, "mem_total_kb");
    m->mem_avail_kb = parse_long  (json, "mem_avail_kb");
    m->mem_pct      = parse_double(json, "mem_pct");
    m->load_1m      = parse_double(json, "load_1m");
    m->load_5m      = parse_double(json, "load_5m");
    m->load_15m     = parse_double(json, "load_15m");
    m->uptime_sec   = parse_long  (json, "uptime_sec");
    parse_cpu_cores(json, m);
}

/* ── TCP connect ──────────────────────────────────────────────────────────── */
static int connect_to_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

/* ── pick color pair based on percentage ─────────────────────────────────── */
static int pct_color(double pct) {
    if (pct >= 80.0) return COLOR_HIGH;
    if (pct >= 50.0) return COLOR_MED;
    return COLOR_LOW;
}

/* ── draw a labeled bar gauge ─────────────────────────────────────────────── */
/*  label  [████████░░░░░░░░░░░░]  XX.X%                                      */
static void draw_bar(WINDOW *win, int row, int col,
                     const char *label, int label_w,
                     double pct, int bar_w) {
    int filled = (int)(pct / 100.0 * bar_w);
    if (filled > bar_w) filled = bar_w;

    /* label */
    mvwprintw(win, row, col, "%-*s", label_w, label);
    col += label_w;

    /* opening bracket */
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    mvwaddch(win, row, col++, '[');
    wattroff(win, COLOR_PAIR(COLOR_BORDER));

    /* filled portion */
    wattron(win, COLOR_PAIR(pct_color(pct)) | A_BOLD);
    for (int i = 0; i < filled; i++)
        mvwaddch(win, row, col + i, '#');
    wattroff(win, COLOR_PAIR(pct_color(pct)) | A_BOLD);

    /* empty portion */
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    for (int i = filled; i < bar_w; i++)
        mvwaddch(win, row, col + i, '.');
    col += bar_w;

    /* closing bracket + percentage */
    mvwprintw(win, row, col, "]  %5.1f%%", pct);
    wattroff(win, COLOR_PAIR(COLOR_BORDER));
                     }

                     /* ── format uptime into d:hh:mm:ss ───────────────────────────────────────── */
                     static void fmt_uptime(long sec, char *buf, size_t len) {
                         long d  =  sec / 86400;
                         long h  = (sec % 86400) / 3600;
                         long m  = (sec % 3600)  / 60;
                         long s  =  sec % 60;
                         if (d > 0)
                             snprintf(buf, len, "%ldd %02ldh %02ldm %02lds", d, h, m, s);
                         else
                             snprintf(buf, len, "%02ldh %02ldm %02lds", h, m, s);
                     }

                     /* ── render full dashboard ────────────────────────────────────────────────── */
                     static void render(WINDOW *win, const SystemMetrics *m) {
                         int rows, cols;
                         getmaxyx(win, rows, cols);
                         (void)rows;

                         wclear(win);
                         box(win, 0, 0);

                         int bar_w  = cols - 26;   /* dynamic bar width */
                         if (bar_w < 10) bar_w = 10;

                         /* ── title bar ── */
                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, 0, (cols - 34) / 2,
                                   "  NixMon - Real-Time System Monitor  ");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

                         int row = 2;

                         /* ── CPU section ── */
                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, row++, 2, "CPU");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

                         char lbl[16];
                         snprintf(lbl, sizeof(lbl), "  avg     ");
                         draw_bar(win, row++, 2, lbl, 10, m->cpu_pct, bar_w);

                         for (int i = 0; i < m->core_count && i < 8; i++) {
                             snprintf(lbl, sizeof(lbl), "  core%-2d  ", i);
                             draw_bar(win, row++, 2, lbl, 10, m->cpu_cores[i], bar_w);
                         }
                         row++;

                         /* ── Memory section ── */
                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, row++, 2, "MEMORY");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

                         long used_mb  = (m->mem_total_kb - m->mem_avail_kb) / 1024;
                         long total_mb =  m->mem_total_kb / 1024;
                         char mem_lbl[32];
                         snprintf(mem_lbl, sizeof(mem_lbl), "  %ld/%ld MB ", used_mb, total_mb);
                         draw_bar(win, row++, 2, mem_lbl, 10, m->mem_pct, bar_w);
                         row++;

                         /* ── Load average section ── */
                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, row++, 2, "LOAD AVERAGE");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

                         mvwprintw(win, row++, 2, "   1m: %.2f    5m: %.2f    15m: %.2f",
                                   m->load_1m, m->load_5m, m->load_15m);
                         row++;

                         /* ── Uptime ── */
                         char upbuf[64];
                         fmt_uptime(m->uptime_sec, upbuf, sizeof(upbuf));
                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, row++, 2, "UPTIME");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, row++, 2, "   %s", upbuf);
                         row++;

                         /* ── footer ── */
                         wattron(win, A_DIM);
                         mvwprintw(win, row, 2, "q - quit  |  connected to %s:%d", SERVER_IP, PORT);
                         wattroff(win, A_DIM);

                         wrefresh(win);
                     }

                     /* ── reconnecting screen ──────────────────────────────────────────────────── */
                     static void render_reconnecting(WINDOW *win, int attempt) {
                         int rows, cols;
                         getmaxyx(win, rows, cols);

                         wclear(win);
                         box(win, 0, 0);

                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, 0, (cols - 34) / 2,
                                   "  NixMon - Real-Time System Monitor  ");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

                         wattron(win, COLOR_PAIR(COLOR_HIGH) | A_BOLD);
                         mvwprintw(win, rows / 2 - 1, (cols - 24) / 2, "  Server disconnected!  ");
                         wattroff(win, COLOR_PAIR(COLOR_HIGH) | A_BOLD);

                         wattron(win, COLOR_PAIR(COLOR_MED));
                         mvwprintw(win, rows / 2 + 1, (cols - 36) / 2,
                                   "  Reconnecting to %s:%d  (attempt %d)  ",
                                   SERVER_IP, PORT, attempt);
                         wattroff(win, COLOR_PAIR(COLOR_MED));

                         wattron(win, A_DIM);
                         mvwprintw(win, rows / 2 + 3, (cols - 16) / 2, "  q — quit  ");
                         wattroff(win, A_DIM);

                         wrefresh(win);
                     }

                     /* ── main ─────────────────────────────────────────────────────────────────── */
                     int main(void) {
                         /* ── ncurses init ── */
                         initscr();
                         cbreak();
                         noecho();
                         curs_set(0);
                         keypad(stdscr, TRUE);
                         timeout(0);   /* non-blocking getch */

                         if (!has_colors()) {
                             endwin();
                             fprintf(stderr, "Terminal does not support colors.\n");
                             return 1;
                         }
                         start_color();
                         init_pair(COLOR_LOW,    COLOR_GREEN,  COLOR_BLACK);
                         init_pair(COLOR_MED,    COLOR_YELLOW, COLOR_BLACK);
                         init_pair(COLOR_HIGH,   COLOR_RED,    COLOR_BLACK);
                         init_pair(COLOR_TITLE,  COLOR_CYAN,   COLOR_BLACK);
                         init_pair(COLOR_BORDER, COLOR_WHITE,  COLOR_BLACK);

                         int rows, cols;
                         getmaxyx(stdscr, rows, cols);
                         WINDOW *win = newwin(rows, cols, 0, 0);

                         /* ── connect to server ── */
                         int fd = connect_to_server(SERVER_IP, PORT);
                         if (fd < 0) {
                             endwin();
                             fprintf(stderr, "Could not connect to %s:%d — is nixmon-server running?\n",
                                     SERVER_IP, PORT);
                             return 1;
                         }

                         char buf[BUFFER_SIZE];
                         int  buf_len = 0;
                         SystemMetrics m = {0};
                         int quit = 0;
                         int attempt = 1;

                         /* show a "waiting" message until first frame arrives */
                         wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         mvwprintw(win, rows / 2, (cols - 28) / 2, "Waiting for first metrics...");
                         wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                         wrefresh(win);

                         while (!quit) {
                             /* ── reconnect loop ── */
                             while (fd < 0 && !quit) {
                                 render_reconnecting(win, attempt++);

                                 /* wait 2 seconds, but keep checking for 'q' */
                                 for (int i = 0; i < 200 && !quit; i++) {
                                     int ch = getch();
                                     if (ch == 'q' || ch == 'Q') { quit = 1; break; }
                                     usleep(10000);   /* 10ms × 200 = 2s */
                                 }
                                 if (quit) break;

                                 fd = connect_to_server(SERVER_IP, PORT);
                                 if (fd >= 0) {
                                     attempt = 1;
                                     buf_len = 0;
                                 }
                             }
                             if (quit) break;

                             /* ── keyboard input ── */
                             int ch = getch();
                             if (ch == 'q' || ch == 'Q') break;

                             /* ── receive data ── */
                             int n = recv(fd, buf + buf_len, sizeof(buf) - buf_len - 1, MSG_DONTWAIT);
                             if (n > 0) {
                                 buf_len += n;
                                 buf[buf_len] = '\0';

                                 char *line_start = buf;
                                 char *newline;
                                 while ((newline = strchr(line_start, '\n')) != NULL) {
                                     *newline = '\0';
                                     parse_metrics(line_start, &m);
                                     render(win, &m);
                                     line_start = newline + 1;
                                 }

                                 buf_len = strlen(line_start);
                                 memmove(buf, line_start, buf_len);

                             } else if (n == 0) {
                                 /* server closed connection — trigger reconnect */
                                 close(fd);
                                 fd = -1;
                             }

                             /* ── ~60 fps loop ── */
                             usleep(16000);
                         }

                         if (fd >= 0) close(fd);
                         delwin(win);
                         endwin();
                         printf("NixMon exited.\n");
                         return 0;
                     }
