#include "tui.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *ui_kind(const KmUi *ui) {
    static const char *const kinds[] = {"mcp", "skill", "subagent", "client", "status"};
    return kinds[ui->tab];
}

const KmUiRow *ui_current_row(const KmUi *ui) {
    return ui->row >= 0 && ui->row < ui->count ? &ui->rows[ui->row] : NULL;
}

const char *ui_current(const KmUi *ui) {
    const KmUiRow *row = ui_current_row(ui);
    return row && row->type != UI_UNGROUPED ? row->id : NULL;
}

bool ui_resource_selected(const KmUi *ui) {
    const KmUiRow *row = ui_current_row(ui);
    return row && row->type == UI_RESOURCE;
}

bool ui_group_selected(const KmUi *ui) {
    const KmUiRow *row = ui_current_row(ui);
    return row && row->type == UI_GROUP;
}

json_object *ui_item(KmUi *ui, const char *id) {
    return id && ui->tab < UI_CLIENTS ? km_get(km_resources(ui->km, ui_kind(ui)), id) : NULL;
}

json_object *ui_group_item(KmUi *ui, const char *id) {
    return id && ui->tab < UI_CLIENTS ? km_group_get(ui->km, ui_kind(ui), id) : NULL;
}

json_object *ui_current_item(KmUi *ui) {
    return ui_group_selected(ui) ? ui_group_item(ui, ui_current(ui)) : ui_item(ui, ui_current(ui));
}

void ui_mark_key(const KmUiRow *row, char *key, size_t capacity) {
    snprintf(key, capacity, "%c:%s", row->type == UI_GROUP ? 'g' : 'r', row->id);
}

static void collapse_key(KmUi *ui, const KmUiRow *row, char *key, size_t capacity) {
    snprintf(key, capacity, "%s:%s", ui_kind(ui), row->type == UI_UNGROUPED ? "" : row->id);
}

bool ui_is_collapsed(KmUi *ui, const KmUiRow *row) {
    char key[80]; collapse_key(ui, row, key, sizeof(key));
    return !*ui->filter && km_get(ui->collapsed, key);
}

void ui_expand(KmUi *ui, bool expanded) {
    const KmUiRow *row = ui_current_row(ui);
    if (!row || row->type == UI_RESOURCE) return;
    if (*ui->filter) { ui_notice(ui, "Clear search to change group expansion."); return; }
    char key[80]; collapse_key(ui, row, key, sizeof(key));
    if (expanded) json_object_object_del(ui->collapsed, key);
    else json_object_object_add(ui->collapsed, key, json_object_new_boolean(true));
    ui_notice(ui, "%s %s.", row->id, expanded ? "expanded" : "collapsed");
    ui_rebuild(ui);
}

void ui_free_rows(KmUi *ui) {
    for (int i = 0; i < ui->count; i++) { free(ui->rows[i].id); free(ui->rows[i].group); }
    free(ui->rows); ui->rows = NULL; ui->count = ui->capacity = 0;
}

static bool matches(const char *id, const char *filter) {
    if (!*filter) return true;
    for (; *id; id++) {
        size_t i = 0;
        while (filter[i] && id[i] && tolower((unsigned char)filter[i]) == tolower((unsigned char)id[i])) i++;
        if (!filter[i]) return true;
    }
    return false;
}

static int compare_ids(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static char **sorted_names(json_object *items, const char *kind, size_t *count) {
    char **names = calloc((size_t)json_object_object_length(items) + 1, sizeof(*names));
    *count = 0;
    if (!names) return NULL;
    json_object_object_foreach(items, id, item) {
        if (kind && (!km_string(item, "kind") || strcmp(km_string(item, "kind"), kind))) continue;
        const char *name = kind ? km_string(item, "name") : id;
        if (!name) continue;
        names[*count] = strdup(name);
        if (!names[*count]) break;
        (*count)++;
    }
    qsort(names, *count, sizeof(*names), compare_ids);
    return names;
}

static void free_names(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static bool append_row(KmUi *ui, KmUiRowType type, const char *id, const char *group) {
    if (ui->count == ui->capacity) {
        int capacity = ui->capacity ? ui->capacity * 2 : 16;
        KmUiRow *rows = realloc(ui->rows, (size_t)capacity * sizeof(*rows));
        if (!rows) { ui_notice(ui, "Cannot allocate resource list"); return false; }
        ui->rows = rows; ui->capacity = capacity;
    }
    KmUiRow row = {.type = type, .id = strdup(id), .group = group ? strdup(group) : NULL};
    if (!row.id || (group && !row.group)) { free(row.id); free(row.group); return false; }
    ui->rows[ui->count++] = row;
    return true;
}

static bool contains(json_object *members, const char *kind, const char *id) {
    char reference[80]; snprintf(reference, sizeof(reference), "%s:%s", kind, id);
    for (size_t i = 0; members && i < json_object_array_length(members); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(members, i)), reference)) return true;
    return false;
}

