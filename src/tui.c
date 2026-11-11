#include <signal.h>
#include "tui.h"
#include "subagents.h"
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

volatile sig_atomic_t km_ui_interrupted;
static const char *const tabs[] = {"MCPs", "Skills", "Subagents", "Clients", "Status"};

static void draw_tabs(KmUi *ui) {
    int x = 2;
    for (int i = 0; i < UI_TAB_COUNT; i++) {
        if (i == ui->tab) attron(A_BOLD | A_UNDERLINE | (ui->colors ? COLOR_PAIR(1) : 0));
        mvprintw(3, x, "%d %s", i + 1, tabs[i]);
        if (i == ui->tab) attroff(A_BOLD | A_UNDERLINE | (ui->colors ? COLOR_PAIR(1) : 0));
        x += (int)strlen(tabs[i]) + 5;
    }
    mvhline(4, 2, ACS_HLINE, COLS - 4);
}

static size_t row_member_count(KmUi *ui, const KmUiRow *row) {
    if (row->type == UI_GROUP) return json_object_array_length(km_get(ui_group_item(ui, row->id), "members"));
    size_t count = 0;
    json_object_object_foreach(km_resources(ui->km, ui_kind(ui)), id, item) {
        (void)item; bool grouped = false;
        json_object_object_foreach(km_get(ui->km->catalog, "groups"), key, group) {
            (void)key;
            const char *kind = km_string(group, "kind");
            if (!kind || strcmp(kind, ui_kind(ui))) continue;
            json_object *members = km_get(group, "members");
            char reference[80]; snprintf(reference, sizeof(reference), "%s:%s", kind, id);
            for (size_t m = 0; m < json_object_array_length(members); m++)
                if (!strcmp(json_object_get_string(json_object_array_get_idx(members, m)), reference)) grouped = true;
        }
        if (!grouped) count++;
    }
    return count;
}

static char group_client_state(KmUi *ui, json_object *group, const char *agent) {
    json_object *members = km_get(group, "members"); size_t on = 0, count = json_object_array_length(members);
    for (size_t i = 0; i < count; i++) {
        const char *reference = json_object_get_string(json_object_array_get_idx(members, i));
        const char *id = strchr(reference, ':');
        if (id && km_effective(ui->km, ui_kind(ui), id + 1, agent, NULL, 0)) on++;
    }
    return !on ? ' ' : on == count ? 'x' : '-';
}

static void draw_row(KmUi *ui, int i, int y) {
    const KmUiRow *row = &ui->rows[i]; const char *id = row->id;
    json_object *item = row->type == UI_GROUP ? ui_group_item(ui, id) : ui_item(ui, id);
    json_object *flag = km_get(item, "enabled");
    bool enabled = !flag || json_object_get_boolean(flag);
    int style = i == ui->row ? A_REVERSE | A_BOLD : 0;
    if (row->type != UI_RESOURCE) style |= A_BOLD | (ui->colors ? COLOR_PAIR(1) : 0);
    if (style) attron(style);
    mvhline(y, 2, ' ', COLS - 4);
    char key[80]; ui_mark_key(row, key, sizeof(key));
    if (row->type == UI_RESOURCE)
        mvprintw(y, 2, "%c %c   %-22.22s %-5s", i == ui->row ? '>' : ' ', km_get(ui->marks, key) ? '*' : ' ', id, enabled ? "ON" : "OFF");
    else {
        char label[96]; snprintf(label, sizeof(label), "%c %s (%zu)", ui_is_collapsed(ui, row) ? '>' : 'v', id, row_member_count(ui, row));
        bool mixed = false;
        for (int a = 0; row->type == UI_GROUP && a < KM_AGENT_COUNT; a++)
            if (group_client_state(ui, item, km_agents[a]) == '-') mixed = true;
        mvprintw(y, 2, "%c %c %-26.26s %-5s", i == ui->row ? '>' : ' ', km_get(ui->marks, key) ? '*' : ' ', label,
                   row->type == UI_UNGROUPED ? "" : !enabled ? "OFF" : mixed ? "MIX" : "ON");
    }
    for (int a = 0; row->type != UI_UNGROUPED && a < KM_AGENT_COUNT; a++) {
        char marker = row->type == UI_GROUP ? group_client_state(ui, item, km_agents[a]) :
                      km_effective(ui->km, ui_kind(ui), id, km_agents[a], NULL, 0) ? 'x' : ' ';
        if (row->type == UI_RESOURCE && ui->tab == UI_SUBAGENTS && !km_subagent_compatible(item, km_agents[a], NULL, 0)) marker = '!';
        mvprintw(y, 40 + a * 10, "[%c]", marker);
    }
    if (style) attroff(style);
}

static int draw_resource_clients(KmUi *ui, json_object *item, const char *id, int start) {
    for (int a = 0; a < KM_AGENT_COUNT && start < LINES - 5; a++) {
        char reason[192] = "";
        bool compatible = ui->tab != UI_SUBAGENTS || km_subagent_compatible(item, km_agents[a], reason, sizeof(reason));
        const char *state = "N/A";
        if (compatible) state = km_effective(ui->km, ui_kind(ui), id, km_agents[a], reason, sizeof(reason)) ? "ON" : "OFF";
        mvprintw(start++, 4, "%-9s %-3s  %.52s", km_agents[a], state, reason);
    }
    return start;
}

