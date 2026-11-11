#include "tui.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int ui_tokens(char *text, char **argv, int capacity) {
    char *source = text, *target = text; int count = 0;
    while (*source) {
        while (isspace((unsigned char)*source)) source++;
        if (!*source) break;
        if (count == capacity) return -1;
        argv[count++] = target; char quote = 0;
        while (*source && (quote || !isspace((unsigned char)*source))) {
            char c = *source++;
            if (c == '\\' && quote != '\'') { if (!*source) return -1; *target++ = *source++; }
            else if ((c == '\'' || c == '"') && (!quote || c == quote)) quote = quote ? 0 : c;
            else *target++ = c;
        }
        if (quote) return -1;
        if (*source) source++;
        *target++ = '\0';
    }
    return count;
}

static void group_form(KmUi *ui, bool editing) {
    char id[65] = "";
    if (editing && !ui_current(ui)) return;
    char *old = editing ? strdup(ui_current(ui)) : NULL;
    if (editing && !old) return;
    if (editing) snprintf(id, sizeof(id), "%s", old);
    if (ui_input(ui, editing ? "Rename group" : "Add group", "Group name", id, sizeof(id)) && *id) {
        char *args[] = {"group", editing ? "rename" : "add", editing ? old : id, id};
        ui_command(ui, editing ? 4 : 3, args);
    }
    free(old);
}

static bool command_defaults(json_object *item, char *command, size_t size, char *arguments, size_t capacity) {
    json_object *array = km_get(item, "command");
    if (!array || !json_object_array_length(array)) return true;
    snprintf(command, size, "%s", json_object_get_string(json_object_array_get_idx(array, 0)));
    size_t used = 0;
    for (size_t i = 1; i < json_object_array_length(array); i++) {
        const char *arg = json_object_get_string(json_object_array_get_idx(array, i));
        if (used + 4 >= capacity) return false;
        arguments[used++] = '"';
        for (; *arg && used + 4 < capacity; arg++) { if (*arg == '"' || *arg == '\\') arguments[used++] = '\\'; arguments[used++] = *arg; }
        if (*arg) return false;
        arguments[used++] = '"'; arguments[used++] = ' '; arguments[used] = '\0';
    }
    return true;
}

static void mcp_form(KmUi *ui, bool editing) {
    char id[65] = "", transport[16] = "stdio", command[PATH_MAX] = "", arguments[4096] = "";
    bool managed = false;
    if (editing && !ui_current(ui)) return;
    if (editing) {
        snprintf(id, sizeof(id), "%s", ui_current(ui));
        json_object *item = ui_item(ui, id);
        const char *mode = km_string(item, "mode"); managed = mode && !strcmp(mode, "managed");
        const char *value = km_string(item, "transport"); if (value) snprintf(transport, sizeof(transport), "%s", value);
        value = km_string(item, "url"); if (value) snprintf(command, sizeof(command), "%s", value);
        else if (!command_defaults(item, command, sizeof(command), arguments, sizeof(arguments))) { ui_notice(ui, "Arguments exceed this form's limit; use mcp edit in the CLI."); return; }
    } else if (!ui_input(ui, "Add MCP", "MCP name", id, sizeof(id)) || !*id) return;
    if (!ui_input(ui, "MCP transport", "stdio or http", transport, sizeof(transport))) return;
    bool http = !strcmp(transport, "http");
    if (!http && strcmp(transport, "stdio")) { ui_notice(ui, "Transport must be stdio or http."); return; }
    if (!http) {
        const char *modes[] = {"direct - your coding agent owns the connection", "managed - live local process control (tools-only MCP)"};
        int choice = ui_choice("Connection mode", modes, 2, managed ? 1 : 0);
        if (choice < 0) return;
        managed = choice == 1;
    }
    if (!ui_input(ui, "MCP endpoint", http ? "HTTP URL" : "Executable path or command", command, sizeof(command)) || !*command) return;
    if (!http && !ui_input(ui, "MCP arguments", "Arguments (quotes supported; no shell evaluation)", arguments, sizeof(arguments))) return;
    char *args[138] = {"mcp", editing ? "edit" : "add", id, http ? "--url" : "--command", command,
                       "--mode", !http && managed ? "managed" : "direct"}; int count = 7;
    if (!http) {
        args[count++] = "--";
        int tokens = ui_tokens(arguments, args + count, 128);
        if (tokens < 0) { ui_notice(ui, "Unmatched quote or too many arguments."); return; }
        count += tokens;
    }
    ui_command(ui, count, args);
}

