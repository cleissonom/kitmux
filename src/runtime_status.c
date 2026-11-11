#include "runtime.h"
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool upstream_running(Rt *rt) {
    if (rt->child <= 0) return false;
    siginfo_t observed = {0};
    int status;
    do { status = waitid(P_PID, (id_t)rt->child, &observed, WEXITED | WNOHANG | WNOWAIT); }
    while (status < 0 && errno == EINTR);
    return status == 0 && observed.si_pid == 0;
}

json_object *rt_connection_status(Rt *rt) {
    bool running = upstream_running(rt);
    const char *state = !rt->enabled ? "disabled" : !rt->initialized ? "waiting" : running ? "running" : "idle";
    json_object *result = json_object_new_object();
    json_object_object_add(result, "state", json_object_new_string(state));
    json_object_object_add(result, "upstream_pid", json_object_new_int64(running ? rt->child : 0));
    json_object_object_add(result, "pending", json_object_new_int(json_object_object_length(rt->pending)));
    const char *version = km_string(rt->initialize, "protocolVersion");
    json_object_object_add(result, "protocol", version ? json_object_new_string(version) : NULL);
    json_object_object_add(result, "upstream_protocol", running && *rt->upstream_protocol ? json_object_new_string(rt->upstream_protocol) : NULL);
    json_object_object_add(result, "protocol_lifecycle", json_object_new_string("legacy"));
    json_object_object_add(result, "tool_refresh", json_object_new_string("unverified"));
    return result;
}

static bool valid_state(const char *state) {
    return state && (!strcmp(state, "waiting") || !strcmp(state, "idle") ||
                     !strcmp(state, "running") || !strcmp(state, "disabled"));
}

static json_object *sanitized_connection(json_object *reply) {
    const char *state = km_string(reply, "state");
    bool reachable = json_object_get_boolean(km_get(reply, "reachable")) && valid_state(state);
    json_object *clean = json_object_new_object();
    json_object_object_add(clean, "reachable", json_object_new_boolean(reachable));
    json_object_object_add(clean, "state", json_object_new_string(reachable ? state : "unreachable"));
    if (!reachable) return clean;
    const char *numbers[] = {"bridge_pid", "upstream_pid", "pending"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
        json_object_object_add(clean, numbers[i], json_object_new_int64(json_object_get_int64(km_get(reply, numbers[i]))));
    const char *protocols[] = {"protocol", "upstream_protocol"};
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
        const char *version = km_string(reply, protocols[i]);
        json_object_object_add(clean, protocols[i], version && rt_legacy_version(version) ? json_object_new_string(version) : NULL);
    }
    json_object_object_add(clean, "protocol_lifecycle", json_object_new_string("legacy"));
    json_object_object_add(clean, "tool_refresh", json_object_new_string("unverified"));
    return clean;
}

static const char *aggregate_state(json_object *connections, int reachable, int unreachable, bool complete) {
    if (unreachable || !complete) return reachable ? "partial" : "unreachable";
    if (!reachable) return "inactive";
    bool waiting = false, idle = false;
    for (size_t i = 0; i < json_object_array_length(connections); i++) {
        const char *state = km_string(json_object_array_get_idx(connections, i), "state");
        if (!strcmp(state, "running")) return "running";
        waiting |= !strcmp(state, "waiting");
        idle |= !strcmp(state, "idle");
    }
    return waiting ? "waiting" : idle ? "idle" : "disabled";
}

json_object *rt_status(Km *km, const char *id, const char *agent) {
    json_object *server = km_get(km_get(km->catalog, "mcps"), id);
    if (!server || !km_valid_agent(agent)) { km_error(km, "Unknown MCP or agent for runtime status"); return NULL; }
    const char *mode = km_string(server, "mode");
    bool managed = mode && !strcmp(mode, "managed"), complete = true;
    json_object *observations = managed ? rt_control_connections(km, id, agent, "status", 500, &complete) : json_object_new_array();
    if (!observations) return NULL;
    json_object *connections = json_object_new_array();
    int reachable = 0, unreachable = 0;
    for (size_t i = 0; i < json_object_array_length(observations); i++) {
        json_object *clean = sanitized_connection(json_object_array_get_idx(observations, i));
        if (json_object_get_boolean(km_get(clean, "reachable"))) reachable++; else unreachable++;
        json_object_array_add(connections, clean);
    }
    json_object_put(observations);
    json_object *result = json_object_new_object();
    json_object_object_add(result, "mode", json_object_new_string(managed ? "managed" : "direct"));
    json_object_object_add(result, "state", json_object_new_string(managed ? aggregate_state(connections, reachable, unreachable, complete) : "unverified"));
    json_object_object_add(result, "connections", connections);
    json_object_object_add(result, "reachable", json_object_new_int(reachable));
    json_object_object_add(result, "unreachable", json_object_new_int(unreachable));
    json_object_object_add(result, "scan_complete", json_object_new_boolean(complete));
    json_object_object_add(result, "tool_refresh", json_object_new_string("unverified"));
    return result;
}
