#include "tui.h"
#include "subagents.h"
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ui_notice(KmUi *ui, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(ui->notice, sizeof(ui->notice), format, arguments);
    va_end(arguments);
}

static void dialog_title(const char *title) {
    erase(); attron(A_BOLD); mvprintw(2, 3, "%s", title); attroff(A_BOLD);
    mvhline(3, 3, ACS_HLINE, COLS > 6 ? COLS - 6 : 1);
}

bool ui_modal_ready(void) {
    while (COLS < 74 || LINES < 20) {
        erase(); mvaddnstr(0, 0, "Resize to 74 columns x 20 rows. Esc cancels.", COLS > 0 ? COLS - 1 : 0); refresh();
        int key = getch();
        if (key == 27 || key == 'q' || km_ui_interrupted) return false;
    }
    return !km_ui_interrupted;
}

bool ui_input(KmUi *ui, const char *title, const char *label, char *value, size_t capacity) {
    size_t length = strlen(value);
    curs_set(1);
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) { curs_set(0); return false; }
        dialog_title(title); mvprintw(5, 3, "%.120s", label);
        mvaddnstr(7, 3, value, COLS > 8 ? COLS - 8 : 1);
        mvprintw(10, 3, "Enter confirms. Esc cancels."); refresh();
        int key = getch();
        if (key == 27) { curs_set(0); return false; }
        if (key == '\n' || key == KEY_ENTER) { curs_set(0); return true; }
        if (key == KEY_BACKSPACE || key == 127 || key == 8) { if (length) value[--length] = '\0'; }
        else if (key >= 32 && key <= 126 && length + 1 < capacity) { value[length++] = (char)key; value[length] = '\0'; }
    }
    curs_set(0); (void)ui; return false;
}

bool ui_confirm(KmUi *ui, const char *message) {
    if (!ui_modal_ready()) return false;
    dialog_title("Confirm change"); mvaddnstr(5, 3, message, COLS - 6);
    mvaddstr(8, 3, "y: confirm   any other key: cancel"); refresh();
    int key;
    do { key = getch(); } while (key == ERR && !km_ui_interrupted);
    (void)ui; return key == 'y' || key == 'Y';
}

int ui_choice(const char *title, const char *const *choices, int count, int initial) {
    int row = initial >= 0 && initial < count ? initial : 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return -1;
        dialog_title(title);
        for (int i = 0; i < count; i++) {
            if (i == row) attron(A_BOLD | A_REVERSE);
            mvprintw(6 + i * 2, 3, "%c %.*s", i == row ? '>' : ' ', COLS - 8, choices[i]);
            if (i == row) attroff(A_BOLD | A_REVERSE);
        }
        mvaddstr(7 + count * 2, 3, "j/k or arrows   Enter choose   Esc cancel"); refresh();
        int key = getch();
        if (key == 27) return -1;
        if (key == '\n' || key == KEY_ENTER) return row;
        if (key == 'j' || key == KEY_DOWN) row = (row + 1) % count;
        if (key == 'k' || key == KEY_UP) row = (row + count - 1) % count;
    }
    return -1;
}

int ui_command(KmUi *ui, int argc, char **argv) {
    ui->output[0] = '\0'; ui->truncated = false;
    FILE *capture = tmpfile();
    if (!capture) { ui_notice(ui, "Cannot capture command output: %s", strerror(errno)); return -1; }
    fflush(stdout); fflush(stderr);
    int out = dup(STDOUT_FILENO), err = dup(STDERR_FILENO);
    if (out < 0 || err < 0) {
        int saved = errno;
        if (out >= 0) close(out); if (err >= 0) close(err); fclose(capture);
        ui_notice(ui, "Cannot capture command output: %s", strerror(saved)); return -1;
    }
    if (dup2(fileno(capture), STDOUT_FILENO) < 0 || dup2(fileno(capture), STDERR_FILENO) < 0) {
        int saved = errno;
        dup2(out, STDOUT_FILENO); dup2(err, STDERR_FILENO); close(out); close(err); fclose(capture);
        ui_notice(ui, "Cannot redirect command output: %s", strerror(saved)); return -1;
    }
    ui->km->error[0] = '\0';
    int result = km_dispatch(ui->km, argc, argv);
    fflush(stdout); fflush(stderr); dup2(out, STDOUT_FILENO); dup2(err, STDERR_FILENO); close(out); close(err);
    rewind(capture);
    size_t length = fread(ui->output, 1, sizeof(ui->output) - 1, capture);
    ui->output[length] = '\0'; ui->truncated = fgetc(capture) != EOF;
    for (size_t i = 0; i < length; i++) if (!isprint((unsigned char)ui->output[i]) && ui->output[i] != '\n' && ui->output[i] != '\t') ui->output[i] = ' ';
    char output[256] = "";
    size_t first = strcspn(ui->output, "\n");
    snprintf(output, sizeof(output), "%.*s", (int)(first < sizeof(output) - 1 ? first : sizeof(output) - 1), ui->output);
    fclose(capture);
    if (result) ui_notice(ui, "%.480s", *ui->km->error ? ui->km->error : "Action failed; run the CLI for details.");
    else ui_notice(ui, "%s", *output ? output : "Saved. Use s to adopt clients if not configured.");
    if (km_load(ui->km) < 0) ui_notice(ui, "%.480s", ui->km->error);
    return result;
}

