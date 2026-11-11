#include "adapters.h"
#include "subagents.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int default_path(Km *km, char *path, const char *agent, const char *id) {
    const char *root=!strcmp(agent,"codex") ? ".codex/agents" : !strcmp(agent,"claude") ? ".claude/agents" : ".config/opencode/agents";
    return km_path(km,path,PATH_MAX,"%s/%s/%s.%s",km->home,root,id,!strcmp(agent,"codex") ? "toml" : "md");
}

static int opencode_collision(AdRun *run, const char *id, bool json_origin);

static int sync_json(AdRun *run, json_object *resource, const char *id,
                      json_object *origin, bool enabled) {
    const char *path=km_string(origin,"path"); char key[80];
    if (km_path(run->km,key,sizeof(key),"agents/%s",id)<0) return -1;
    AdFile *file=ad_native_file(run,"opencode");
    if (!file) return -1;
    if (path && strcmp(file->path,path)) return km_error(run->km,"Native OpenCode subagent origin moved; preserve it and import again");
    if (!enabled && !origin && !km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key)) return 0;
    if (opencode_collision(run,id,true)<0) return -1;
    char *desired=enabled ? km_subagent_render(run->km,resource,id,"opencode") :
                           strdup(km_string(km_get(km_get(resource,"native"),"opencode"),"text"));
    if (!desired) return -1;
    int result=enabled ? 0 : ad_json_edit(run->km,&desired,"disabled","true");
    if (!result) result=ad_change(run,file,key,desired,false);
    free(desired);
    if (!result) json_object_object_add(run->state,"version",json_object_new_int(2));
    return result;
}

static int opencode_collision(AdRun *run, const char *id, bool json_origin) {
    char config[PATH_MAX];
    if (km_path(run->km,config,sizeof(config),"%s/.config/opencode/opencode.json",run->km->home)<0 ||
        ad_subagent_parent_safe(run->km,config)<0) return -1;
    if (json_origin) {
        char path[PATH_MAX]; struct stat st;
        if (default_path(run->km,path,"opencode",id)<0 || ad_subagent_parent_safe(run->km,path)<0) return -1;
        if (lstat(path,&st)==0) return km_error(run->km,"OpenCode subagent has both file and JSON definitions (%s); consolidate them before sync",id);
        return errno==ENOENT ? 0 : km_error(run->km,"Cannot inspect OpenCode subagent discovery");
    }
    AdFile *file=ad_native_file(run,"opencode"); char key[80];
    if (!file || km_path(run->km,key,sizeof(key),"agents/%s",id)<0) return -1;
    char *raw=ad_json_raw(run->km,file->text,key);
    bool collision=raw!=NULL; free(raw);
    if (run->km->error[0]) return -1;
    return collision ? km_error(run->km,"Unowned OpenCode subagent collision in native JSON (%s); import or consolidate it before sync",id) : 0;
}

static int sync_resource(AdRun *run, const char *agent, const char *id, json_object *resource) {
    if (ad_subagent_origins_valid(run->km,resource,id)<0) return -1;
    bool enabled=km_effective(run->km,"subagent",id,agent,NULL,0);
    char reason[160];
    if (!km_subagent_compatible(resource,agent,reason,sizeof(reason))) {
        if (enabled) printf("%s: subagent %s incompatible; provide a native variant before delegation\n",agent,id);
        return 0;
    }
    json_object *origin=km_get(km_get(resource,"origins"),agent);
    const char *format=km_string(km_get(km_get(resource,"native"),agent),"format");
    bool json_target=!strcmp(agent,"opencode") && ((origin && km_string(origin,"key")) || (format && !strcmp(format,"opencode-json")));
    if (json_target) return sync_json(run,resource,id,origin,enabled);
    if (!strcmp(agent,"opencode") && (enabled || origin) && opencode_collision(run,id,false)<0) return -1;
    char path[PATH_MAX];
    if (default_path(run->km,path,agent,id)<0) return -1;
    json_object *record=km_get(km_get(run->state,"subagent_files"),path);
    if (!enabled && !record) return 0;
    char *desired=enabled ? km_subagent_render(run->km,resource,id,agent) : NULL;
    if (enabled && !desired) return -1;
    int result=ad_subagent_change(run,agent,path,id,agent,desired,false);
    free(desired); return result;
}

static int detach_removed_files(AdRun *run, const char *agent) {
    json_object *records=km_get(run->state,"subagent_files"), *resources=km_resources(run->km,"subagent");
    if (!records) return 0;
    json_object_object_foreach(records,path,record) {
        const char *id=km_string(record,"id");
        if (strcmp(agent,km_string(record,"agent")) || km_get(resources,id)) continue;
        if (ad_subagent_change(run,agent,path,id,km_string(record,"format"),NULL,false)<0) return -1;
    }
    return 0;
}

static int detach_removed_json(AdRun *run, const char *agent) {
    if (strcmp(agent,"opencode")) return 0;
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        if (strcmp(agent,km_string(record,"agent"))) continue;
        json_object *entries=km_get(record,"entries");
        json_object_object_foreach(entries,key,entry) {
            (void)entry;
            if (strncmp(key,"agents/",7) || km_get(km_resources(run->km,"subagent"),key+7)) continue;
            AdFile *file=ad_native_file(run,agent);
            if (!file) return -1;
            if (strcmp(path,file->path)) return km_error(run->km,"Native OpenCode subagent origin moved");
            if (ad_change(run,file,key,NULL,false)<0) return -1;
        }
    }
    return 0;
}

static bool manages_subagents(AdRun *run, const char *agent) {
    json_object *resources=km_resources(run->km,"subagent");
    json_object_object_foreach(resources,id,resource) {
        (void)id;
        if (km_subagent_compatible(resource,agent,NULL,0)) return true;
    }
    json_object *records=km_get(run->state,"subagent_files");
    if (records) { json_object_object_foreach(records,path,record) {
        (void)path;
        if (!strcmp(agent,km_string(record,"agent"))) return true;
    } }
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        (void)path;
        if (strcmp(agent,km_string(record,"agent"))) continue;
        json_object *entries=km_get(record,"entries");
        json_object_object_foreach(entries,key,entry) {
            (void)entry;
            if (!strncmp(key,"agents/",7)) return true;
        }
    }
    return false;
}

int ad_sync_subagents(AdRun *run, const char *agent) {
    if (manages_subagents(run,agent) && ad_subagent_flat_discovery(run->km,agent)<0) return -1;
    json_object *resources=km_resources(run->km,"subagent");
    if (resources) {
        json_object_object_foreach(resources,id,resource)
            if (sync_resource(run,agent,id,resource)<0) return -1;
    }
    if (detach_removed_files(run,agent)<0) return -1;
    return detach_removed_json(run,agent);
}
