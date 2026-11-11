#include "adapters.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *links(AdRun *run) {
    json_object *value=km_get(run->state,"links");
    if (!value) { value=json_object_new_object(); json_object_object_add(run->state,"links",value); }
    return value;
}

static int real_parents(Km *km, const char *path) {
    char copy[PATH_MAX]; snprintf(copy,sizeof(copy),"%s",path);
    for (char *p=copy+1;*p;p++) {
        if (*p!='/') continue;
        *p=0; struct stat st;
        if (lstat(copy,&st)==0 && !S_ISDIR(st.st_mode)) return km_error(km,"shared or symlinked skill directory prevents agent isolation; migrate it first");
        if (errno!=ENOENT && lstat(copy,&st)<0) return km_error(km,"cannot inspect skill discovery directory");
        *p='/';
    }
    return 0;
}

static bool projection_path(Km *km, const char *agent, const char *path, const char *id) {
    const char *roots[3]={NULL,NULL,NULL}; char expected[PATH_MAX];
    if (!strcmp(agent,"codex")) { roots[0]=".agents/skills"; roots[1]=".codex/skills"; }
    else roots[0]=!strcmp(agent,"claude") ? ".claude/skills" : ".config/opencode/skills";
    if (!km_valid_agent(agent) || !km_valid_id(id)) return false;
    for (size_t i=0;roots[i];i++) {
        if (km_path(km,expected,sizeof(expected),"%s/%s/%s",km->home,roots[i],id)<0) return false;
        if (!strcmp(expected,path)) return true;
    }
    return false;
}

static int link_change(AdRun *run, const char *agent, const char *path, const char *target, bool adopt) {
    const char *id=strrchr(path,'/');
    if (!id || !projection_path(run->km,agent,path,id+1)) return km_error(run->km,"unsafe skill projection path");
    if (real_parents(run->km,path)<0) return -1;
    json_object *record=km_get(links(run),path);
    struct stat st; char actual[PATH_MAX]; const char *current=NULL;
    if (lstat(path,&st)==0) {
        ssize_t n=readlink(path,actual,sizeof(actual)-1);
        if (n<0 || (size_t)n>=sizeof(actual)-1) return km_error(run->km,"unowned skill projection collision; preserve the existing skill");
        actual[n]=0; current=actual;
    } else if (errno!=ENOENT) return km_error(run->km,"cannot inspect skill projection");
    if (record) {
        bool pending=json_object_get_boolean(km_get(run->state,"pending"));
        const char *after=km_string(record,"after"), *previous=km_string(record,"previous");
        bool expected=(!current && !after) || (current && after && !strcmp(current,after));
        bool prior=(!current && !previous) || (current && previous && !strcmp(current,previous));
        if (!expected && !(pending && prior)) return km_error(run->km,"owned skill link changed outside Kitmux; preserve that edit first");
    } else {
        if (current && !adopt) return km_error(run->km,"unowned skill symlink collision; migrate it before adoption");
        record=json_object_new_object();
        json_object_object_add(record,"agent",json_object_new_string(agent));
        if (current) json_object_object_add(record,"before",json_object_new_string(current));
        json_object_object_add(links(run),path,record);
    }
    json_object_object_del(record,"previous"); json_object_object_del(record,"after");
    if (current) json_object_object_add(record,"previous",json_object_new_string(current));
    if (target) json_object_object_add(record,"after",json_object_new_string(target));
    if ((!current && !target) || (current && target && !strcmp(current,target))) return 0;
    json_object *action=json_object_new_object();
    json_object_object_add(action,"path",json_object_new_string(path));
    if (current) json_object_object_add(action,"before",json_object_new_string(current));
    if (target) json_object_object_add(action,"target",json_object_new_string(target));
    json_object_array_add(run->links,action); return 0;
}

int ad_adopt_skill_link(AdRun *run, const char *agent, const char *path, const char *target) {
    char actual[PATH_MAX];
    ssize_t size=readlink(path,actual,sizeof(actual)-1);
    if (size<0 || (size_t)size>=sizeof(actual)-1) return km_error(run->km,"native skill alias changed during import");
    actual[size]=0;
    if (strcmp(actual,target)) return km_error(run->km,"native skill alias changed during import");
    return link_change(run,agent,path,target,true);
}

static const char *skill_root(const char *agent) {
    return !strcmp(agent,"codex") ? ".agents/skills" : !strcmp(agent,"claude") ? ".claude/skills" : ".config/opencode/skills";
}

