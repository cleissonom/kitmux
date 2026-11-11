#include "subagents.h"
#include "adapters.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *path;
    const char *format;
    const char *description;
} SubagentOptions;

static json_object *copy_resource(Km *km, json_object *resource) {
    json_object *copy = NULL;
    if (json_object_deep_copy(resource, &copy, NULL)) km_error(km, "Cannot copy subagent definition");
    return copy;
}

static int options(Km *km, int argc, char **argv, SubagentOptions *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 2; i < argc; i++) {
        const char **field = !strcmp(argv[i], "--from") ? &out->path :
                             !strcmp(argv[i], "--format") ? &out->format :
                             !strcmp(argv[i], "--description") ? &out->description : NULL;
        if (!field || *field || i + 1 >= argc) return km_error(km, "Unknown, repeated, or incomplete subagent option");
        *field = argv[++i];
    }
    if (out->format && strcmp(out->format, "common") && strcmp(out->format, "codex") &&
        strcmp(out->format, "claude") && strcmp(out->format, "opencode") && strcmp(out->format, "opencode-json"))
        return km_error(km, "Use subagent format common, codex, claude, opencode, or opencode-json");
    if (out->format && !out->path) return km_error(km, "--format requires --from FILE");
    return 0;
}

static char *read_source(Km *km, const char *path) {
    size_t length;
    char *text = km_read_file(km, path, &length);
    if (!text) return NULL;
    if (!length || memchr(text, '\0', length)) {
        free(text); km_error(km, "Subagent source must be nonempty text without NUL bytes"); return NULL;
    }
    return text;
}

static json_object *portable_resource(const char *text, const char *description) {
    json_object *resource = json_object_new_object();
    json_object_object_add(resource, "description", json_object_new_string(description));
    json_object_object_add(resource, "instructions", json_object_new_string(text));
    json_object_object_add(resource, "portable", json_object_new_boolean(true));
    json_object_object_add(resource, "enabled", json_object_new_boolean(true));
    json_object_object_add(resource, "agents", json_object_new_object());
    return resource;
}

static int attach_variant(json_object *resource, json_object *parsed, const char *format) {
    const char *client = !strcmp(format, "opencode-json") ? "opencode" : format;
    json_object *native = km_get(resource, "native");
    if (!native) { native = json_object_new_object(); json_object_object_add(resource, "native", native); }
    json_object *snapshot = km_get(km_get(parsed, "native"), client);
    json_object_object_add(snapshot, "description", json_object_get(km_get(resource, "description")));
    json_object_object_add(snapshot, "instructions", json_object_get(km_get(resource, "instructions")));
    json_object_object_add(native, client, json_object_get(snapshot));
    return 0;
}

static bool owned_projection_conflict(AdRun *run, const char *id, bool json_target) {
    json_object *files = km_get(run->state, json_target ? "subagent_files" : "files");
    if (!files) return false;
    char key[80]; snprintf(key, sizeof(key), "agents/%s", id);
    json_object_object_foreach(files, path, record) {
        (void)path;
        if (strcmp(km_string(record, "agent"), "opencode")) continue;
        if (json_target ? !strcmp(km_string(record, "id"), id) : km_get(km_get(record, "entries"), key) != NULL)
            return true;
    }
    return false;
}

static int check_projection_format(Km *km, const char *id, const char *format) {
    if (!format || (strcmp(format, "opencode") && strcmp(format, "opencode-json"))) return 0;
    AdRun run;
    int result = ad_open(&run, km, false);
    if (!result && owned_projection_conflict(&run, id, !strcmp(format, "opencode-json")))
        result = km_error(km, "Native format differs from its managed projection; use the existing format or a separate profile");
    ad_cleanup(&run);
    return result;
}

static json_object *replacement(Km *km, const char *id, json_object *existing, SubagentOptions *choice) {
    bool common = !choice->format || !strcmp(choice->format, "common");
    char *text = choice->path ? read_source(km, choice->path) : NULL;
    if (choice->path && !text) return NULL;
    json_object *resource = existing ? copy_resource(km, existing) : NULL;
    if (existing && !resource) { free(text); return NULL; }
    if (!existing && common) {
        if (!choice->description) km_error(km, "Common subagents require --description TEXT");
        else resource = portable_resource(text, choice->description);
    } else if (text && !common) {
        json_object *parsed = km_subagent_parse(km, text, choice->format, id);
        if (!parsed) { free(text); json_object_put(resource); return NULL; }
        if (existing) { attach_variant(resource, parsed, choice->format); json_object_put(parsed); }
        else resource = parsed;
    } else if (text && resource) json_object_object_add(resource, "instructions", json_object_new_string(text));
    free(text);
    if (!resource) return NULL;
    if (choice->description) json_object_object_add(resource, "description", json_object_new_string(choice->description));
    return resource;
}

static int define(Km *km, int argc, char **argv, bool editing) {
    const char *id = argv[1];
    if (!km_valid_id(id)) return km_error(km, "Use a subagent ID with 1-64 letters, digits, hyphens or underscores");
    json_object *resources = km_resources(km, "subagent"), *existing = km_get(resources, id);
    if (editing != (existing != NULL)) return km_error(km, editing ? "Unknown subagent: %s" : "Subagent already exists: %s", id);
    SubagentOptions choice;
    if (options(km, argc, argv, &choice) < 0) return -1;
    if (editing && check_projection_format(km, id, choice.format) < 0) return -1;
    if ((!editing && !choice.path) || (editing && !choice.path && !choice.description))
        return km_error(km, "Usage: subagent add|edit ID --from FILE [--format FORMAT] [--description TEXT]");
    json_object *resource = replacement(km, id, existing, &choice);
    if (!resource) return -1;
    if (km_subagent_validate(km, id, resource) < 0) { json_object_put(resource); return -1; }
    json_object_object_add(resources, id, resource);
    if (km_save(km) < 0) return -1;
    printf("%s subagent %s. Native clients may need a reload.\n", editing ? "Updated" : "Added", id);
    return 0;
}