static void draw_details(KmUi *ui, int start) {
    const char *id = ui_current(ui);
    json_object *item = ui_current_item(ui);
    if (!id || !item || start >= LINES - 6) return;
    mvhline(start++, 2, ACS_HLINE, COLS - 4);
    attron(A_BOLD); mvprintw(start++, 2, "%s", id); attroff(A_BOLD);
    if (ui_group_selected(ui)) {
        mvaddnstr(start++, 4, "Clients show effective member availability; [-] means mixed.", COLS - 8);
        if (start < LINES - 5) mvaddnstr(start++, 4, "Other groups can enable members. Explicit/global OFF still wins.", COLS - 8);
        if (start < LINES - 5) mvaddnstr(start, 4, "m: choose members   a: group clients   e: group availability", COLS - 8);
    } else start = draw_resource_clients(ui, item, id, start);
    if (ui->tab == UI_SUBAGENTS && start < LINES - 5)
        mvaddnstr(start, 4, "Native reload required after changes; running work stays native.", COLS - 8);
}

static void draw_clients(KmUi *ui) {
    for (int i = 0; i < KM_AGENT_COUNT; i++) {
        attron(A_BOLD); mvprintw(6 + i * 3, 3, "%s", km_agents[i]); attroff(A_BOLD);
        mvprintw(7 + i * 3, 5, "Separate configuration, resource availability, and permissions");
    }
    mvprintw(17, 3, "s: preview native changes   i: preview import   c: run doctor");
    (void)ui;
}

void ui_refresh_status(KmUi *ui) {
    char *args[] = {"status"};
    if (ui_command(ui, 1, args)) return;
    snprintf(ui->snapshot, sizeof(ui->snapshot), "%s", ui->output);
    ui->snapshot_at = time(NULL); ui->snapshot_top = 0; ui->snapshot_truncated = ui->truncated;
    ui_notice(ui, "Captured runtime observation. Native client refresh remains unverified.");
}

static void draw_status(KmUi *ui) {
    mvaddstr(6, 3, "Runtime snapshot | Catalog and upstream observation");
    char captured[32] = "not captured"; struct tm local;
    if (ui->snapshot_at && localtime_r(&ui->snapshot_at, &local)) strftime(captured, sizeof(captured), "%H:%M:%S", &local);
    mvprintw(7, 3, "Captured %s   f: capture new observation   j/k: scroll", captured);
    mvhline(8, 3, ACS_HLINE, COLS - 6);
    ui->snapshot_more = *ui_text_lines(*ui->snapshot ? ui->snapshot : "No observation available. Press f to retry.", 9, LINES - 15, ui->snapshot_top) != '\0';
    mvaddnstr(LINES - 5, 3, ui->snapshot_truncated ? "Snapshot truncated. Use kitmux status for full diagnostics." : "Upstream observation only; native client refresh is unverified.", COLS - 6);
}

void ui_draw(KmUi *ui) {
    erase();
    if (COLS < 74 || LINES < 20) {
        mvaddnstr(0, 0, "Kitmux needs a terminal of at least 74 columns x 20 rows. q exits.", COLS - 1);
        refresh(); return;
    }
    attron(A_BOLD | (ui->colors ? COLOR_PAIR(1) : 0)); mvaddstr(1, 2, "kitmux");
    attroff(A_BOLD | (ui->colors ? COLOR_PAIR(1) : 0));
    mvaddstr(1, 18, "MCPs, skills, and subagents. Your choice of clients.");
    draw_tabs(ui);
    if (ui->tab < UI_CLIENTS) {
        mvprintw(6, 6, "%-24s %-5s  CODEX     CLAUDE    OPENCODE", tabs[ui->tab], "STATE");
        int visible = ui->details ? (LINES - 15) / 2 : LINES - 13;
        if (ui->row >= ui->top + visible) ui->top = ui->row - visible + 1;
        for (int i = ui->top; i < ui->count && i < ui->top + visible; i++) draw_row(ui, i, 8 + i - ui->top);
        if (!ui->count) mvprintw(9, 4, "Nothing here yet. Press n to add, or i to import.");
        if (ui->details) draw_details(ui, 8 + visible);
    } else if (ui->tab == UI_CLIENTS) draw_clients(ui); else draw_status(ui);
    if (*ui->filter && ui->tab < UI_CLIENTS) mvprintw(LINES - 5, 2, "Filter: %.100s", ui->filter);
    mvhline(LINES - 4, 2, ACS_HLINE, COLS - 4);
    const char *actions = ui->tab == UI_STATUS ? "j/k scroll  f capture observation  s sync  c doctor" :
                          "Enter expand/details  Space select  e toggle  a clients  m groups/members";
    const char *navigation = ui->tab == UI_STATUS ? "1-5 tabs  ? help  q quit" :
                             "g group  n add  x edit  d remove  / search  1-5 tabs  ? help  q quit";
    mvaddnstr(LINES - 3, 2, actions, COLS - 4);
    mvaddnstr(LINES - 2, 2, navigation, COLS - 4);
    if (*ui->notice) { attron(A_BOLD); mvaddnstr(LINES - 1, 2, ui->notice, COLS - 4); attroff(A_BOLD); }
    refresh();
}