static bool binding_text(json_object *entry, const char *key) {
    json_object *value=km_get(entry,key);
    const char *text=km_string(entry,key);
    return text && *text && strlen(text)==(size_t)json_object_get_string_len(value);
}

static bool origin_path(Km *km, const char *agent, const char *path, const char *id) {
    if (projection_path(km,agent,path,id)) return true;
    if (strcmp(agent,"opencode")) return false;
    if (projection_path(km,"claude",path,id)) return true;
    char shared[PATH_MAX];
    return km_path(km,shared,sizeof(shared),"%s/.agents/skills/%s",km->home,id)==0 && !strcmp(shared,path);
}

int ad_skill_bindings_valid(Km *km, json_object *skill, const char *id) {
    const char *fields[]={"discovery","origins"};
    for (size_t f=0;f<2;f++) {
        json_object *all=km_get(skill,fields[f]);
        if (!all) continue;
        if (!json_object_is_type(all,json_type_object)) return km_error(km,"invalid imported skill bindings");
        json_object_object_foreach(all,agent,bindings) {
            if (!km_valid_agent(agent) || !json_object_is_type(bindings,json_type_array) || json_object_array_length(bindings)>4)
                return km_error(km,"invalid imported skill binding client or count");
            for (size_t i=0;i<json_object_array_length(bindings);i++) {
                json_object *entry=json_object_array_get_idx(bindings,i);
                const char *path=km_string(entry,"path");
                if (!binding_text(entry,"path") || !binding_text(entry,"target") ||
                    !(f ? origin_path(km,agent,path,id) : projection_path(km,agent,path,id)))
                    return km_error(km,"unsafe imported skill discovery binding");
                for (size_t prior=0;prior<i;prior++) {
                    const char *previous=km_string(json_object_array_get_idx(bindings,prior),"path");
                    if (!strcmp(previous,path)) return km_error(km,"duplicate imported skill discovery binding");
                }
            }
        }
    }
    return 0;
}

static int project_path(AdRun *run, const char *agent, const char *id, json_object *skill, const char *path) {
    char reason[256];
    if (!projection_path(run->km,agent,path,id)) return km_error(run->km,"unsafe imported skill discovery binding");
    bool enabled=km_effective(run->km,"skill",id,agent,reason,sizeof(reason));
    const char *target=enabled ? km_string(skill,"path") : NULL;
    if (enabled) {
        struct stat st;
        if (!target || stat(target,&st)<0 || !S_ISDIR(st.st_mode)) return km_error(run->km,"selected skill package is missing: %s",id);
        if (!strcmp(agent,"codex")) {
            bool native=true;
            if (ad_skill_native_enabled(run,agent,id,path,&native)<0) return -1;
            if (!native) return km_error(run->km,"native Codex skills.config still disables %s; change that native override before enabling and retry sync --apply",id);
        }
    }
    return link_change(run,agent,path,target,false);
}

static int project(AdRun *run, const char *agent, const char *id, json_object *skill) {
    if (ad_skill_bindings_valid(run->km,skill,id)<0) return -1;
    json_object *bindings=km_get(km_get(skill,"discovery"),agent);
    if (bindings) {
        if (!json_object_is_type(bindings,json_type_array)) return km_error(run->km,"invalid imported skill discovery bindings");
        if (json_object_array_length(bindings)) {
            for (size_t i=0;i<json_object_array_length(bindings);i++) {
                const char *path=km_string(json_object_array_get_idx(bindings,i),"path");
                if (!path || project_path(run,agent,id,skill,path)<0) return -1;
            }
            return 0;
        }
    }
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/%s/%s",run->km->home,skill_root(agent),id)<0) return -1;
    return project_path(run,agent,id,skill,path);
}

static int claude_overrides(AdRun *run) {
    AdFile *file=ad_file(run,"claude",".claude/settings.json",false);
    if (!file) return -1;
    json_object_object_foreach(km_resources(run->km,"skill"),id,skill) {
        (void)skill; char key[256],reason[256]; snprintf(key,sizeof(key),"skillOverrides/%s",id);
        bool enabled=km_effective(run->km,"skill",id,"claude",reason,sizeof(reason));
        json_object *record=km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
        char *current=ad_json_raw(run->km,file->text,key);
        const char *before=record ? km_string(record,"before") : current;
        json_object *original=before ? ad_parse(run->km,before) : NULL;
        bool was_off=original && json_object_is_type(original,json_type_string) && !strcmp(json_object_get_string(original),"off");
        const char *desired=enabled ? (was_off ? NULL : before) : "\"off\"";
        int result=ad_change(run,file,key,desired,true);
        json_object_put(original); free(current);
        if (result<0) return -1;
    }
    json_object *record=km_get(km_get(run->state,"files"),file->path), *entries=km_get(record,"entries");
    if (entries) {
        json_object_object_foreach(entries,key,entry) {
            const char *id=strrchr(key,'/');
            if (id && !km_get(km_resources(run->km,"skill"),id+1)) {
                const char *before=km_string(entry,"before"); char *copy=before ? strdup(before) : NULL;
                int result=ad_change(run,file,key,copy,true); free(copy); if (result<0) return -1;
            }
        }
    }
    return 0;
}