static void append_group(KmUi *ui, const char *name, char **names, size_t count, json_object *grouped) {
    json_object *group = ui_group_item(ui, name), *members = km_get(group, "members");
    bool group_match = matches(name, ui->filter), child_match = false;
    for (size_t i = 0; i < count; i++) {
        if (!contains(members, ui_kind(ui), names[i])) continue;
        json_object_object_add(grouped, names[i], json_object_new_boolean(true));
        if (matches(names[i], ui->filter)) child_match = true;
    }
    if (!group_match && !child_match) return;
    if (!append_row(ui, UI_GROUP, name, name) || ui_is_collapsed(ui, &ui->rows[ui->count - 1])) return;
    for (size_t i = 0; i < count; i++)
        if (contains(members, ui_kind(ui), names[i]) && (group_match || matches(names[i], ui->filter)))
            if (!append_row(ui, UI_RESOURCE, names[i], name)) return;
}

static void append_ungrouped(KmUi *ui, char **names, size_t count, json_object *grouped) {
    bool any = false;
    for (size_t i = 0; i < count; i++)
        if (!km_get(grouped, names[i]) && matches(names[i], ui->filter)) any = true;
    if (!any || !append_row(ui, UI_UNGROUPED, "Ungrouped", NULL)) return;
    if (ui_is_collapsed(ui, &ui->rows[ui->count - 1])) return;
    for (size_t i = 0; i < count; i++)
        if (!km_get(grouped, names[i]) && matches(names[i], ui->filter))
            if (!append_row(ui, UI_RESOURCE, names[i], NULL)) return;
}

void ui_rebuild(KmUi *ui) {
    const KmUiRow *selected = ui_current_row(ui);
    char id[65] = "", group[65] = ""; KmUiRowType type = UI_RESOURCE;
    if (selected) { snprintf(id, sizeof(id), "%s", selected->id); type = selected->type;
        if (selected->group) snprintf(group, sizeof(group), "%s", selected->group); }
    ui_free_rows(ui);
    if (ui->tab >= UI_CLIENTS) return;
    size_t count, groups_count;
    char **names = sorted_names(km_resources(ui->km, ui_kind(ui)), NULL, &count);
    char **groups = sorted_names(km_get(ui->km->catalog, "groups"), ui_kind(ui), &groups_count);
    json_object *grouped = json_object_new_object();
    if (names && groups) {
        for (size_t i = 0; i < groups_count; i++) append_group(ui, groups[i], names, count, grouped);
        append_ungrouped(ui, names, count, grouped);
    } else ui_notice(ui, "Cannot allocate resource list");
    free_names(names, count); free_names(groups, groups_count); json_object_put(grouped);
    for (int i = 0; i < ui->count; i++) {
        KmUiRow *row = &ui->rows[i];
        if ((*ui->focus && row->type == (ui->focus_group ? UI_GROUP : UI_RESOURCE) && !strcmp(row->id, ui->focus)) ||
            (!*ui->focus && row->type == type && !strcmp(row->id, id) &&
             !strcmp(row->group ? row->group : "", group))) { ui->row = i; break; }
    }
    ui->focus[0] = '\0';
    ui->focus_group = false;
    if (ui->row >= ui->count) ui->row = ui->count ? ui->count - 1 : 0;
    if (ui->top > ui->row) ui->top = ui->row;
}
