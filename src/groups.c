#include "kitmux.h"
#include <stdlib.h>
#include <string.h>

static bool group_kind(const char *kind) {
    return kind && (!strcmp(kind, "mcp") || !strcmp(kind, "skill") || !strcmp(kind, "subagent"));
}

static int group_key(Km *km, char *key, const char *kind, const char *id) {
    if (!group_kind(kind) || !km_valid_id(id)) return km_error(km, "Use a valid resource kind and group ID");
    return km_path(km, key, 80, "%s:%s", kind, id);
}

json_object *km_group_get(Km *km, const char *kind, const char *id) {
    char key[80];
    if (group_key(km, key, kind, id) < 0) return NULL;
    return km_get(km_get(km->catalog, "groups"), key);
}

bool km_groups_need_migration(Km *km) {
    json_object_object_foreach(km_get(km->catalog, "groups"), id, group) {
        (void)id;
        if (!km_string(group, "kind")) return true;
    }
    return false;
}

static int group_add(Km *km, const char *kind, const char *id) {
    char key[80];
    if (group_key(km, key, kind, id) < 0) return -1;
    json_object *groups = km_get(km->catalog, "groups");
    if (km_get(groups, key)) return km_error(km, "Group already exists: %s", id);
    json_object *group = json_tokener_parse("{\"enabled\":true,\"agents\":[],\"members\":[]}");
    json_object_object_add(group, "kind", json_object_new_string(kind));
    json_object_object_add(group, "name", json_object_new_string(id));
    json_object_object_add(groups, key, group);
    json_object_object_add(km->catalog, "version", json_object_new_int(3));
    return km_save(km);
}

static int group_assign(Km *km, json_object *group, const char *csv) {
    json_object *agents = json_object_new_array();
    char *copy = strdup(csv), *save = NULL;
    if (!copy) { json_object_put(agents); return km_error(km, "Out of memory"); }
    bool selected[KM_AGENT_COUNT] = {false};
    for (char *s = strtok_r(copy, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        if (!km_valid_agent(s)) { free(copy); json_object_put(agents); return km_error(km, "Unknown agent; use codex,claude,opencode"); }
        for (size_t i = 0; i < KM_AGENT_COUNT; ++i) if (!strcmp(s, km_agents[i])) selected[i] = true;
    }
    free(copy);
    for (size_t i = 0; i < KM_AGENT_COUNT; ++i) if (selected[i]) json_object_array_add(agents, json_object_new_string(km_agents[i]));
    if (json_object_equal(agents, km_get(group, "agents"))) { json_object_put(agents); return 0; }
    json_object_object_add(group, "agents", agents);
    return km_save(km);
}

static int member_name(Km *km, const char *kind, const char *value, char *member) {
    const char *colon = strchr(value, ':');
    const char *id = colon ? colon + 1 : value;
    if (colon && ((size_t)(colon - value) != strlen(kind) || strncmp(value, kind, strlen(kind))))
        return km_error(km, "A %s group accepts only %s resources", kind, kind);
    if (!km_valid_id(id) || !km_get(km_resources(km, kind), id)) return km_error(km, "Unknown %s resource: %s", kind, id);
    return km_path(km, member, 80, "%s:%s", kind, id);
}

static int group_members(Km *km, const char *kind, json_object *group, int count, char **values, bool add) {
    char member[80];
    for (int i = 0; i < count; ++i) if (member_name(km, kind, values[i], member) < 0) return -1;
    json_object *members = km_get(group, "members");
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        if (member_name(km, kind, values[i], member) < 0) return -1;
        size_t index = 0, length = json_object_array_length(members);
        while (index < length && strcmp(json_object_get_string(json_object_array_get_idx(members, index)), member)) ++index;
        if (add && index == length) { json_object_array_add(members, json_object_new_string(member)); changed = true; }
        if (!add && index < length) { json_object_array_del_idx(members, index, 1); changed = true; }
    }
    return changed ? km_save(km) : 0;
}

