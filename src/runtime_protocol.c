#include "runtime.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int rt_changed(Rt *rt) {
    if (!rt->initialized) return 0;
    json_object *notice = json_object_new_object();
    json_object_object_add(notice, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(notice, "method", json_object_new_string("notifications/tools/list_changed"));
    int status = rt_send(rt->km, STDOUT_FILENO, notice);
    json_object_put(notice);
    return status;
}

int rt_refresh(Rt *rt, bool force) {
    char path[PATH_MAX];
    struct stat st;
    if (km_path(rt->km, path, sizeof(path), "%s/catalog.json", rt->km->config)) return -1;
    bool exists = stat(path, &st) == 0;
    if (!force && exists && st.st_dev == rt->catalog_dev && st.st_ino == rt->catalog_inode) return 0;
    bool valid = exists && km_load(rt->km) == 0;
    json_object *server = valid ? km_get(km_get(rt->km->catalog, "mcps"), rt->id) : NULL;
    json_object *definition = server ? km_mcp_execution(server, rt->agent) : json_object_new_object();
    if (!definition) return km_error(rt->km, "Cannot allocate MCP execution definition");
    const char *mode = km_string(definition, "mode"), *transport = km_string(definition, "transport");
    bool enabled = mode && !strcmp(mode, "managed") && transport && !strcmp(transport, "stdio") &&
                   km_effective(rt->km, "mcp", rt->id, rt->agent, NULL, 0);
    bool changed = rt->enabled != enabled || !rt->execution || !json_object_equal(definition, rt->execution);
    if (changed) {
        rt_stop(rt, "MCP availability or configuration changed; completion unknown, call not replayed");
        json_object_put(rt->execution);
        rt->execution = json_object_get(definition);
        rt->enabled = enabled;
        if (rt_changed(rt)) { json_object_put(definition); return -1; }
    }
    json_object_put(definition);
    rt->catalog_dev = exists ? st.st_dev : 0;
    rt->catalog_inode = exists ? st.st_ino : 0;
    return 0;
}

bool rt_legacy_version(const char *version) {
    const char *versions[] = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
        if (version && !strcmp(version, versions[i])) return true;
    return false;
}

static bool identifier(json_object *value) {
    return json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_string);
}

static bool text_field(json_object *object, const char *key, bool nonempty) {
    json_object *value = km_get(object, key);
    if (!json_object_is_type(value, json_type_string)) return false;
    const char *text = json_object_get_string(value);
    return (!nonempty || *text) && strlen(text) == (size_t)json_object_get_string_len(value);
}

static bool optional_object(json_object *object, const char *key) {
    json_object *value = NULL;
    return !json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_object);
}

static bool request_shape(json_object *packet) {
    json_object *reserved = NULL;
    return text_field(packet, "method", true) && optional_object(packet, "params") &&
           !json_object_object_get_ex(packet, "result", &reserved) &&
           !json_object_object_get_ex(packet, "error", &reserved);
}

static bool cancelled(json_object *record) {
    return json_object_get_boolean(km_get(record, "cancelled"));
}

static int cancel_request(Rt *rt, json_object *packet) {
    json_object *params = km_get(packet, "params"), *id = km_get(params, "requestId"), *reason = NULL;
    if (!rt->initialized || rt->child <= 0 || !identifier(id) ||
        (json_object_object_get_ex(params, "reason", &reason) && !text_field(params, "reason", false))) return 0;
    const char *key = json_object_to_json_string_ext(id, JSON_C_TO_STRING_PLAIN);
    json_object *record = km_get(rt->pending, key);
    if (!record || cancelled(record)) return 0;
    if (rt_send(rt->km, rt->upstream_in, packet)) {
        rt_stop(rt, "MCP disconnected during cancellation; completion unknown, call not replayed");
        return 0;
    }
    /* Keep the ID reserved until completion/reconnect: upstream may still run. */
    json_object_object_add(record, "cancelled", json_object_new_boolean(true));
    return 0;
}

static bool unique_progress_token(Rt *rt, json_object *token) {
    if (!token) return true;
    json_object_object_foreach(rt->pending, key, record) {
        (void)key;
        json_object *existing = km_get(record, "progressToken");
        if (existing && json_object_equal(existing, token)) return false;
    }
    return true;
}

