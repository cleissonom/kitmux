#include "kitmux.h"
#include "mcp_metadata.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool environment_name(const char *value, size_t length) {
    if (!length || !(isalpha((unsigned char)value[0]) || value[0] == '_')) return false;
    for (size_t i = 1; i < length; ++i) if (!isalnum((unsigned char)value[i]) && value[i] != '_') return false;
    return true;
}

static int add_env(Km *km, json_object *server, const char *value) {
    const char *eq = strchr(value, '=');
    if (!eq || !environment_name(value, (size_t)(eq - value))) return km_error(km, "Environment must be NAME=VALUE");
    char name[256];
    if ((size_t)(eq - value) >= sizeof(name)) return km_error(km, "Environment name is too long");
    memcpy(name, value, (size_t)(eq - value)); name[eq - value] = '\0';
    json_object *env = km_get(server, "env");
    if (!env) { env = json_object_new_object(); json_object_object_add(server, "env", env); }
    json_object_object_add(env, name, json_object_new_string(eq + 1));
    return 0;
}

static int execution_option(Km *km, json_object *execution, const char *option, const char *value) {
    if (!strcmp(option, "--command")) {
        if (!*value) return km_error(km, "Command cannot be empty");
        json_object *command = json_object_new_array();
        json_object_array_add(command, json_object_new_string(value));
        json_object_object_add(execution, "command", command);
        json_object_object_add(execution, "transport", json_object_new_string("stdio"));
        json_object_object_del(execution, "url");
    } else if (!strcmp(option, "--url")) {
        if (strncmp(value, "https://", 8) && strncmp(value, "http://", 7)) return km_error(km, "MCP URL must use HTTP or HTTPS");
        json_object_object_add(execution, "url", json_object_new_string(value));
        json_object_object_add(execution, "transport", json_object_new_string("http"));
        json_object_object_del(execution, "command");
    } else if (!strcmp(option, "--cwd")) json_object_object_add(execution, "cwd", json_object_new_string(value));
    else if (!strcmp(option, "--env")) return add_env(km, execution, value);
    else return km_error(km, "Unknown MCP option (use -- before command arguments)");
    return 0;
}

static int shared_option(Km *km, json_object *server, const char *option, const char *value) {
    if (!strcmp(option, "--mode") && (!strcmp(value, "direct") || !strcmp(value, "managed"))) {
        json_object_object_add(server, "mode", json_object_new_string(value));
        return 0;
    }
    if (execution_option(km, server, option, value) < 0) return -1;
    json_object *variants = km_get(server, "variants");
    if (variants) {
        json_object_object_foreach(variants, agent, execution) {
            (void)agent;
            if (execution_option(km, execution, option, value) < 0) return -1;
        }
    }
    return 0;
}

static int append_arguments(Km *km, json_object *execution, int argc, char **argv) {
    json_object *command = km_get(execution, "command");
    if (!command) return km_error(km, "Arguments require a stdio command for every mapped agent");
    for (int i = 0; i < argc; i++) json_object_array_add(command, json_object_new_string(argv[i]));
    return 0;
}

static int server_options(Km *km, json_object *server, int argc, char **argv) {
    KmMcpDescription description = {0};
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) {
            if (append_arguments(km, server, argc - i - 1, argv + i + 1) < 0) return -1;
            json_object *variants = km_get(server, "variants");
            if (variants) {
                json_object_object_foreach(variants, agent, execution) {
                    (void)agent;
                    if (append_arguments(km, execution, argc - i - 1, argv + i + 1) < 0) return -1;
                }
            }
            return km_mcp_description_apply(km, server, &description);
        }
        if (i + 1 >= argc) return km_error(km, "MCP option needs a value");
        const char *option = argv[i++], *value = argv[i];
        int metadata = km_mcp_description_option(km, &description, option, value);
        if (metadata < 0) return -1;
        if (metadata) continue;
        if (shared_option(km, server, option, value) < 0) return -1;
    }
    return km_mcp_description_apply(km, server, &description);
}

static bool relative_command_moved(json_object *before, json_object *after) {
    if (json_object_equal(km_get(before, "cwd"), km_get(after, "cwd"))) return false;
    json_object *command = km_get(before, "command");
    const char *program = command ? json_object_get_string(json_object_array_get_idx(command, 0)) : NULL;
    return program && program[0] != '/';
}

static bool endpoint_changed(json_object *old, json_object *server) {
    const char *keys[] = {"transport", "command", "url"};
    for (size_t i = 0; i < KM_AGENT_COUNT; i++) {
        json_object *before = km_mcp_execution(old, km_agents[i]);
        json_object *after = km_mcp_execution(server, km_agents[i]);
        bool different = !before || !after;
        for (size_t j = 0; !different && j < sizeof(keys) / sizeof(keys[0]); j++)
            different = !json_object_equal(km_get(before, keys[j]), km_get(after, keys[j]));
        if (!different) different = relative_command_moved(before, after);
        json_object_put(before); json_object_put(after);
        if (different) return true;
    }
    return false;
}