static int group_list(Km *km, const char *kind, bool as_json) {
    json_object *groups = km_get(km->catalog, "groups"), *selected = json_object_new_object();
    json_object_object_foreach(groups, key, group) {
        const char *scope = km_string(group, "kind"), *name = km_string(group, "name");
        if (kind && (!scope || strcmp(scope, kind))) continue;
        json_object_object_add(selected, kind ? name : key, json_object_get(group));
    }
    if (as_json) puts(json_object_to_json_string_ext(selected, JSON_C_TO_STRING_PRETTY));
    else {
        printf("%-22s %-6s %-28s %s\n", "GROUP", "STATE", "AGENTS", "ITEMS");
        json_object_object_foreach(selected, id, item) {
            char names[64] = "";
            json_object *agents = km_get(item, "agents");
            for (size_t i = 0; i < json_object_array_length(agents); ++i) {
                size_t used = strlen(names);
                snprintf(names + used, sizeof(names) - used, "%s%s", i ? "," : "", json_object_get_string(json_object_array_get_idx(agents, i)));
            }
            printf("%-22s %-6s %-28s %zu\n", id, json_object_get_boolean(km_get(item, "enabled")) ? "ON" : "OFF", *names ? names : "none", json_object_array_length(km_get(item, "members")));
        }
    }
    json_object_put(selected);
    return 0;
}

int km_scoped_group_command(Km *km, const char *kind, int argc, char **argv) {
    if (!group_kind(kind)) return km_error(km, "Unknown group resource kind");
    if (!argc || !strcmp(argv[0], "list")) {
        if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json"))) return km_error(km, "Usage: %s group list [--json]", kind);
        return group_list(km, kind, argc == 2);
    }
    if (km_groups_need_migration(km)) return km_error(km, "Legacy groups need migration; run group migrate --preview then group migrate --apply");
    if (argc < 2) return km_error(km, "Use %s group add|show|assign|include|exclude|enable|disable|rename|remove ID", kind);
    const char *verb = argv[0], *id = argv[1]; char key[80];
    if (group_key(km, key, kind, id) < 0) return -1;
    if (!strcmp(verb, "add") && argc == 2) return group_add(km, kind, id);
    json_object *groups = km_get(km->catalog, "groups"), *group = km_get(groups, key);
    if (!group) return km_error(km, "Unknown %s group: %s", kind, id);
    if (!strcmp(verb, "show") && argc == 2) { puts(json_object_to_json_string_ext(group, JSON_C_TO_STRING_PRETTY)); return 0; }
    if (!strcmp(verb, "assign") && argc == 4 && !strcmp(argv[2], "--agents")) return group_assign(km, group, argv[3]);
    if ((!strcmp(verb, "include") || !strcmp(verb, "exclude")) && argc >= 3)
        return group_members(km, kind, group, argc - 2, argv + 2, !strcmp(verb, "include"));
    if ((!strcmp(verb, "enable") || !strcmp(verb, "disable")) && argc == 2) {
        bool enabled = !strcmp(verb, "enable");
        if (json_object_get_boolean(km_get(group, "enabled")) == enabled) return 0;
        json_object_object_add(group, "enabled", json_object_new_boolean(enabled));
    } else if (!strcmp(verb, "remove") && argc == 2) json_object_object_del(groups, key);
    else if (!strcmp(verb, "rename") && argc == 3) {
        char next[80];
        if (group_key(km, next, kind, argv[2]) < 0) return -1;
        if (!strcmp(key, next)) return 0;
        if (km_get(groups, next)) return km_error(km, "Group already exists: %s", argv[2]);
        json_object_object_add(group, "name", json_object_new_string(argv[2]));
        json_object_object_add(groups, next, json_object_get(group)); json_object_object_del(groups, key);
    } else return km_error(km, "Invalid group command or arguments");
    return km_save(km);
}

int km_group_command(Km *km, int argc, char **argv) {
    if (argc && !strcmp(argv[0], "migrate")) return km_groups_migrate(km, argc - 1, argv + 1);
    if (!argc || !strcmp(argv[0], "list")) {
        if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json"))) return km_error(km, "Usage: group list [--json]");
        return group_list(km, NULL, argc == 2);
    }
    if (argc == 2 && !strcmp(argv[0], "show")) {
        json_object *group = km_get(km_get(km->catalog, "groups"), argv[1]);
        if (!group) return km_error(km, "Unknown group; use mcp, skill, or subagent group show ID");
        puts(json_object_to_json_string_ext(group, JSON_C_TO_STRING_PRETTY)); return 0;
    }
    return km_error(km, "Groups belong to resources; use mcp group, skill group, or subagent group. Migrate old groups with group migrate --preview");
}
