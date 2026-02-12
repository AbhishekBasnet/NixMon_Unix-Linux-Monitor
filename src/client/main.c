#include <ncurses.h>
#include <stdlib.h>

int main() {
    initscr();
    if (stdscr == NULL) {
        fprintf(stderr, "Error initializing ncurses.\n");
        exit(1);
    }

    cbreak();
    noecho();
    curs_set(0);

    refresh();

    WINDOW *win = newwin(10, 40, 1, 1);

    if (win == NULL) {
        endwin();
        fprintf(stderr, "Error creating window.\n");
        exit(1);
    }

    box(win, 0, 0);
    mvwprintw(win, 1, 2, "NixMon Client - v0.1");
    mvwprintw(win, 3, 2, "Status: Connected");
    mvwprintw(win, 5, 2, "Press ANY KEY to exit...");

    wrefresh(win);

    getch();

    delwin(win);
    endwin();

    return 0;
}
