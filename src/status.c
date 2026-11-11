#include "runtime.h"
#include "subagents.h"
#include <string.h>

static json_object *subagent_dependencies(Km *km, json_object *resource, const char *client) {
    json_object *requires = km_get(km_get(km_get(resource, "native"), client), "requires");
    json_object *result = json_object_new_array();
    if (!requires) return result;
    for (size_t i = 0; i < json_object_array_length(requires); i++) {
        const char *reference = json_object_get_string(json_object_array_get_idx(requires, i));
        const char *colon = strchr(reference, ':');
        char kind[10]; size_t length = (size_t)(colon - reference);
        memcpy(kind, reference, length); kind[length] = '\0';
        json_object *managed = km_get(km_resources(km, kind), colon + 1);
        const char *state = !managed ? "unresolved_native" : km_effective(km, kind, colon + 1, client, NULL, 0) ? "available" : "unavailable";
        json_object *row = json_object_new_object();
        json_object_object_add(row, "resource", json_object_new_string(reference));
        json_object_object_add(row, "state", json_object_new_string(state));
        json_object_array_add(result, row);
    }
    return result;
}

static void subagent_status(Km *km, const char *id, const char *client, json_object *row, bool enabled) {
    json_object *resource = km_get(km_resources(km, "subagent"), id);
    char reason[128];
    bool compatible = km_subagent_compatible(resource, client, reason, sizeof(reason));
    json_object_object_add(row, "compatible", json_object_new_boolean(compatible));
    json_object_object_add(row, "compatibility_reason", json_object_new_string(reason));
    json_object_object_add(row, "availability", json_object_new_string(!compatible ? "incompatible" : enabled ? "available" : "disabled"));
    json_object_object_add(row, "dependencies", subagent_dependencies(km, resource, client));
    json_object *review = km_get(km_get(km_get(resource, "native"), client), "requires_review");
    json_object_object_add(row, "dependency_review", json_object_new_boolean(json_object_get_boolean(review)));
}

static json_object *mcp_observation(Km *km, const char *id, const char *agent) {
    json_object *observation = rt_status(km, id, agent);
    if (!observation) return NULL;
    char reason[128];
    bool enabled = km_effective(km, "mcp", id, agent, reason, sizeof(reason));
    json_object_object_add(observation, "enabled", json_object_new_boolean(enabled));
    json_object_object_add(observation, "reason", json_object_new_string(reason));
    return observation;
}

static json_object *agent_status(Km *km, const char *kind, const char *id) {
    json_object *agents = json_object_new_object();
    for (size_t i = 0; i < KM_AGENT_COUNT; ++i) {
        char reason[128];
        bool enabled = km_effective(km, kind, id, km_agents[i], reason, sizeof(reason));
        json_object *row = json_object_new_object();
        json_object_object_add(row, "enabled", json_object_new_boolean(enabled));
        json_object_object_add(row, "reason", json_object_new_string(reason));
        json_object *observation = !strcmp(kind, "mcp") ? mcp_observation(km, id, km_agents[i]) : NULL;
        if (!strcmp(kind, "mcp") && !observation) {
            json_object_put(row); json_object_put(agents); return NULL;
        }
        const char *state = observation ? km_string(observation, "state") : !strcmp(kind, "subagent") ? "native" : "unknown";
        json_object_object_add(row, "runtime", json_object_new_string(state ? state : "unknown"));
        if (observation) json_object_object_add(row, "runtime_status", observation);
        json_object_object_add(row, "refresh", json_object_new_string("unverified"));
        if (!strcmp(kind, "subagent")) subagent_status(km, id, km_agents[i], row, enabled);
        json_object_object_add(agents, km_agents[i], row);
    }
    return agents;
}

static json_object *resource_status(Km *km, const char *kind) {
    json_object *result = json_object_new_object(), *resources = km_resources(km, kind);
    json_object_object_foreach(resources, id, resource) {
        json_object *row = json_object_new_object();
        json_object *agents = agent_status(km, kind, id);
        if (!agents) { json_object_put(row); json_object_put(result); return NULL; }
        json_object *enabled = km_get(resource, "enabled"), *assignments = km_get(resource, "agents");
        json_object_object_add(row, "enabled", json_object_new_boolean(!enabled || json_object_get_boolean(enabled)));
        json_object_object_add(row, "assignments", assignments ? json_object_get(assignments) : json_object_new_object());
        json_object_object_add(row, "agents", agents);
        if (!strcmp(kind, "mcp")) {
            const char *transport = km_string(resource, "transport"), *mode = km_string(resource, "mode");
            json_object_object_add(row, "transport", json_object_new_string(transport && !strcmp(transport, "http") ? "http" : "stdio"));
            json_object_object_add(row, "mode", json_object_new_string(mode && !strcmp(mode, "managed") ? "managed" : "direct"));
        }
        json_object_object_add(result, id, row);
    }
    return result;
}

