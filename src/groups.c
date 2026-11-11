#include "kitmux.h"
#include <stdlib.h>
#include <string.h>

static int group_add(Km *km, const char *id) {
    json_object *groups = km_get(km->catalog, "groups");
    if (!km_valid_id(id)) return km_error(km, "Use a group ID with 1-64 letters, digits, hyphens or underscores");
    if (km_get(groups, id)) return km_error(km, "Group already exists: %s", id);
    json_object *group = json_tokener_parse("{\"enabled\":true,\"agents\":[],\"members\":[]}");
    json_object_object_add(groups, id, group);
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
    json_object_object_add(group, "agents", agents);
    return km_save(km);
}

static int member_valid(Km *km, const char *member) {
    const char *colon = strchr(member, ':');
    if (!colon) return km_error(km, "Membership must be mcp:ID, skill:ID, or subagent:ID");
    char kind[10];
    size_t length = (size_t)(colon - member);
    if (length >= sizeof(kind)) return km_error(km, "Unknown resource kind");
    memcpy(kind, member, length); kind[length] = '\0';
    if (!km_valid_id(colon + 1) || !km_get(km_resources(km, kind), colon + 1)) return km_error(km, "Unknown resource: %s", member);
    return 0;
}

static int group_members(Km *km, json_object *group, int count, char **values, bool add) {
    for (int i = 0; i < count; ++i) if (member_valid(km, values[i]) < 0) return -1;
    json_object *members = km_get(group, "members");
    for (int i = 0; i < count; ++i) {
        size_t index = 0, length = json_object_array_length(members);
        while (index < length && strcmp(json_object_get_string(json_object_array_get_idx(members, index)), values[i])) ++index;
        if (add && index == length) json_object_array_add(members, json_object_new_string(values[i]));
        if (!add && index < length) json_object_array_del_idx(members, index, 1);
    }
    return km_save(km);
}

static int group_list(Km *km, bool as_json) {
    json_object *groups = km_get(km->catalog, "groups");
    if (as_json) { puts(json_object_to_json_string_ext(groups, JSON_C_TO_STRING_PRETTY)); return 0; }
    printf("%-22s %-6s %-28s %s\n", "GROUP", "STATE", "AGENTS", "ITEMS");
    json_object_object_foreach(groups, id, group) {
        char names[64] = "";
        json_object *agents = km_get(group, "agents");
        for (size_t i = 0; i < json_object_array_length(agents); ++i) {
            size_t used = strlen(names);
            snprintf(names + used, sizeof(names) - used, "%s%s", i ? "," : "", json_object_get_string(json_object_array_get_idx(agents, i)));
        }
        printf("%-22s %-6s %-28s %zu\n", id, json_object_get_boolean(km_get(group, "enabled")) ? "ON" : "OFF", *names ? names : "none", json_object_array_length(km_get(group, "members")));
    }
    return 0;
}

int km_group_command(Km *km, int argc, char **argv) {
    if (!argc || !strcmp(argv[0], "list")) {
        if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json"))) return km_error(km, "Usage: group list [--json]");
        return group_list(km, argc == 2);
    }
    if (argc < 2) return km_error(km, "Usage: group add|show|assign|include|exclude|enable|disable|remove ID");
    const char *verb = argv[0], *id = argv[1];
    if (!strcmp(verb, "add") && argc == 2) return group_add(km, id);
    json_object *groups = km_get(km->catalog, "groups"), *group = km_get(groups, id);
    if (!group) return km_error(km, "Unknown group: %s", id);
    if (!strcmp(verb, "show") && argc == 2) { puts(json_object_to_json_string_ext(group, JSON_C_TO_STRING_PRETTY)); return 0; }
    if (!strcmp(verb, "assign") && argc == 4 && !strcmp(argv[2], "--agents")) return group_assign(km, group, argv[3]);
    if ((!strcmp(verb, "include") || !strcmp(verb, "exclude")) && argc >= 3) return group_members(km, group, argc - 2, argv + 2, !strcmp(verb, "include"));
    if ((!strcmp(verb, "enable") || !strcmp(verb, "disable")) && argc == 2)
        json_object_object_add(group, "enabled", json_object_new_boolean(!strcmp(verb, "enable")));
    else if (!strcmp(verb, "remove") && argc == 2) json_object_object_del(groups, id);
    else if (!strcmp(verb, "rename") && argc == 3 && km_valid_id(argv[2]) && !km_get(groups, argv[2])) {
        json_object_object_add(groups, argv[2], json_object_get(group)); json_object_object_del(groups, id);
    } else return km_error(km, "Invalid group command or arguments");
    return km_save(km);
}