static int rename_variants(Km *km, json_object *resource, const char *id) {
    json_object *native = km_get(resource, "native");
    if (!native) return 0;
    for (size_t i = 0; i < KM_AGENT_COUNT; i++) {
        json_object *snapshot = km_get(native, km_agents[i]);
        if (!snapshot) continue;
        const char *format = km_string(snapshot, "format");
        char *text = km_subagent_render(km, resource, id, km_agents[i]);
        if (!text) return -1;
        json_object *parsed = km_subagent_parse(km, text, format, id); free(text);
        if (!parsed) return -1;
        attach_variant(resource, parsed, format); json_object_put(parsed);
    }
    return 0;
}

static int duplicate(Km *km, const char *id, const char *new_id) {
    json_object *resources = km_resources(km, "subagent"), *existing = km_get(resources, id);
    if (!existing) return km_error(km, "Unknown subagent: %s", id);
    if (!km_valid_id(new_id) || km_get(resources, new_id)) return km_error(km, "New subagent ID is invalid or already exists");
    json_object *resource = copy_resource(km, existing);
    if (!resource) return -1;
    json_object_object_del(resource, "origins");
    json_object_object_del(resource, "source");
    json_object_object_add(resource, "enabled", json_object_new_boolean(true));
    json_object_object_add(resource, "agents", json_object_new_object());
    if (rename_variants(km, resource, new_id) < 0) { json_object_put(resource); return -1; }
    json_object_object_add(resources, new_id, resource);
    if (km_save(km) < 0) return -1;
    printf("Duplicated %s as %s; choose a group or client to enable it.\n", id, new_id);
    return 0;
}

static json_object *summary(json_object *resource, bool detail) {
    json_object *result = json_object_new_object();
    const char *fields[] = {"description", "enabled", "agents", "portable", "instructions"};
    for (size_t i = 0; i < (detail ? 5u : 4u); i++) {
        json_object *value = km_get(resource, fields[i]);
        if (value) json_object_object_add(result, fields[i], json_object_get(value));
    }
    json_object *clients = json_object_new_array();
    for (size_t i = 0; i < KM_AGENT_COUNT; i++) {
        if (km_get(km_get(resource, "native"), km_agents[i]))
            json_object_array_add(clients, json_object_new_string(km_agents[i]));
    }
    json_object_object_add(result, "native_clients", clients);
    return result;
}

static int list(Km *km, bool as_json) {
    json_object *resources = km_resources(km, "subagent"), *result = json_object_new_object();
    if (!as_json) printf("%-24s %-5s %-8s %-8s %-8s\n", "SUBAGENT", "STATE", "CODEX", "CLAUDE", "OPENCODE");
    json_object_object_foreach(resources, id, resource) {
        if (as_json) json_object_object_add(result, id, summary(resource, false));
        else {
            printf("%-24s %-5s", id, json_object_get_boolean(km_get(resource, "enabled")) ? "ON" : "OFF");
            for (size_t i = 0; i < KM_AGENT_COUNT; i++) {
                bool compatible = km_subagent_compatible(resource, km_agents[i], NULL, 0);
                printf(" %-8s", compatible ? (km_effective(km, "subagent", id, km_agents[i], NULL, 0) ? "ON" : "OFF") : "N/A");
            }
            putchar('\n');
        }
    }
    if (as_json) puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY));
    json_object_put(result); return 0;
}

static int import(Km *km, int argc, char **argv) {
    char **args = calloc((size_t)argc + 1, sizeof(*args));
    if (!args) return km_error(km, "Out of memory");
    args[0] = "import"; args[1] = "--subagents";
    for (int i = 1; i < argc; i++) args[i + 1] = argv[i];
    int result = km_adapter_command(km, argc + 1, args); free(args);
    return result;
}

int km_subagent_command(Km *km, int argc, char **argv) {
    if (!argc || !strcmp(argv[0], "list")) {
        if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json"))) return km_error(km, "Usage: subagent list [--json]");
        return list(km, argc == 2);
    }
    if (!strcmp(argv[0], "import")) return import(km, argc, argv);
    if (argc < 2) return km_error(km, "Expected subagent command and ID; run --help");
    const char *verb = argv[0], *id = argv[1];
    if (!strcmp(verb, "add") || !strcmp(verb, "edit")) return define(km, argc, argv, !strcmp(verb, "edit"));
    if (!strcmp(verb, "duplicate") && argc == 3) return duplicate(km, id, argv[2]);
    json_object *resource = km_get(km_resources(km, "subagent"), id);
    if (!resource) return km_error(km, "Unknown subagent: %s", id);
    if (!strcmp(verb, "show") && (argc == 2 || (argc == 3 && !strcmp(argv[2], "--json")))) {
        json_object *result = summary(resource, true);
        puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY)); json_object_put(result); return 0;
    }
    if (!strcmp(verb, "remove") && argc == 2) return km_remove_resource(km, "subagent", id);
    if (!strcmp(verb, "enable") || !strcmp(verb, "disable") || !strcmp(verb, "inherit")) return km_resource_toggle(km, "subagent", argc, argv);
    return km_error(km, "Invalid subagent command or arguments; run --help");
}