static const char *text_line(const char *text, int row, int width) {
    size_t length = strcspn(text, "\n"), take = length < (size_t)width ? length : (size_t)width;
    if (take < length) {
        size_t space = take;
        while (space && text[space] != ' ') space--;
        if (space > (size_t)width / 2) take = space;
    }
    if (row >= 0) mvaddnstr(row, 3, text, (int)take);
    text += take;
    if (*text == '\n') text++;
    else if (take < length) while (*text == ' ') text++;
    return text;
}

const char *ui_text_lines(const char *text, int row, int rows, int offset) {
    int width = COLS > 6 ? COLS - 6 : 1;
    for (int i = 0; i < offset && *text; i++) text = text_line(text, -1, width);
    for (int i = 0; i < rows && *text; i++) text = text_line(text, row + i, width);
    return text;
}

static bool output_view(KmUi *ui, const char *title, bool applying) {
    int offset = 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return false;
        dialog_title(title);
        const char *line = ui_text_lines(ui->output, 5, LINES - 9, offset);
        const char *hint = applying ? "j/k scroll   Enter apply   Esc cancel" : "j/k scroll   Enter or Esc returns";
        mvaddnstr(LINES - 2, 3, ui->truncated ? "Output truncated. Inspect full output with the CLI. Esc returns." : hint, COLS - 6);
        refresh(); int key = getch();
        if (key == 27 || key == 'q') return false;
        if (key == '\n' || key == KEY_ENTER) { if (!applying) return false; if (!ui->truncated) return true; }
        if (key == KEY_DOWN || key == 'j') { if (*line) offset++; }
        if ((key == KEY_UP || key == 'k') && offset) offset--;
    }
    return false;
}

bool ui_review(KmUi *ui, const char *title) { return output_view(ui, title, true); }

void ui_view(KmUi *ui, const char *title) { (void)output_view(ui, title, false); }

static void toggle_items(KmUi *ui, bool removing) {
    if (ui->tab >= UI_CLIENTS || !ui_current(ui)) return;
    if (removing && !ui_confirm(ui, "Remove the selected catalog items? Sources are preserved.")) return;
    bool marked = json_object_object_length(ui->marks) > 0;
    for (int i = 0; i < ui->count; i++) {
        if ((marked && !km_get(ui->marks, ui->ids[i])) || (!marked && i != ui->row)) continue;
        json_object *flag = km_get(ui_item(ui, ui->ids[i]), "enabled");
        char *verb = removing ? "remove" : (!flag || json_object_get_boolean(flag) ? "disable" : "enable");
        char *args[] = {(char *)ui_kind(ui), verb, ui->ids[i]};
        if (ui_command(ui, 3, args)) break;
    }
    json_object_put(ui->marks); ui->marks = json_object_new_object(); ui_rebuild(ui);
}

static bool has_agent(json_object *array, const char *agent) {
    for (size_t i = 0; array && i < json_object_array_length(array); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(array, i)), agent)) return true;
    return false;
}