static int server_save(Km *km, int argc, char **argv, bool edit) {
    if (argc < 2 || !km_valid_id(argv[1])) return km_error(km, "MCP requires a valid server ID");
    for (int i = 2; i < argc && strcmp(argv[i], "--"); i += 2)
        if (!strcmp(argv[i], "--from")) return km_mcp_native_save(km, argc, argv, edit);
    json_object *resources = km_resources(km, "mcp"), *old = km_get(resources, argv[1]), *server = NULL;
    if (edit != (old != NULL)) return km_error(km, edit ? "Unknown MCP" : "MCP already exists; use edit");
    if (old) json_object_deep_copy(old, &server, NULL);
    else server = json_tokener_parse("{\"enabled\":true,\"agents\":{},\"mode\":\"direct\"}");
    if (server_options(km, server, argc - 2, argv + 2) < 0) { json_object_put(server); return -1; }
    const char *transport = km_string(server, "transport"), *mode = km_string(server, "mode");
    if (!transport || (mode && !strcmp(mode, "managed") && (strcmp(transport, "stdio") || !strcmp(argv[1], "ai-memory")))) {
        json_object_put(server); return km_error(km, "Specify --command or --url; managed mode requires a session-independent local stdio server");
    }
    /* The adapter preserves native auth fields. Never move those to another endpoint. */
    if (old && km_get(old, "native") && endpoint_changed(old, server)) {
        json_object_put(server);
        return km_error(km, "Imported endpoints carry native settings; add the changed endpoint under a new ID and review its configuration");
    }
    if (old && json_object_equal(old, server)) {
        json_object_put(server); printf("MCP %s is already current.\n", argv[1]); return 0;
    }
    json_object_object_add(resources, argv[1], server);
    return km_save(km);
}

static int server_list(Km *km) {
    puts("MCP                    TRANSPORT  MODE       CODEX  CLAUDE  OPENCODE");
    json_object *servers = km_resources(km, "mcp");
    json_object_object_foreach(servers, id, server) {
        const char *transport = km_string(server, "transport"), *mode = km_string(server, "mode");
        printf("%-22s %-10s %-10s", id, transport ? transport : "unknown", mode ? mode : "direct");
        for (size_t i = 0; i < KM_AGENT_COUNT; ++i) printf(" %-7s", km_effective(km, "mcp", id, km_agents[i], NULL, 0) ? "on" : "off");
        putchar('\n');
    }
    return 0;
}

static int server_status(Km *km, int argc, char **argv) {
    const char *agent = NULL;
    bool json = false;
    if (argc < 2 || !km_valid_id(argv[1])) return km_error(km, "MCP status requires a valid server ID");
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--agent") && !agent && i + 1 < argc) agent = argv[++i];
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else return km_error(km, "Usage: mcp status ID --agent AGENT [--json]");
    }
    if (!agent || !km_valid_agent(agent)) return km_error(km, "MCP status requires --agent codex|claude|opencode");
    return km_mcp_status(km, argv[1], agent, json);
}

int km_mcp_command(Km *km, int argc, char **argv) {
    if (!argc || (!strcmp(argv[0], "list") && argc == 1)) return server_list(km);
    if (!strcmp(argv[0], "list") && argc == 2 && !strcmp(argv[1], "--json")) return km_status(km, true);
    if (!strcmp(argv[0], "add") || !strcmp(argv[0], "edit")) return server_save(km, argc, argv, !strcmp(argv[0], "edit"));
    if (!strcmp(argv[0], "enable") || !strcmp(argv[0], "disable") || !strcmp(argv[0], "inherit")) return km_resource_toggle(km, "mcp", argc, argv);
    if (!strcmp(argv[0], "remove") && argc == 2) return km_remove_resource(km, "mcp", argv[1]);
    if (!strcmp(argv[0], "show") && argc == 2) {
        if (!km_get(km_resources(km, "mcp"), argv[1])) return km_error(km, "Unknown MCP");
        for (size_t i = 0; i < KM_AGENT_COUNT; ++i) {
            char reason[128]; bool on = km_effective(km, "mcp", argv[1], km_agents[i], reason, sizeof(reason));
            printf("%s: %s — %s\n", km_agents[i], on ? "on" : "off", reason);
        }
        return 0;
    }
    if (!strcmp(argv[0], "reconnect")) return km_runtime_command(km, argc, argv);
    if (!strcmp(argv[0], "status")) return server_status(km, argc, argv);
    return km_error(km, "Usage: mcp add|edit|list|show|enable|disable|inherit|remove|reconnect|status");
}
