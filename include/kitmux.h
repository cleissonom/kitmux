#ifndef KITMUX_H
#define KITMUX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <limits.h>
#include <json-c/json.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define KM_VERSION "0.5.0"
#define KM_MAX_FILE (8u * 1024u * 1024u)
#define KM_AGENT_COUNT 3

typedef struct {
    char home[PATH_MAX];
    char config[PATH_MAX];
    char state[PATH_MAX];
    char data[PATH_MAX];
    char error[1024];
    int lock_fd;
    bool isolated;
    json_object *catalog;
} Km;

extern const char *const km_agents[KM_AGENT_COUNT];
int km_error(Km *km, const char *format, ...);
int km_path(Km *km, char *out, size_t size, const char *format, ...);
int km_mkdirs(Km *km, const char *path);
char *km_read_file(Km *km, const char *path, size_t *length);
int km_write_file(Km *km, const char *path, const void *data, size_t length, unsigned mode);
int km_init(Km *km, const char *home);
int km_lock(Km *km);
void km_unlock(Km *km);
int km_load(Km *km);
int km_save(Km *km);
void km_close(Km *km);
bool km_valid_id(const char *id);
bool km_valid_agent(const char *agent);
json_object *km_get(json_object *obj, const char *key);
const char *km_string(json_object *obj, const char *key);
json_object *km_resources(Km *km, const char *kind);
/* Returns an owned normalized execution; policy/mode are shared by all variants. */
json_object *km_mcp_execution(json_object *server, const char *agent);
bool km_effective(Km *km, const char *kind, const char *id, const char *agent,
                  char *reason, size_t reason_size);
int km_resource_toggle(Km *km, const char *kind, int argc, char **argv);
int km_remove_resource(Km *km, const char *kind, const char *id);
int km_dispatch(Km *km, int argc, char **argv);
int km_group_command(Km *km, int argc, char **argv);
int km_scoped_group_command(Km *km, const char *kind, int argc, char **argv);
json_object *km_group_get(Km *km, const char *kind, const char *id);
bool km_groups_need_migration(Km *km);
int km_groups_migrate(Km *km, int argc, char **argv);
int km_mcp_command(Km *km, int argc, char **argv);
int km_mcp_native_save(Km *km, int argc, char **argv, bool editing);
int km_skill_command(Km *km, int argc, char **argv);
int km_subagent_command(Km *km, int argc, char **argv);
int km_adapter_command(Km *km, int argc, char **argv);
int km_runtime_command(Km *km, int argc, char **argv);
int km_tui(Km *km);
int km_status(Km *km, bool json);
int km_mcp_status(Km *km, const char *id, const char *agent, bool as_json);
void km_help(FILE *out);

/* Commands receive argv starting with their verb (e.g. add, list, reconnect).
 * Main holds the catalog lock and loads it for mutations; bridge/TUI manage
 * their own command lifetimes. Successful mutations call km_save exactly once.
 * Resources are maps in catalog.mcps / catalog.skills / catalog.subagents;
 * groups are catalog.groups. Scoped groups use catalog version 3 and kind:ID keys.
 * Resource: enabled bool (default true), agents map with on/off/inherit values.
 * MCP: transport stdio/http, command array, url, cwd, env object, mode direct/managed,
 * optional description Markdown stored locally without native projection changes.
 * variants[agent] is a full execution (transport/command-or-url/env?/cwd?) without
 * optional-field inheritance. Shared explicit edits update every mapped variant.
 * Skill: path (complete immutable package), source, revision, subdir, hash.
 * Subagent: description/instructions, portable bool, native client snapshots.
 * Group: kind/name strings, enabled bool, agents array, same-kind typed members.
 * km_write_file is atomic and refuses final symlinks. Errors return -1 and set error.
 */
#endif
