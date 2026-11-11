#include "tui.h"
#include <stdlib.h>
#include <string.h>

static bool member_exists(json_object *group, const char *kind, const char *id) {
    char reference[80]; snprintf(reference, sizeof(reference), "%s:%s", kind, id);
    json_object *members = km_get(group, "members");
    for (size_t i = 0; members && i < json_object_array_length(members); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(members, i)), reference)) return true;
    return false;
}

static int compare_names(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static size_t collect_names(KmUi *ui, char **names, bool group) {
    json_object *items = group ? km_resources(ui->km, ui_kind(ui)) : km_get(ui->km->catalog, "groups");
    size_t count = 0;
    json_object_object_foreach(items, id, item) {
        const char *kind = km_string(item, "kind");
        if (!group && (!kind || strcmp(kind, ui_kind(ui)))) continue;
        const char *name = group ? id : km_string(item, "name");
        if (!name) continue;
        names[count] = strdup(name);
        if (!names[count]) break;
        count++;
    }
    qsort(names, count, sizeof(*names), compare_names);
    return count;
}

static bool select_members(KmUi *ui, char **names, bool *choices, size_t count, bool group) {
    size_t row = 0, top = 0;
    while (!km_ui_interrupted) {
        if (!ui_modal_ready()) return false;
        erase(); attron(A_BOLD); mvaddstr(2, 3, group ? "Group members" : "Item groups"); attroff(A_BOLD);
        mvaddstr(4, 3, "Space toggles membership. Enter saves. Esc cancels.");
        size_t visible = LINES > 9 ? (size_t)LINES - 9 : 1;
        if (row < top) top = row;
        if (row >= top + visible) top = row - visible + 1;
        for (size_t i = top; i < count && i < top + visible; i++)
            mvprintw(6 + (int)(i - top), 3, "%c [%c] %.*s", row == i ? '>' : ' ', choices[i] ? 'x' : ' ', COLS - 10, names[i]);
        if (!count) mvaddstr(6, 3, group ? "Add a resource in this tab first." : "Press g in the resource list to create a group.");
        refresh(); int key = getch();
        if (key == 27) return false;
        if (key == '\n' || key == KEY_ENTER) return true;
        if (key == ' ' && count) choices[row] = !choices[row];
        if ((key == 'j' || key == KEY_DOWN) && row + 1 < count) row++;
        if ((key == 'k' || key == KEY_UP) && row) row--;
    }
    (void)ui; return false;
}

static void apply_memberships(KmUi *ui, const char *current, char **names, bool *before,
                              bool *choices, size_t count, bool group) {
    for (size_t i = 0; i < count; i++) {
        if (before[i] == choices[i]) continue;
        char *args[] = {(char *)ui_kind(ui), "group", choices[i] ? "include" : "exclude",
                        group ? (char *)current : names[i], group ? names[i] : (char *)current};
        if (ui_command(ui, 5, args)) break;
    }
}

void ui_members(KmUi *ui) {
    if (!ui_current(ui) || ui->tab >= UI_CLIENTS) {
        ui_notice(ui, "Select a group or resource to change memberships."); return;
    }
    bool group = ui_group_selected(ui);
    char *current = strdup(ui_current(ui));
    json_object *items = group ? km_resources(ui->km, ui_kind(ui)) : km_get(ui->km->catalog, "groups");
    size_t capacity = (size_t)json_object_object_length(items) + 1;
    char **names = calloc(capacity, sizeof(*names));
    bool *choices = calloc(capacity, sizeof(*choices)), *before = calloc(capacity, sizeof(*before));
    if (!current || !names || !choices || !before) {
        free(current); free(names); free(choices); free(before);
        ui_notice(ui, "Cannot allocate membership selector."); return;
    }
    size_t count = collect_names(ui, names, group);
    for (size_t i = 0; i < count; i++) {
        json_object *definition = ui_group_item(ui, group ? current : names[i]);
        before[i] = choices[i] = member_exists(definition, ui_kind(ui), group ? names[i] : current);
    }
    if (select_members(ui, names, choices, count, group))
        apply_memberships(ui, current, names, before, choices, count, group);
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names); free(choices); free(before); free(current); ui_rebuild(ui);
}