static int initialize(Rt *rt, json_object *packet, json_object *id) {
    if (rt->initialized) return rt_error(rt, id, -32600, "This connection is already initialized");
    json_object *params = km_get(packet, "params");
    const char *version = km_string(params, "protocolVersion");
    if (!text_field(params, "protocolVersion", true) || !optional_object(params, "capabilities") ||
        !optional_object(params, "clientInfo"))
        return rt_error(rt, id, -32602, "initialize requires a protocolVersion string");
    /* An initialize request follows the legacy negotiation contract even if
     * its offered date is newer. Do not claim the modern discovery lifecycle. */
    if (!rt_legacy_version(version)) version = "2025-11-25";
    if (json_object_deep_copy(params, &rt->initialize, NULL)) return km_error(rt->km, "Cannot retain MCP initialization");
    json_object_object_add(rt->initialize, "protocolVersion", json_object_new_string(version));
    json_object *result = json_object_new_object(), *caps = json_object_new_object();
    json_object *tools = json_object_new_object(), *info = json_object_new_object();
    json_object_object_add(tools, "listChanged", json_object_new_boolean(true));
    json_object_object_add(caps, "tools", tools);
    json_object_object_add(result, "protocolVersion", json_object_new_string(version));
    json_object_object_add(result, "capabilities", caps);
    json_object_object_add(info, "name", json_object_new_string("kitmux-stdio-bridge"));
    json_object_object_add(info, "version", json_object_new_string(KM_VERSION));
    json_object_object_add(result, "serverInfo", info);
    int status = rt_result(rt, id, result);
    if (!status) rt->initialized = true;
    return status;
}

static int forward_request(Rt *rt, json_object *packet, json_object *id) {
    if (json_object_object_length(rt->pending) >= RT_MAX_PENDING)
        return rt_error(rt, id, -32000, "Managed MCP has 64 outstanding requests; wait or reconnect");
    const char *key = json_object_to_json_string_ext(id, JSON_C_TO_STRING_PLAIN);
    if (km_get(rt->pending, key)) return rt_error(rt, id, -32600, "Duplicate outstanding request ID");
    json_object *meta = km_get(km_get(packet, "params"), "_meta"), *token = NULL;
    bool has_token = meta && json_object_object_get_ex(meta, "progressToken", &token);
    if (has_token && (!identifier(token) || !unique_progress_token(rt, token)))
        return rt_error(rt, id, -32602, "progressToken must be a unique active string or integer");
    if (rt_start(rt)) return rt_error(rt, id, -32000, rt->km->error);
    json_object *record = json_object_new_object();
    if (!record) return rt_error(rt, id, -32000, "Cannot track the MCP request");
    json_object_object_add(record, "id", json_object_get(id));
    if (token) json_object_object_add(record, "progressToken", json_object_get(token));
    json_object_object_add(rt->pending, key, record);
    if (!rt_send(rt->km, rt->upstream_in, packet)) return 0;
    rt_stop(rt, "MCP disconnected; completion unknown, call not replayed");
    return 0;
}

int rt_client(Rt *rt, json_object *packet) {
    if (rt_refresh(rt, true)) return -1;
    const char *method = km_string(packet, "method");
    json_object *id = NULL;
    bool has_id = json_object_object_get_ex(packet, "id", &id);
    if (has_id && !identifier(id)) return rt_error(rt, NULL, -32600, "MCP request IDs must be strings or integers");
    if (!request_shape(packet)) return has_id ? rt_error(rt, id, -32600, "Expected an MCP request object") : 0;
    if (!has_id) {
        if (!strcmp(method, "notifications/cancelled")) return cancel_request(rt, packet);
        return 0;
    }
    if (!strcmp(method, "server/discover"))
        return rt_error(rt, id, -32601, "Managed bridge uses legacy initialize; modern discovery is unsupported");
    if (!strcmp(method, "initialize")) return initialize(rt, packet, id);
    if (!rt->initialized) return rt_error(rt, id, -32002, "Initialize the MCP connection first");
    if (!strcmp(method, "ping")) return rt_result(rt, id, json_object_new_object());
    bool list = !strcmp(method, "tools/list"), call = !strcmp(method, "tools/call");
    if (!list && !call) return rt_error(rt, id, -32601, "Managed bridge supports tools only; use direct mode for other capabilities");
    json_object *params = km_get(packet, "params"), *cursor = NULL, *task = NULL;
    if ((params && !optional_object(params, "_meta")) ||
        (list && json_object_object_get_ex(params, "cursor", &cursor) && !json_object_is_type(cursor, json_type_string)) ||
        (call && (!text_field(params, "name", true) || !optional_object(params, "arguments"))))
        return rt_error(rt, id, -32602, "Invalid tools request parameters");
    if (call && json_object_object_get_ex(params, "task", &task))
        return rt_error(rt, id, -32602, "Task-augmented calls require direct mode");
    if (!rt->enabled) {
        if (call) return rt_error(rt, id, -32000, "This MCP is disabled for this agent");
        json_object *empty = json_object_new_object();
        json_object_object_add(empty, "tools", json_object_new_array());
        return rt_result(rt, id, empty);
    }
    return forward_request(rt, packet, id);
}

