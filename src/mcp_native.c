#include "adapters.h"
#include "mcp_metadata.h"
#include <stdlib.h>
#include <string.h>

static json_object *copy(Km *km, json_object *value) {
    json_object *result = NULL;
    if (json_object_deep_copy(value, &result, NULL)) km_error(km, "Cannot copy native MCP definition");
    return result;
}

static json_object *object_at(json_object *parent, const char *key) {
    json_object *object = km_get(parent, key);
    if (!object) { object = json_object_new_object(); json_object_object_add(parent, key, object); }
    return object;
}

static int options(Km *km, int argc, char **argv, const char **path, const char **client,
                   KmMcpDescription *description) {
    for (int i = 2; i < argc; i++) {
        if (i + 1 >= argc) return km_error(km, "MCP option needs a value");
        int metadata = km_mcp_description_option(km, description, argv[i], argv[i + 1]);
        if (metadata < 0) return -1;
        if (metadata) { i++; continue; }
        const char **field = !strcmp(argv[i], "--from") ? path : !strcmp(argv[i], "--format") ? client : NULL;
        if (!field || *field || i + 1 >= argc) return km_error(km, "Use --from FILE --format codex|claude|opencode");
        *field = argv[++i];
    }
    return *path && *client && km_valid_agent(*client) ? 0 :
        km_error(km, "Native MCP source requires --from FILE --format codex|claude|opencode");
}

static json_object *source(Km *km, const char *path, const char *client) {
    size_t length = 0;
    char *text = km_read_file(km, path, &length);
    if (!text) return NULL;
    if (strlen(text) != length) { free(text); km_error(km, "Native MCP source contains NUL bytes"); return NULL; }
    json_object *native = ad_parse(km, text); free(text);
    if (!native) return NULL;
    const char *url = km_string(native, "url");
    json_object *command = km_get(native, "command");
    bool valid = ad_server_shape(km, client, native) == 0 && (!!url != !!command);
    if (valid && url) valid = !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
    if (valid && command) {
        const char *program = !strcmp(client, "opencode") ?
            json_object_get_string(json_object_array_get_idx(command, 0)) : json_object_get_string(command);
        valid = program && *program;
    }
    if (!valid) { json_object_put(native); km_error(km, "Native MCP source needs one valid command or HTTP URL"); return NULL; }
    return native;
}

static bool same_endpoint(json_object *left, json_object *right) {
    const char *keys[] = {"transport", "command", "url"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!json_object_equal(km_get(left, keys[i]), km_get(right, keys[i]))) return false;
    const char *transport = km_string(left, "transport");
    if (transport && !strcmp(transport, "stdio") &&
        !json_object_equal(km_get(left, "cwd"), km_get(right, "cwd"))) return false;
    return true;
}

static bool same_execution(json_object *left, json_object *right) {
    return same_endpoint(left, right) && json_object_equal(km_get(left, "env"), km_get(right, "env")) &&
           json_object_equal(km_get(left, "cwd"), km_get(right, "cwd"));
}

static int merge(Km *km, json_object *target, json_object *incoming, unsigned depth) {
    if (depth > 32) return km_error(km, "Native MCP options are too deeply nested");
    json_object_object_foreach(incoming, key, value) {
        json_object *old = km_get(target, key);
        if (json_object_is_type(old, json_type_object) && json_object_is_type(value, json_type_object)) {
            if (merge(km, old, value, depth + 1) < 0) return -1;
        } else json_object_object_add(target, key, json_object_get(value));
    }
    return 0;
}

static json_object *replacement(Km *km, json_object *old, json_object *native, const char *client) {
    json_object *parsed = ad_mcp_canonical(km, client, native);
    if (!parsed) return NULL;
    json_object *previous = old ? km_mcp_execution(old, client) : NULL;
    json_object *snapshot = km_get(km_get(old, "native"), client);
    if (snapshot && same_endpoint(previous, parsed)) {
        json_object *merged = copy(km, snapshot);
        if (!merged || merge(km, merged, native, 0) < 0) {
            json_object_put(merged); json_object_put(previous); json_object_put(parsed); return NULL;
        }
        json_object_put(parsed); parsed = ad_mcp_canonical(km, client, merged);
        if (parsed) json_object_object_add(km_get(parsed, "native"), client, merged);
        else json_object_put(merged);
    } else json_object_object_add(km_get(parsed, "native"), client, json_object_get(native));
    json_object_put(previous);
    if (!parsed || !old) return parsed;
    json_object *resource = copy(km, old), *execution = km_mcp_execution(parsed, client);
    if (!resource || !execution) { json_object_put(resource); json_object_put(execution); json_object_put(parsed); return NULL; }
    if (!km_get(resource, "description") && km_get(parsed, "description"))
        json_object_object_add(resource, "description", json_object_get(km_get(parsed, "description")));
    json_object_object_del(execution, "mode");
    if (same_execution(resource, parsed)) {
        json_object *variants = km_get(resource, "variants");
        if (variants) json_object_object_del(variants, client);
        if (variants && !json_object_object_length(variants)) json_object_object_del(resource, "variants");
        json_object_put(execution);
    } else json_object_object_add(object_at(resource, "variants"), client, execution);
    json_object_object_add(object_at(resource, "native"), client,
                           json_object_get(km_get(km_get(parsed, "native"), client)));
    json_object_put(parsed); return resource;
}

static int validate_projection(Km *km, json_object *resource, const char *id, const char *client) {
    json_object *native = ad_native(km, resource, id, client, true);
    if (!native) return -1;
    if (strcmp(client, "codex")) { json_object_put(native); return 0; }
    char *text = ad_toml_render(km, native, id, NULL);
    json_object *parsed = text ? ad_toml_parse(km, text) : NULL;
    bool valid = parsed && json_object_equal(parsed, native);
    free(text); json_object_put(parsed); json_object_put(native);
    return valid ? 0 : km_error(km, "Native MCP options cannot be represented safely in Codex TOML");
}

int km_mcp_native_save(Km *km, int argc, char **argv, bool editing) {
    const char *path = NULL, *client = NULL;
    KmMcpDescription description = {0};
    if (options(km, argc, argv, &path, &client, &description) < 0) return -1;
    json_object *resources = km_resources(km, "mcp"), *old = km_get(resources, argv[1]);
    if (editing != (old != NULL)) return km_error(km, editing ? "Unknown MCP" : "MCP already exists; use edit");
    json_object *native = source(km, path, client);
    if (!native) return -1;
    json_object *resource = replacement(km, old, native, client); json_object_put(native);
    if (!resource) return -1;
    if (km_mcp_description_apply(km, resource, &description) < 0) { json_object_put(resource); return -1; }
    if (validate_projection(km, resource, argv[1], client) < 0) { json_object_put(resource); return -1; }
    if (old && json_object_equal(old, resource)) {
        json_object_put(resource); printf("MCP %s is already current.\n", argv[1]); return 0;
    }
    json_object_object_add(resources, argv[1], resource);
    if (km_save(km) < 0) return -1;
    printf("%s native MCP %s for %s.\n", editing ? "Updated" : "Added", argv[1], client);
    return 0;
}