static void skill_form(KmUi *ui) {
    char source[PATH_MAX] = "", path[PATH_MAX] = "", revision[128] = "";
    if (!ui_input(ui, "Install skill", "Local directory, Git URL, or owner/repository", source, sizeof(source)) || !*source) return;
    if (!ui_input(ui, "Skill location", "Repository subdirectory (blank for root/local source)", path, sizeof(path))) return;
    if (!ui_input(ui, "Pin revision", "Git commit or tag (blank for repository default)", revision, sizeof(revision))) return;
    char *args[] = {"skill", "add", source, NULL, NULL, NULL, NULL}; int count = 3;
    if (*path) { args[count++] = "--path"; args[count++] = path; }
    if (*revision) { args[count++] = "--ref"; args[count++] = revision; }
    ui_command(ui, count, args);
}

static void subagent_form(KmUi *ui, bool editing) {
    char id[65] = "", source[PATH_MAX] = "", description[1024] = "";
    const char *formats[] = {"common - shared instructions", "codex - native TOML", "claude - native Markdown", "opencode - native Markdown", "opencode-json - native JSON/JSONC"};
    const char *values[] = {"common", "codex", "claude", "opencode", "opencode-json"};
    if (editing) {
        if (!ui_current(ui)) return;
        snprintf(id, sizeof(id), "%s", ui_current(ui));
    } else if (!ui_input(ui, "Create subagent", "Subagent name", id, sizeof(id)) || !*id) return;
    int format = ui_choice("Definition format", formats, 5, 0);
    if (format < 0 || !ui_input(ui, "Subagent source", "Definition file", source, sizeof(source)) || !*source) return;
    if (!format && (!ui_input(ui, "Subagent purpose", "Description", description, sizeof(description)) || !*description)) return;
    char *args[] = {"subagent", editing ? "edit" : "add", id, "--from", source,
                   "--format", (char *)values[format], "--description", description};
    ui_command(ui, format ? 7 : 9, args);
}

void ui_duplicate(KmUi *ui) {
    if (ui->tab != UI_SUBAGENTS || !ui_current(ui)) return;
    char *id = strdup(ui_current(ui)); char name[65] = "";
    if (!id) return;
    if (ui_input(ui, "Duplicate subagent", "New subagent name", name, sizeof(name)) && *name) {
        char *args[] = {"subagent", "duplicate", id, name};
        ui_command(ui, 4, args);
    }
    free(id); ui_rebuild(ui);
}

void ui_add(KmUi *ui, bool editing) {
    if (ui->tab == 0) group_form(ui, editing);
    else if (ui->tab == 1) mcp_form(ui, editing);
    else if (ui->tab == 2 && !editing) skill_form(ui);
    else if (ui->tab == UI_SUBAGENTS) subagent_form(ui, editing);
    else ui_notice(ui, "Select Groups, MCPs, Skills, or Subagents to add an item.");
    ui_rebuild(ui);
}

static bool member_exists(json_object *members, const char *id) {
    for (size_t i = 0; members && i < json_object_array_length(members); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(members, i)), id)) return true;
    return false;
}

static int collect_members(KmUi *ui, char **names, bool *choices, json_object *members) {
    int count = 0;
    const char *kinds[] = {"mcp", "skill", "subagent"};
    for (size_t type = 0; type < sizeof(kinds) / sizeof(*kinds); type++) {
        const char *kind = kinds[type];
        json_object_object_foreach(km_resources(ui->km, kind), id, item) {
            (void)item; size_t length = strlen(kind) + strlen(id) + 2;
            names[count] = malloc(length);
            if (!names[count]) return -1;
            snprintf(names[count], length, "%s:%s", kind, id);
            choices[count] = member_exists(members, names[count]); count++;
        }
    }
    return count;
}