static int reject_server_request(Rt *rt, json_object *packet, json_object *id) {
    json_object *error = json_object_new_object(), *reply = json_object_new_object();
    json_object_object_add(reply, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(reply, "id", json_object_get(id));
    if (!strcmp(km_string(packet, "method"), "ping")) {
        json_object_put(error);
        json_object_object_add(reply, "result", json_object_new_object());
    } else {
        json_object_object_add(error, "code", json_object_new_int(-32601));
        json_object_object_add(error, "message", json_object_new_string("Managed bridge does not advertise client callbacks; use direct mode"));
        json_object_object_add(reply, "error", error);
    }
    int status = rt_send(rt->km, rt->upstream_in, reply);
    json_object_put(reply);
    return status;
}

static bool finite_number(json_object *value) {
    return (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) &&
           isfinite(json_object_get_double(value));
}

static int forward_progress(Rt *rt, json_object *packet) {
    json_object *params = km_get(packet, "params"), *token = km_get(params, "progressToken");
    json_object *progress = km_get(params, "progress"), *total = NULL, *message = NULL;
    if (!identifier(token) || !finite_number(progress) ||
        (json_object_object_get_ex(params, "total", &total) && !finite_number(total)) ||
        (json_object_object_get_ex(params, "message", &message) && !text_field(params, "message", false))) return 0;
    json_object_object_foreach(rt->pending, key, record) {
        (void)key;
        json_object *expected = km_get(record, "progressToken"), *previous = km_get(record, "progress");
        if (!expected || !json_object_equal(expected, token) || cancelled(record)) continue;
        if (previous && json_object_get_double(progress) <= json_object_get_double(previous)) return 0;
        json_object_object_add(record, "progress", json_object_get(progress));
        return rt_send(rt->km, STDOUT_FILENO, packet);
    }
    return 0;
}

static bool response_shape(json_object *packet) {
    json_object *result = NULL, *error = NULL, *method = NULL;
    bool good_result = json_object_object_get_ex(packet, "result", &result);
    bool good_error = json_object_object_get_ex(packet, "error", &error);
    if (json_object_object_get_ex(packet, "method", &method) || good_result == good_error) return false;
    if (good_result) return json_object_is_type(result, json_type_object);
    return json_object_is_type(error, json_type_object) &&
           json_object_is_type(km_get(error, "code"), json_type_int) && text_field(error, "message", false);
}

int rt_server(Rt *rt, json_object *packet) {
    const char *method = km_string(packet, "method");
    json_object *id = NULL;
    bool has_id = json_object_object_get_ex(packet, "id", &id);
    if (method) {
        if (!request_shape(packet) || (has_id && !identifier(id))) {
            rt_stop(rt, "Invalid MCP request; completion unknown, call not replayed");
            return 0;
        }
        if (has_id) return reject_server_request(rt, packet, id);
        if (!strcmp(method, "notifications/progress")) return forward_progress(rt, packet);
        return !strcmp(method, "notifications/tools/list_changed") ? rt_send(rt->km, STDOUT_FILENO, packet) : 0;
    }
    const char *key = has_id ? json_object_to_json_string_ext(id, JSON_C_TO_STRING_PLAIN) : NULL;
    json_object *record = key ? km_get(rt->pending, key) : NULL;
    if (!identifier(id) || !record || !response_shape(packet)) {
        rt_stop(rt, "Invalid MCP response; completion unknown, call not replayed");
        return 0;
    }
    int status = cancelled(record) ? 0 : rt_send(rt->km, STDOUT_FILENO, packet);
    json_object_object_del(rt->pending, key);
    return status;
}