static json_object *group_status(Km *km) {
    json_object *result = json_object_new_object(), *groups = km_get(km->catalog, "groups");
    json_object_object_foreach(groups, id, group) {
        json_object *copy = json_object_new_object();
        json_object_object_add(copy, "enabled", json_object_get(km_get(group, "enabled")));
        json_object_object_add(copy, "agents", json_object_get(km_get(group, "agents")));
        json_object_object_add(copy, "members", json_object_get(km_get(group, "members")));
        json_object_object_add(result, id, copy);
    }
    return result;
}

static void print_observation(json_object *observation) {
    const char *mode = km_string(observation, "mode"), *state = km_string(observation, "state");
    if (mode && !strcmp(mode, "direct")) { fputs("native connection unverified", stdout); return; }
    printf("upstream %s; bridges %d reachable, %d unreachable", state ? state : "unknown",
           json_object_get_int(km_get(observation, "reachable")),
           json_object_get_int(km_get(observation, "unreachable")));
}

static void print_connections(json_object *observation, const char *indent) {
    json_object *connections = km_get(observation, "connections");
    for (size_t i = 0; i < json_object_array_length(connections); i++) {
        json_object *connection = json_object_array_get_idx(connections, i);
        if (!json_object_get_boolean(km_get(connection, "reachable"))) {
            printf("%sbridge unreachable; process state unavailable\n", indent); continue;
        }
        printf("%sbridge %lld / upstream %lld: %s, %d pending\n", indent,
               (long long)json_object_get_int64(km_get(connection, "bridge_pid")),
               (long long)json_object_get_int64(km_get(connection, "upstream_pid")),
               km_string(connection, "state"), json_object_get_int(km_get(connection, "pending")));
    }
}

int km_mcp_status(Km *km, const char *id, const char *agent, bool as_json) {
    if (!km_get(km_resources(km, "mcp"), id)) return km_error(km, "Unknown MCP: %s", id);
    json_object *observation = mcp_observation(km, id, agent);
    if (!observation) return -1;
    if (as_json) puts(json_object_to_json_string_ext(observation, JSON_C_TO_STRING_PRETTY));
    else {
        printf("%s / %s: desired %s (%s)\n", id, agent,
               json_object_get_boolean(km_get(observation, "enabled")) ? "on" : "off", km_string(observation, "reason"));
        print_observation(observation); putchar('\n');
        print_connections(observation, "  ");
        puts("Client tool refresh remains unverified.");
    }
    json_object_put(observation);
    return 0;
}

static void print_resources(json_object *resources, const char *kind) {
    json_object_object_foreach(resources, id, resource) {
        printf("%s:%s\n", kind, id);
        json_object *agents = km_get(resource, "agents");
        for (size_t i = 0; i < KM_AGENT_COUNT; i++) {
            json_object *row = km_get(agents, km_agents[i]);
            printf("  %-8s %-3s  %s", km_agents[i],
                   json_object_get_boolean(km_get(row, "enabled")) ? "on" : "off", km_string(row, "reason"));
            json_object *observation = km_get(row, "runtime_status");
            if (observation) { fputs(" | ", stdout); print_observation(observation); }
            putchar('\n');
            if (observation) print_connections(observation, "    ");
        }
    }
}

int km_status(Km *km, bool as_json) {
    json_object *result = json_object_new_object();
    json_object *mcps = resource_status(km, "mcp");
    if (!mcps) { json_object_put(result); return -1; }
    json_object_object_add(result, "version", json_object_new_string(KM_VERSION));
    json_object_object_add(result, "revision", json_object_get(km_get(km->catalog, "revision")));
    json_object_object_add(result, "groups", group_status(km));
    json_object_object_add(result, "mcps", mcps);
    json_object_object_add(result, "skills", resource_status(km, "skill"));
    json_object_object_add(result, "subagents", resource_status(km, "subagent"));
    if (as_json) puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY));
    else {
        printf("kitmux %s  ·  catalog revision %lld\n\n", KM_VERSION, (long long)json_object_get_int64(km_get(km->catalog, "revision")));
        print_resources(mcps, "mcp");
        print_resources(km_get(result, "skills"), "skill");
        print_resources(km_get(result, "subagents"), "subagent");
        puts("\nProcess observations are a snapshot. Client tool refresh remains unverified.\nUse sync --preview to inspect pending native changes.");
    }
    json_object_put(result);
    return 0;
}
