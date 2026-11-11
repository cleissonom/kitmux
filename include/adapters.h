#ifndef KITMUX_ADAPTERS_H
#define KITMUX_ADAPTERS_H
#include "kitmux.h"

typedef struct AdFile {
    char path[PATH_MAX];
    char agent[16];
    char *original;
    char *text;
    bool existed;
    bool toml;
    struct AdFile *next;
} AdFile;

typedef struct {
    Km *km;
    json_object *state;
    AdFile *files;
    json_object *links;
    json_object *management;
    json_object *subagent_actions;
    bool apply;
    bool restore;
    bool refresh;
    bool check;
} AdRun;

json_object *ad_parse(Km *km, const char *text);
char *ad_json_raw(Km *km, const char *text, const char *path);
int ad_json_edit(Km *km, char **text, const char *path, const char *value);
int ad_array_append(Km *km, char **text, const char *value);
int ad_replace(Km *km, char **text, size_t start, size_t end, const char *value);
bool ad_same(Km *km, const char *a, const char *b, bool toml);
AdFile *ad_file(AdRun *run, const char *agent, const char *relative, bool toml);
int ad_change(AdRun *run, AdFile *file, const char *key, const char *desired, bool adopt);
int ad_finish(AdRun *run);
void ad_cleanup(AdRun *run);
int ad_open(AdRun *run, Km *km, bool apply);
int ad_validate_state(Km *km, json_object *state);
int ad_native_shape(Km *km, const char *agent, const char *path, json_object *doc);
int ad_server_shape(Km *km, const char *agent, json_object *server);
int ad_sync_mcp(AdRun *run, const char *agent);
int ad_import(AdRun *run, const char *agent);
int ad_import_skills(AdRun *run, const char *agent);
int ad_import_subagents(AdRun *run, const char *agent);
int ad_sync_subagents(AdRun *run, const char *agent);
int ad_restore_subagents(AdRun *run, const char *agent);
int ad_subagent_origins_valid(Km *km, json_object *resource, const char *id);
bool ad_subagent_path_valid(Km *km, const char *path, const char *agent, const char *id);
int ad_subagent_parent_safe(Km *km, const char *path);
int ad_subagent_flat_discovery(Km *km, const char *agent);
int ad_subagent_change(AdRun *run, const char *agent, const char *path,
                       const char *id, const char *format, const char *desired, bool adopt);
int ad_subagent_state_valid(Km *km, json_object *state);
int ad_subagent_check_actions(AdRun *run);
int ad_subagent_apply_actions(AdRun *run);
void ad_subagent_preview(AdRun *run);
int ad_adopt_skill_link(AdRun *run, const char *agent, const char *path, const char *target);
int ad_skill_native_enabled(AdRun *run, const char *agent, const char *id, const char *source, bool *enabled);
int ad_skill_bindings_valid(Km *km, json_object *skill, const char *id);
int ad_sync_skills(AdRun *run, const char *agent);
int ad_restore_links(AdRun *run, const char *agent);
int ad_restore(AdRun *run, const char *agent);
AdFile *ad_native_file(AdRun *run, const char *agent);
json_object *ad_native(Km *km, json_object *resource, const char *id, const char *agent, bool enabled);
json_object *ad_mcp_canonical(Km *km, const char *agent, json_object *native);
char *ad_toml_raw(Km *km, const char *text, const char *id);
int ad_toml_edit(Km *km, char **text, const char *id, const char *value);
json_object *ad_toml_ids(Km *km, const char *text);
json_object *ad_toml_parse(Km *km, const char *raw);
char *ad_toml_render(Km *km, json_object *native, const char *id, const char *base);
#endif