void ui_assign(KmUi *ui) {
    const char *current = ui_current(ui);
    if (ui->tab >= UI_CLIENTS || !current) return;
    char *id = strdup(current);
    if (!id) return;
    bool choices[KM_AGENT_COUNT]; int row = 0;
    for (int a = 0; a < KM_AGENT_COUNT; a++) choices[a] = ui->tab == 0 ? has_agent(km_get(ui_item(ui, id), "agents"), km_agents[a]) : km_effective(ui->km, ui_kind(ui), id, km_agents[a], NULL, 0);
    int key = 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) { free(id); return; }
        dialog_title("Assign agents"); mvprintw(5, 3, "%s", id);
        if (ui->tab) mvaddstr(6, 3, "Explicit overrides; use CLI inherit to follow groups again.");
        for (int a = 0; a < KM_AGENT_COUNT; a++) {
            bool compatible = ui->tab != UI_SUBAGENTS || km_subagent_compatible(ui_item(ui, id), km_agents[a], NULL, 0);
            mvprintw(7 + a, 3, "%c [%c] %s%s", row == a ? '>' : ' ', choices[a] ? 'x' : ' ', km_agents[a], compatible ? "" : " (native variant required)");
        }
        mvprintw(12, 3, "j/k move   Space toggle   Enter save   Esc cancel"); refresh(); key = getch();
        if (key == 27) { free(id); return; }
        if (key == '\n' || key == KEY_ENTER) break;
        if (key == KEY_DOWN || key == 'j') row = (row + 1) % KM_AGENT_COUNT;
        if (key == KEY_UP || key == 'k') row = (row + KM_AGENT_COUNT - 1) % KM_AGENT_COUNT;
        if (key == ' ') choices[row] = !choices[row];
    }
    if (km_ui_interrupted) { free(id); return; }
    int result = 0;
    if (ui->tab == 0) {
        char csv[64] = "";
        for (int a = 0; a < KM_AGENT_COUNT; a++) if (choices[a]) { if (*csv) strcat(csv, ","); strcat(csv, km_agents[a]); }
        char *args[] = {"group", "assign", id, "--agents", csv}; result = ui_command(ui, 5, args);
    } else for (int a = 0; a < KM_AGENT_COUNT && !result; a++) {
        char *args[] = {(char *)ui_kind(ui), choices[a] ? "enable" : "disable", id, "--agent", (char *)km_agents[a]};
        result = ui_command(ui, 5, args);
    }
    if (!result) ui_notice(ui, "Assignments saved. Adopt clients with s; agent reload may be required.");
    free(id); ui_rebuild(ui);
}

static void show_help(KmUi *ui) {
    if (!ui_modal_ready()) return;
    dialog_title("Kitmux keyboard shortcuts");
    const char *lines[] = {"1-6 / Tab   Groups, MCPs, Skills, Subagents, Clients, Status", "j/k / arrows   Move selection or scroll Status", "Enter   Toggle details     Space   Select multiple rows", "e   Enable/disable selected     a   Assign agents", "n Add  x Edit MCP/subagent or rename group  d Remove", "m   Group membership     /   Filter     Esc   Clear filter", "r   Reconnect MCP     s   Preview/adopt native configuration", "i Import (current Subagents tab: profiles only)  p Duplicate subagent", "u   Preview/apply skill update     b   Roll back skill", "f   Capture new Status observation     c   Doctor     q   Quit", "", "Adopt clients with s. Later changes sync adopted clients.", "Your coding agent may still need a refresh or reload.", "Press any key to return."};
    for (size_t i = 0; i < sizeof(lines) / sizeof(*lines) && (int)i + 5 < LINES - 1; i++) mvaddnstr(5 + (int)i, 3, lines[i], COLS - 6);
    refresh(); while (getch() == ERR && !km_ui_interrupted) {} (void)ui;
}

static void skill_action(KmUi *ui, bool rollback) {
    if (ui->tab != 2 || !ui_current(ui)) return;
    char *id = strdup(ui_current(ui));
    if (!id) return;
    char *args[] = {"skill", rollback ? "rollback" : "update", id, "--preview"};
    if (rollback) { if (ui_confirm(ui, "Restore this skill's previous pinned revision?")) ui_command(ui, 3, args); }
    else {
        if (!ui_command(ui, 4, args) && ui_review(ui, "Review skill update")) { args[3] = "--apply"; ui_command(ui, 4, args); }
    }
    free(id); ui_rebuild(ui);
}

void ui_action(KmUi *ui, int key) {
    if (key == 'e') toggle_items(ui, false);
    else if (key == 'd') toggle_items(ui, true);
    else if (key == 'a') ui_assign(ui);
    else if (key == 'm') ui_members(ui);
    else if (key == 'n' || key == 'x') ui_add(ui, key == 'x');
    else if (key == 'p') ui_duplicate(ui);
    else if (key == 'r') ui_reconnect(ui);
    else if (key == 'f' && ui->tab == UI_STATUS) ui_refresh_status(ui);
    else if (key == 's' || key == 'i') ui_sync(ui, key == 'i');
    else if (key == 'u' || key == 'b') skill_action(ui, key == 'b');
    else if (key == 'c') { char *args[] = {"doctor"}; ui_command(ui, 1, args); }
    else if (key == '?') show_help(ui);
}