static int opencode_permissions(AdRun *run) {
    AdFile *file=ad_native_file(run,"opencode");
    if (!file) return -1;
    json_object *record=km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),"permissions");
    char *current=ad_json_raw(run->km,file->text,"permissions");
    const char *original=record ? km_string(record,"before") : current;
    char *desired=strdup(original ? original : "[]");
    json_object *array=ad_parse(run->km,desired);
    if (!array || !json_object_is_type(array,json_type_array)) {
        if (array) json_object_put(array);
        free(current); free(desired); return km_error(run->km,"OpenCode permissions must use the v2 array schema");
    }
    bool added=false;
    json_object *document=ad_parse(run->km,file->text), *agents=km_get(document,"agents");
    bool agent_skill_allow=false;
    if (agents && json_object_is_type(agents,json_type_object)) {
        json_object_object_foreach(agents,name,agent) {
            (void)name; json_object *permissions=km_get(agent,"permissions");
            if (!permissions) continue;
            if (!json_object_is_type(permissions,json_type_array)) { agent_skill_allow=true; break; }
            for (size_t i=0;i<json_object_array_length(permissions);i++) {
                json_object *rule=json_object_array_get_idx(permissions,i);
                const char *action=km_string(rule,"action"), *effect=km_string(rule,"effect");
                if (action && (!strcmp(action,"skill") || !strcmp(action,"*")) && (!effect || strcmp(effect,"deny"))) agent_skill_allow=true;
            }
        }
    }
    if (document) json_object_put(document);
    json_object_object_foreach(km_resources(run->km,"skill"),id,skill) {
        (void)skill; char reason[256];
        if (km_effective(run->km,"skill",id,"opencode",reason,sizeof(reason))) continue;
        if (agent_skill_allow) {
            json_object_put(array); free(current); free(desired);
            return km_error(run->km,"OpenCode agent-level skill permissions conflict with isolation; reconcile them before sync");
        }
        json_object *rule=json_object_new_object();
        json_object_object_add(rule,"action",json_object_new_string("skill"));
        json_object_object_add(rule,"resource",json_object_new_string(id));
        json_object_object_add(rule,"effect",json_object_new_string("deny"));
        int result=ad_array_append(run->km,&desired,json_object_to_json_string_ext(rule,JSON_C_TO_STRING_PLAIN));
        json_object_put(rule);
        if (result<0) { json_object_put(array); free(current); free(desired); return -1; }
        added=true;
    }
    json_object_put(array);
    int result=0;
    if (record || added) result=ad_change(run,file,"permissions",(!original && !added) ? NULL : desired,true);
    free(current); free(desired); return result;
}

int ad_sync_skills(AdRun *run, const char *agent) {
    json_object_object_foreach(km_resources(run->km,"skill"),id,skill)
        if (project(run,agent,id,skill)<0) return -1;
    json_object *records=links(run);
    json_object_object_foreach(records,path,record) {
        if (strcmp(km_string(record,"agent"),agent)) continue;
        const char *id=strrchr(path,'/');
        if (id && !km_get(km_resources(run->km,"skill"),id+1) && link_change(run,agent,path,NULL,false)<0) return -1;
    }
    if (!strcmp(agent,"claude")) return claude_overrides(run);
    if (!strcmp(agent,"opencode")) return opencode_permissions(run);
    return 0;
}

int ad_restore_links(AdRun *run, const char *agent) {
    json_object *records=links(run);
    json_object_object_foreach(records,path,record) {
        const char *owner=km_string(record,"agent");
        if (agent && strcmp(agent,owner)) continue;
        if (strncmp(path,run->km->home,strlen(run->km->home)) || path[strlen(run->km->home)]!='/')
            return km_error(run->km,"ownership record points outside selected home");
        const char *before=km_string(record,"before");
        if (link_change(run,owner,path,before,false)<0) return -1;
    }
    return 0;
}
