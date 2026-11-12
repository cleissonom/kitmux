#include "kitmux.h"
#include "subagents.h"
#include "adapters.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const km_agents[KM_AGENT_COUNT] = {"codex", "claude", "opencode"};

bool km_valid_agent(const char *agent) {
    if (!agent) return false;
    for (size_t i = 0; i < KM_AGENT_COUNT; ++i) if (!strcmp(agent, km_agents[i])) return true;
    return false;
}

static int storage_path(Km *km, char *out, const char *env, const char *fallback) {
    const char *value = km->isolated ? NULL : getenv(env);
    if (value && *value && value[0] != '/') return km_error(km, "%s must be an absolute path", env);
    return value && *value ? km_path(km, out, PATH_MAX, "%s/kitmux", value)
                          : km_path(km, out, PATH_MAX, "%s/%s/kitmux", km->home, fallback);
}

int km_init(Km *km, const char *home) {
    memset(km, 0, sizeof(*km));
    km->lock_fd = -1;
    km->isolated = home != NULL;
    const char *selected = home ? home : getenv("HOME");
    struct stat st;
    if (!selected || selected[0] != '/' || !realpath(selected, km->home) ||
        stat(km->home, &st) < 0 || !S_ISDIR(st.st_mode))
        return km_error(km, "Home must be an existing absolute directory");
    if (storage_path(km, km->config, "XDG_CONFIG_HOME", ".config") < 0 ||
        storage_path(km, km->state, "XDG_STATE_HOME", ".local/state") < 0 ||
        storage_path(km, km->data, "XDG_DATA_HOME", ".local/share") < 0) return -1;
    return 0;
}

int km_lock(Km *km) {
    char path[PATH_MAX];
    if (km->lock_fd >= 0) return km_error(km, "Catalog lock is already held");
    if (km_mkdirs(km, km->config) < 0 || km_path(km, path, sizeof(path), "%s/catalog.lock", km->config) < 0) return -1;
    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0) return km_error(km, "Cannot open catalog lock: %s", strerror(errno));
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd); return km_error(km, "Catalog is busy or its lock is invalid; retry shortly");
    }
    km->lock_fd = fd;
    return 0;
}

void km_unlock(Km *km) {
    if (km->lock_fd < 0) return;
    flock(km->lock_fd, LOCK_UN);
    close(km->lock_fd);
    km->lock_fd = -1;
}

static json_object *empty_catalog(void) {
    return json_tokener_parse("{\"version\":1,\"revision\":0,\"mcps\":{},\"skills\":{},\"subagents\":{},\"groups\":{}}");
}

static bool string_array(json_object *array) {
    if (!json_object_is_type(array, json_type_array)) return false;
    for (size_t i = 0; i < json_object_array_length(array); ++i) {
        json_object *v = json_object_array_get_idx(array, i);
        if (!json_object_is_type(v, json_type_string) ||
            strlen(json_object_get_string(v)) != (size_t)json_object_get_string_len(v)) return false;
    }
    return true;
}

static bool plain_string(json_object *value) {
    return json_object_is_type(value, json_type_string) &&
           strlen(json_object_get_string(value)) == (size_t)json_object_get_string_len(value);
}

static int validate_execution(Km *km, const char *id, json_object *server, const char *mode) {
    const char *transport = km_string(server, "transport");
    if (!plain_string(km_get(server, "transport")) || (strcmp(transport, "stdio") && strcmp(transport, "http")))
        return km_error(km, "Invalid MCP transport for %s", id);
    if (!strcmp(transport, "stdio")) {
        json_object *command = km_get(server, "command");
        if (!string_array(command) || !json_object_array_length(command) ||
            !*json_object_get_string(json_object_array_get_idx(command, 0))) return km_error(km, "Invalid MCP command for %s", id);
    } else {
        const char *url = km_string(server, "url");
        if (!plain_string(km_get(server, "url")) || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)))
            return km_error(km, "Invalid MCP URL for %s", id);
        if (mode && !strcmp(mode, "managed")) return km_error(km, "Managed HTTP is unsupported");
    }
    if (km_get(server, "cwd") && !plain_string(km_get(server, "cwd"))) return km_error(km, "Invalid MCP working directory");
    json_object *env = km_get(server, "env");
    if (env) {
        if (!json_object_is_type(env, json_type_object)) return km_error(km, "Invalid MCP environment");
        json_object_object_foreach(env, name, value) {
            if (!*name || strchr(name, '=') || !plain_string(value)) return km_error(km, "Invalid MCP environment value for %s", id);
        }
    }
    return 0;
}