static void interrupted(int signal_number) { (void)signal_number; km_ui_interrupted = 1; }

static void change_tab(KmUi *ui, int tab) {
    ui_free_rows(ui); ui->tab = tab; ui->row = ui->top = 0; ui->filter[0] = '\0';
    json_object_put(ui->marks); ui->marks = json_object_new_object(); ui_rebuild(ui);
    if (tab == UI_STATUS) ui_refresh_status(ui);
}

static void handle_key(KmUi *ui, int key) {
    if ((COLS < 74 || LINES < 20) && key != 'q') return;
    if (key == 'q') ui->done = true;
    else if (key >= '1' && key <= '5') change_tab(ui, key - '1');
    else if (key == '\t') change_tab(ui, (ui->tab + 1) % UI_TAB_COUNT);
    else if (key == KEY_DOWN || key == 'j') { if (ui->tab == UI_STATUS) { if (ui->snapshot_more) ui->snapshot_top++; } else if (ui->row + 1 < ui->count) ui->row++; }
    else if (key == KEY_UP || key == 'k') { if (ui->tab == UI_STATUS) { if (ui->snapshot_top) ui->snapshot_top--; } else if (ui->row > 0) ui->row--; }
    else if (key == '\n' || key == KEY_ENTER) {
        if (ui_resource_selected(ui)) ui->details = !ui->details;
        else if (ui_current_row(ui)) ui_expand(ui, ui_is_collapsed(ui, ui_current_row(ui)));
    } else if (key == KEY_LEFT || key == KEY_RIGHT) ui_expand(ui, key == KEY_RIGHT);
    else if (key == 'v') ui->details = !ui->details;
    else if (key == ' ' && ui_current(ui)) {
        char id[80]; ui_mark_key(ui_current_row(ui), id, sizeof(id));
        if (km_get(ui->marks, id)) json_object_object_del(ui->marks, id);
        else json_object_object_add(ui->marks, id, json_object_new_boolean(true));
    } else if (key == '/') {
        ui_input(ui, "Filter resources", "Search", ui->filter, sizeof(ui->filter));
        ui->row = ui->top = 0; ui_rebuild(ui);
    } else if (key == 27) { ui->filter[0] = '\0'; ui->details = false; ui_notice(ui, "Filter cleared."); ui_rebuild(ui); }
    else if (key != ERR && key != KEY_RESIZE) ui_action(ui, key);
}

static int prepare_groups(KmUi *ui) {
    if (!km_groups_need_migration(ui->km)) return 0;
    char *args[] = {"group", "migrate", "--preview"};
    int result = ui_command(ui, 3, args);
    if (!result && !ui_review(ui, "Migrate groups into resource tabs")) { ui->done = true; return 0; }
    if (!result) { args[2] = "--apply"; result = ui_command(ui, 3, args); }
    if (result) {
        snprintf(ui->output, sizeof(ui->output), "%s", ui->notice);
        ui_view(ui, "Group migration failed"); ui->done = true;
    }
    return result;
}

int km_tui(Km *km) {
    const char *term = getenv("TERM");
    if (!isatty(STDIN_FILENO) || !term || !strcmp(term, "dumb")) { km_help(stdout); return 0; }
    struct termios original;
    if (tcgetattr(STDIN_FILENO, &original) < 0) return km_error(km, "Cannot read terminal state");
    setlocale(LC_ALL, "");
    KmUi ui = {.km = km, .marks = json_object_new_object(), .collapsed = json_object_new_object()};
    if (!initscr()) { json_object_put(ui.marks); json_object_put(ui.collapsed); return km_error(km, "Cannot initialize the terminal"); }
    cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(100);
    ui.colors = !getenv("NO_COLOR") && has_colors();
    if (ui.colors) { start_color(); use_default_colors(); init_pair(1, COLORS >= 256 ? 208 : COLOR_YELLOW, -1); }
    struct sigaction action = {.sa_handler = interrupted}, old_int, old_term;
    sigemptyset(&action.sa_mask); km_ui_interrupted = 0;
    sigaction(SIGINT, &action, &old_int); sigaction(SIGTERM, &action, &old_term);
    int result = prepare_groups(&ui);
    ui_rebuild(&ui);
    while (!ui.done && !km_ui_interrupted) { ui_draw(&ui); handle_key(&ui, getch()); }
    endwin();
    int restored;
    do { restored = tcsetattr(STDIN_FILENO, TCSANOW, &original); } while (restored < 0 && errno == EINTR);
    sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL);
    ui_free_rows(&ui); json_object_put(ui.marks); json_object_put(ui.collapsed);
    return restored < 0 ? km_error(km, "Cannot restore terminal state") : result;
}
