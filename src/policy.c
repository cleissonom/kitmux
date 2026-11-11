#include "kitmux.h"
#include "subagents.h"
#include <stdlib.h>
#include <string.h>

static bool contains(json_object *array, const char *value) {
    if (!json_object_is_type(array, json_type_array)) return false;
    for (size_t i = 0; i < json_object_array_length(array); ++i)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(array, i)), value)) return true;
    return false;
}

static bool answer(char *reason, size_t size, const char *text, bool result) {
    if (reason && size) snprintf(reason, size, "%s", text);
    return result;
}

bool km_effective(Km *km, const char *kind, const char *id, const char *agent, char *reason, size_t size) {
    json_object *resource = km_get(km_resources(km, kind), id);
    if (!resource || !km_valid_agent(agent)) return answer(reason, size, "Unavailable", false);
    json_object *enabled = km_get(resource, "enabled");
    if (enabled && !json_object_get_boolean(enabled)) return answer(reason, size, "Globally disabled", false);
    if (!strcmp(kind, "subagent") && !km_subagent_compatible(resource, agent, reason, size)) return false;
    const char *override = km_string(km_get(resource, "agents"), agent);
    if (override && !strcmp(override, "off")) return answer(reason, size, "Disabled for this agent", false);
    if (override && !strcmp(override, "on")) return answer(reason, size, "Direct assignment", true);
    char member[80];
    snprintf(member, sizeof(member), "%s:%s", kind, id);
    json_object *groups = km_get(km->catalog, "groups");
    json_object_object_foreach(groups, name, group) {
        if (json_object_get_boolean(km_get(group, "enabled")) && contains(km_get(group, "agents"), agent) &&
            contains(km_get(group, "members"), member)) {
            if (reason && size) snprintf(reason, size, "Through %s", name);
            return true;
        }
    }
    return answer(reason, size, "No active assignment", false);
}

int km_resource_toggle(Km *km, const char *kind, int argc, char **argv) {
    if (argc != 2 && argc != 4) return km_error(km, "Usage: %s enable|disable|inherit ID [--agent AGENT]", kind);
    json_object *resource = km_get(km_resources(km, kind), argv[1]);
    if (!resource) return km_error(km, "Unknown %s: %s", kind, argv[1]);
    const char *verb = argv[0];
    if (strcmp(verb, "enable") && strcmp(verb, "disable") && strcmp(verb, "inherit")) return km_error(km, "Invalid toggle");
    if (argc == 4) {
        if (strcmp(argv[2], "--agent") || !km_valid_agent(argv[3])) return km_error(km, "Expected --agent codex|claude|opencode");
        json_object *agents = km_get(resource, "agents");
        if (!agents) { agents = json_object_new_object(); json_object_object_add(resource, "agents", agents); }
        json_object_object_add(agents, argv[3], json_object_new_string(!strcmp(verb, "enable") ? "on" : !strcmp(verb, "disable") ? "off" : "inherit"));
    } else {
        if (!strcmp(verb, "inherit")) return km_error(km, "inherit requires --agent");
        json_object_object_add(resource, "enabled", json_object_new_boolean(!strcmp(verb, "enable")));
    }
    return km_save(km);
}

int km_remove_resource(Km *km, const char *kind, const char *id) {
    json_object *resources = km_resources(km, kind);
    if (!km_get(resources, id)) return km_error(km, "Unknown %s: %s", kind, id);
    char member[80];
    snprintf(member, sizeof(member), "%s:%s", kind, id);
    json_object *groups = km_get(km->catalog, "groups");
    json_object_object_foreach(groups, name, group) {
        (void)name;
        json_object *members = km_get(group, "members");
        for (size_t i = json_object_array_length(members); i > 0; --i)
            if (!strcmp(json_object_get_string(json_object_array_get_idx(members, i - 1)), member)) json_object_array_del_idx(members, i - 1, 1);
    }
    json_object_object_del(resources, id);
    return km_save(km);
}