static bool variant_field(const char *key) {
    const char *fields[] = {"transport", "command", "url", "env", "cwd"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (!strcmp(key, fields[i])) return true;
    return false;
}

static int validate_variants(Km *km, const char *id, json_object *variants, const char *mode) {
    if (!json_object_is_type(variants, json_type_object)) return km_error(km, "Invalid MCP execution variants for %s", id);
    json_object_object_foreach(variants, agent, execution) {
        if (!km_valid_agent(agent) || !json_object_is_type(execution, json_type_object))
            return km_error(km, "Invalid MCP execution variant agent or value for %s", id);
        json_object_object_foreach(execution, key, value) {
            (void)value;
            if (!variant_field(key)) return km_error(km, "Unsupported MCP execution variant field for %s", id);
        }
        if (validate_execution(km, id, execution, mode) < 0) return -1;
        const char *transport = km_string(execution, "transport");
        json_object *extra = NULL;
        if (json_object_object_get_ex(execution, !strcmp(transport, "stdio") ? "url" : "command", &extra))
            return km_error(km, "MCP execution variant has conflicting endpoints for %s", id);
    }
    return 0;
}

static int validate_native_snapshots(Km *km, const char *id, json_object *native) {
    if (!json_object_is_type(native, json_type_object)) return km_error(km, "Invalid native MCP snapshots for %s", id);
    json_object_object_foreach(native, agent, snapshot) {
        if (!km_valid_agent(agent) || !json_object_is_type(snapshot, json_type_object))
            return km_error(km, "Invalid native MCP client snapshot for %s", id);
    }
    return 0;
}

static int validate_mcp(Km *km, const char *id, json_object *server) {
    json_object *description = NULL;
    if (json_object_object_get_ex(server, "description", &description) &&
        (!plain_string(description) || (size_t)json_object_get_string_len(description) > KM_MAX_FILE))
        return km_error(km, "Invalid MCP description for %s", id);
    const char *mode = km_string(server, "mode");
    if (km_get(server, "mode") && (!plain_string(km_get(server, "mode")) || (strcmp(mode, "direct") && strcmp(mode, "managed"))))
        return km_error(km, "Invalid MCP mode for %s", id);
    if (mode && !strcmp(mode, "managed") && !strcmp(id, "ai-memory"))
        return km_error(km, "Session-bound ai-memory must use direct mode");
    if (validate_execution(km, id, server, mode) < 0) return -1;
    json_object *variants = NULL;
    if (json_object_object_get_ex(server, "variants", &variants) && validate_variants(km, id, variants, mode) < 0) return -1;
    json_object *native = NULL;
    if (json_object_object_get_ex(server, "native", &native) && validate_native_snapshots(km, id, native) < 0) return -1;
    return 0;
}

static int validate_group(Km *km, json_object *catalog, const char *id, json_object *group) {
    json_object *agents = km_get(group, "agents"), *members = km_get(group, "members");
    const char *scope = km_string(group, "kind"), *group_name = km_string(group, "name");
    int version = json_object_get_int(km_get(catalog, "version"));
    if (version == 3) {
        char key[80];
        if (!scope || !group_name || !plain_string(km_get(group, "kind")) || !plain_string(km_get(group, "name")) ||
            (strcmp(scope, "mcp") && strcmp(scope, "skill") && strcmp(scope, "subagent")) || !km_valid_id(group_name) ||
            km_path(km, key, sizeof(key), "%s:%s", scope, group_name) < 0 || strcmp(key, id))
            return km_error(km, "Invalid scoped group");
    } else if (!km_valid_id(id) || scope || km_get(group, "name")) return km_error(km, "Invalid legacy group");
    if (!string_array(agents) || !string_array(members) || !json_object_is_type(km_get(group, "enabled"), json_type_boolean))
        return km_error(km, "Invalid group %s", id);
    for (size_t i = 0; i < json_object_array_length(agents); ++i)
        if (!km_valid_agent(json_object_get_string(json_object_array_get_idx(agents, i)))) return km_error(km, "Invalid group agent");
    for (size_t i = 0; i < json_object_array_length(members); ++i) {
        const char *s = json_object_get_string(json_object_array_get_idx(members, i));
        const char *name = NULL, *map = NULL;
        if (!strncmp(s, "mcp:", 4)) { name = s + 4; map = "mcps"; }
        else if (!strncmp(s, "skill:", 6)) { name = s + 6; map = "skills"; }
        else if (!strncmp(s, "subagent:", 9)) { name = s + 9; map = "subagents"; }
        if (!name || !km_valid_id(name) || !km_get(km_get(catalog, map), name) ||
            (scope && (strncmp(s, scope, strlen(scope)) || s[strlen(scope)] != ':')))
            return km_error(km, "Invalid member in group %s", id);
    }
    return 0;
}

static int validate_resource(Km *km, const char *id, json_object *value) {
    if (!km_valid_id(id) || !json_object_is_type(value, json_type_object)) return km_error(km, "Invalid catalog resource");
    json_object *enabled = km_get(value, "enabled"), *agents = km_get(value, "agents");
    if (enabled && !json_object_is_type(enabled, json_type_boolean)) return km_error(km, "Invalid enabled flag for %s", id);
    if (agents && !json_object_is_type(agents, json_type_object)) return km_error(km, "Invalid agent overrides for %s", id);
    if (agents) {
        json_object_object_foreach(agents, agent, state) {
            const char *s = json_object_get_string(state);
            if (!km_valid_agent(agent) || !plain_string(state) ||
                (strcmp(s, "on") && strcmp(s, "off") && strcmp(s, "inherit")))
                return km_error(km, "Invalid agent override for %s", id);
        }
    }
    return 0;
}

static int validate_catalog(Km *km, json_object *catalog) {
    json_object *version = km_get(catalog, "version"), *revision = km_get(catalog, "revision");
    if (!json_object_is_type(version, json_type_int) || (json_object_get_int(version) < 1 || json_object_get_int(version) > 3) ||
        !json_object_is_type(revision, json_type_int) || json_object_get_int64(revision) < 0)
        return km_error(km, "Unsupported or invalid catalog version/revision");
    const char *maps[] = {"mcps", "skills", "subagents", "groups"};
    for (size_t i = 0; i < 4; ++i) {
        json_object *map = km_get(catalog, maps[i]);
        if (!json_object_is_type(map, json_type_object)) return km_error(km, "Invalid catalog %s", maps[i]);
        json_object_object_foreach(map, id, value) {
            if ((i < 3 && !km_valid_id(id)) || !json_object_is_type(value, json_type_object)) return km_error(km, "Invalid catalog entry");
            if (i < 3 && validate_resource(km, id, value) < 0) return -1;
            if (i == 0 && validate_mcp(km, id, value) < 0) return -1;
            if (i == 2 && km_subagent_validate(km, id, value) < 0) return -1;
            if (i == 2 && ad_subagent_origins_valid(km, value, id) < 0) return -1;
            if (i == 3 && validate_group(km, catalog, id, value) < 0) return -1;
        }
    }
    return 0;
}

int km_load(Km *km) {
    char path[PATH_MAX];
    if (km_path(km, path, sizeof(path), "%s/catalog.json", km->config) < 0) return -1;
    struct stat st;
    json_object *catalog = NULL;
    if (lstat(path, &st) < 0 && errno == ENOENT) catalog = empty_catalog();
    else {
        size_t size;
        char *text = km_read_file(km, path, &size);
        if (!text) return -1;
        json_tokener *tok = json_tokener_new();
        json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        catalog = json_tokener_parse_ex(tok, text, (int)size + 1);
        bool valid = json_tokener_get_error(tok) == json_tokener_success && !memchr(text, '\0', size);
        free(text); json_tokener_free(tok);
        if (!valid) { json_object_put(catalog); return km_error(km, "Catalog is not valid JSON; file preserved"); }
    }
    json_object *subagents = NULL;
    if (catalog && json_object_get_int(km_get(catalog, "version")) == 1 &&
        !json_object_object_get_ex(catalog, "subagents", &subagents))
        json_object_object_add(catalog, "subagents", json_object_new_object());
    if (!catalog || validate_catalog(km, catalog) < 0) { json_object_put(catalog); return -1; }
    json_object_put(km->catalog);
    km->catalog = catalog;
    return 0;
}

int km_save(Km *km) {
    if (km->lock_fd < 0) return km_error(km, "Cannot save without catalog lock");
    if (validate_catalog(km, km->catalog) < 0) return -1;
    int64_t revision = json_object_get_int64(km_get(km->catalog, "revision"));
    if (revision == INT64_MAX) return km_error(km, "Catalog revision exhausted");
    int version = json_object_get_int(km_get(km->catalog, "version"));
    if (version < 2 && json_object_object_length(km_resources(km, "subagent")))
        json_object_object_add(km->catalog, "version", json_object_new_int(2));
    json_object_object_add(km->catalog, "revision", json_object_new_int64(revision + 1));
    const char *text = json_object_to_json_string_ext(km->catalog, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
    char path[PATH_MAX];
    if (km_path(km, path, sizeof(path), "%s/catalog.json", km->config) < 0) return -1;
    int result = km_write_file(km, path, text, strlen(text), 0600);
    if (result < 0) {
        json_object_object_add(km->catalog, "revision", json_object_new_int64(revision));
        json_object_object_add(km->catalog, "version", json_object_new_int(version));
    }
    return result;
}

void km_close(Km *km) {
    km_unlock(km);
    json_object_put(km->catalog);
    km->catalog = NULL;
}

json_object *km_resources(Km *km, const char *kind) {
    if (!strcmp(kind, "mcp")) return km_get(km->catalog, "mcps");
    if (!strcmp(kind, "skill")) return km_get(km->catalog, "skills");
    if (!strcmp(kind, "subagent")) return km_get(km->catalog, "subagents");
    return NULL;
}