static bool member_selector(KmUi *ui, char **names, bool *choices, int count) {
    int row = 0, top = 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return false;
        erase(); attron(A_BOLD); mvaddstr(2, 3, "Group membership"); attroff(A_BOLD);
        mvaddstr(4, 3, "Space toggles membership. Enter saves. Esc cancels.");
        int visible = LINES - 9;
        if (visible < 1) visible = 1;
        if (row < top) top = row;
        if (row >= top + visible) top = row - visible + 1;
        for (int i = top; i < count && i < top + visible; i++) mvprintw(6 + i - top, 3, "%c [%c] %.100s", row == i ? '>' : ' ', choices[i] ? 'x' : ' ', names[i]);
        if (!count) mvaddstr(6, 3, "Add an MCP, skill, or subagent first.");
        refresh(); int key = getch();
        if (key == 27) return false;
        if (key == '\n' || key == KEY_ENTER) return true;
        if (key == ' ' && count) choices[row] = !choices[row];
        if ((key == 'j' || key == KEY_DOWN) && row + 1 < count) row++;
        if ((key == 'k' || key == KEY_UP) && row) row--;
    }
    (void)ui; return false;
}

void ui_members(KmUi *ui) {
    if (ui->tab != 0 || !ui_current(ui)) { ui_notice(ui, "Select a group to change its members."); return; }
    char *group = strdup(ui_current(ui));
    size_t capacity = (size_t)json_object_object_length(km_resources(ui->km, "mcp")) + (size_t)json_object_object_length(km_resources(ui->km, "skill")) + (size_t)json_object_object_length(km_resources(ui->km, "subagent")) + 1;
    char **names = calloc(capacity, sizeof(*names)); bool *choices = calloc(capacity, sizeof(*choices));
    if (!group || !names || !choices) { free(group); free(names); free(choices); ui_notice(ui, "Cannot allocate membership selector."); return; }
    int count = collect_members(ui, names, choices, km_get(ui_item(ui, group), "members"));
    if (count >= 0 && member_selector(ui, names, choices, count)) {
        for (int i = 0; i < count; i++) {
            bool before = member_exists(km_get(ui_item(ui, group), "members"), names[i]);
            if (before == choices[i]) continue;
            char *args[] = {"group", choices[i] ? "include" : "exclude", group, names[i]};
            if (ui_command(ui, 4, args)) break;
        }
    }
    for (size_t i = 0; i < capacity; i++) free(names[i]); free(names); free(choices); free(group); ui_rebuild(ui);
}

void ui_sync(KmUi *ui, bool importing) {
    bool profiles = importing && ui->tab == UI_SUBAGENTS;
    char *args[] = {profiles ? "subagent" : importing ? "import" : "sync",
                    profiles ? "import" : "--preview", "--preview"};
    int count = profiles ? 3 : 2;
    const char *title = !importing ? "Review native configuration changes" :
                        profiles ? "Review subagent import" : "Review MCP, skill, and subagent import";
    if (!ui_command(ui, count, args) && ui_review(ui, title)) {
        args[count - 1] = "--apply"; ui_command(ui, count, args); ui_rebuild(ui);
    }
}

void ui_reconnect(KmUi *ui) {
    if (ui->tab != 1 || !ui_current(ui)) { ui_notice(ui, "Select an MCP to reconnect."); return; }
    int agent = ui_choice("Reconnect agent", km_agents, KM_AGENT_COUNT, 0);
    if (agent < 0) return;
    char *id = strdup(ui_current(ui));
    if (!id) return;
    char *args[] = {"mcp", "reconnect", id, "--agent", (char *)km_agents[agent]};
    ui_command(ui, 5, args);
    if (!*ui->output) snprintf(ui->output, sizeof(ui->output), "%s", ui->notice);
    ui_view(ui, "Reconnect result"); free(id);
}
