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

void ui_add_group(KmUi *ui, bool editing) {
    if (ui->tab >= UI_CLIENTS || (editing && !ui_group_selected(ui))) return;
    char id[65] = "";
    if (editing && !ui_current(ui)) return;
    char *old = editing ? strdup(ui_current(ui)) : NULL;
    if (editing && !old) return;
    if (editing) snprintf(id, sizeof(id), "%s", old);
    if (ui_input(ui, editing ? "Rename group" : "Add group", "Group name", id, sizeof(id)) && *id) {
        char *args[] = {(char *)ui_kind(ui), "group", editing ? "rename" : "add", editing ? old : id, id};
        if (!ui_command(ui, editing ? 5 : 4, args)) {
            snprintf(ui->focus, sizeof(ui->focus), "%s", id); ui->focus_group = true;
        }
    }
    free(old); ui_rebuild(ui);
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

static bool mcp_description(KmUi *ui, bool editing, char *value, size_t size, char **option) {
    const char *choices[] = {editing ? "Keep current description" : "Skip description for now",
                            "Enter a short description", "Read a Markdown file", "Clear description"};
    int selected = ui_choice("MCP description", choices, 4, 0);
    if (selected < 0) return false;
    if (!selected) return true;
    *option = selected == 2 ? "--description-file" : "--description";
    if (selected == 3) return true;
    if (!ui_input(ui, "MCP description", selected == 2 ? "Markdown file" : "Description text", value, size)) return false;
    if (selected == 2 && !*value) { ui_notice(ui, "Choose a Markdown file or skip the description."); return false; }
    return true;
}

static void mcp_form(KmUi *ui, bool editing) {
    char id[65] = "", transport[16] = "stdio", command[PATH_MAX] = "", arguments[4096] = "";
    char description[PATH_MAX] = "", *description_option = NULL;
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
    char original_command[sizeof(command)], original_arguments[sizeof(arguments)];
    memcpy(original_command, command, sizeof(command));
    memcpy(original_arguments, arguments, sizeof(arguments));
    bool original_http = !strcmp(transport, "http"), original_managed = managed;
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
    if (!mcp_description(ui, editing, description, sizeof(description), &description_option)) return;
    /* Accepting defaults must retain each imported client's own execution. */
    bool execution_changed = !editing || http != original_http || strcmp(command, original_command) ||
                             (!http && strcmp(arguments, original_arguments));
    char *args[140] = {"mcp", editing ? "edit" : "add", id}; int count = 3;
    if (execution_changed) { args[count++] = http ? "--url" : "--command"; args[count++] = command; }
    if (!editing || (!http && managed) != original_managed) {
        args[count++] = "--mode"; args[count++] = !http && managed ? "managed" : "direct";
    }
    if (description_option) { args[count++] = description_option; args[count++] = description; }
    if (!http && execution_changed) {
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
    if (ui->tab != UI_SUBAGENTS || !ui_resource_selected(ui)) return;
    char *id = strdup(ui_current(ui)); char name[65] = "";
    if (!id) return;
    if (ui_input(ui, "Duplicate subagent", "New subagent name", name, sizeof(name)) && *name) {
        char *args[] = {"subagent", "duplicate", id, name};
        ui_command(ui, 4, args);
    }
    free(id); ui_rebuild(ui);
}

void ui_add(KmUi *ui, bool editing) {
    if (ui->tab >= UI_CLIENTS || (editing && !ui_resource_selected(ui))) return;
    const KmUiRow *selected = ui_current_row(ui);
    char *group = selected && selected->group ? strdup(selected->group) : NULL;
    json_object *before = json_object_get(km_resources(ui->km, ui_kind(ui)));
    if (ui->tab == UI_MCPS) mcp_form(ui, editing);
    else if (ui->tab == UI_SKILLS && !editing) skill_form(ui);
    else if (ui->tab == UI_SUBAGENTS) subagent_form(ui, editing);
    else ui_notice(ui, "Select MCPs, Skills, or Subagents to add an item.");
    json_object_object_foreach(km_resources(ui->km, ui_kind(ui)), id, item) {
        (void)item;
        if (km_get(before, id)) continue;
        snprintf(ui->focus, sizeof(ui->focus), "%s", id);
        if (group) {
            char *args[] = {(char *)ui_kind(ui), "group", "include", group, ui->focus};
            ui_command(ui, 5, args);
            char key[80]; snprintf(key, sizeof(key), "%s:%s", ui_kind(ui), group);
            json_object_object_del(ui->collapsed, key);
        }
        break;
    }
    json_object_put(before); free(group);
    ui_rebuild(ui);
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
    if (ui->tab != UI_MCPS || !ui_resource_selected(ui)) { ui_notice(ui, "Select an MCP to reconnect."); return; }
    int agent = ui_choice("Reconnect agent", km_agents, KM_AGENT_COUNT, 0);
    if (agent < 0) return;
    char *id = strdup(ui_current(ui));
    if (!id) return;
    char *args[] = {"mcp", "reconnect", id, "--agent", (char *)km_agents[agent]};
    ui_command(ui, 5, args);
    if (!*ui->output) snprintf(ui->output, sizeof(ui->output), "%s", ui->notice);
    ui_view(ui, "Reconnect result"); free(id);
}
