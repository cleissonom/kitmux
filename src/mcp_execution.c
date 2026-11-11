#include "kitmux.h"

json_object *km_mcp_execution(json_object *server, const char *agent) {
    if (!json_object_is_type(server, json_type_object) || !km_valid_agent(agent)) return NULL;
    json_object *variant = km_get(km_get(server, "variants"), agent);
    json_object *source = variant ? variant : server;
    const char *keys[] = {"transport", "command", "url", "env", "cwd"};
    json_object *execution = json_object_new_object();
    if (!execution) return NULL;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        json_object *value = km_get(source, keys[i]), *copy = NULL;
        if (!value) continue;
        if (json_object_deep_copy(value, &copy, NULL)) { json_object_put(execution); return NULL; }
        json_object_object_add(execution, keys[i], copy);
    }
    const char *mode = km_string(server, "mode");
    json_object_object_add(execution, "mode", json_object_new_string(mode ? mode : "direct"));
    return execution;
}
