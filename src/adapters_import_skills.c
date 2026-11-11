#include "adapters.h"
#include "adapters_toml_vendor.h"
#include "skills_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *object_at(json_object *parent, const char *key) {
    json_object *value=km_get(parent,key);
    if (!value) { value=json_object_new_object(); json_object_object_add(parent,key,value); }
    return value;
}

static bool path_matches(Km *km, const char *configured, const char *source, const char *id) {
    char expanded[PATH_MAX], candidate[PATH_MAX], canonical[PATH_MAX], resolved[PATH_MAX];
    if (configured[0]=='~' && configured[1]=='/') {
        if (km_path(km,expanded,sizeof(expanded),"%s/%s",km->home,configured+2)<0) return false;
    } else if (km_path(km,expanded,sizeof(expanded),"%s",configured)<0) return false;
    const char *roots[]={source,NULL,NULL,NULL};
    char legacy[PATH_MAX], modern[PATH_MAX];
    if (km_path(km,legacy,sizeof(legacy),"%s/.codex/skills/%s",km->home,id)<0 ||
        km_path(km,modern,sizeof(modern),"%s/.agents/skills/%s",km->home,id)<0) return false;
    roots[1]=legacy; roots[2]=modern;
    bool has_real=realpath(expanded,canonical)!=NULL;
    for (size_t i=0;roots[i];i++) {
        if (!strcmp(expanded,roots[i])) return true;
        if (km_path(km,candidate,sizeof(candidate),"%s/SKILL.md",roots[i])<0) return false;
        if (!strcmp(expanded,candidate)) return true;
        if (has_real && realpath(roots[i],resolved) && !strcmp(canonical,resolved)) return true;
        if (has_real && realpath(candidate,resolved) && !strcmp(canonical,resolved)) return true;
    }
    return false;
}

static int codex_enabled(AdRun *run, const char *id, const char *source, bool *enabled) {
    AdFile *file=ad_native_file(run,"codex");
    if (!file) return -1;
    char error[256], *copy=strdup(file->text);
    if (!copy) return km_error(run->km,"out of memory");
    toml_table_t *root=toml_parse(copy,error,sizeof(error)); free(copy);
    if (!root) return km_error(run->km,"cannot inspect native Codex skill controls");
    toml_table_t *skills=toml_table_in(root,"skills");
    toml_array_t *entries=skills ? toml_array_in(skills,"config") : NULL;
    int result=0;
    if ((toml_raw_in(root,"skills") || toml_array_in(root,"skills")) ||
        (skills && (toml_raw_in(skills,"config") || toml_table_in(skills,"config"))))
        result=km_error(run->km,"unsupported native Codex skills.config layout; explicit migration required");
    for (int i=0;!result && entries && i<toml_array_nelem(entries);i++) {
        toml_table_t *entry=toml_table_at(entries,i);
        if (!entry) { result=km_error(run->km,"unsupported native Codex skill configuration entry"); break; }
        toml_datum_t path=toml_string_in(entry,"path"), name=toml_string_in(entry,"name"), active=toml_bool_in(entry,"enabled");
        if ((!path.ok && !name.ok) || (toml_raw_in(entry,"path") && !path.ok) ||
            (toml_raw_in(entry,"name") && !name.ok) || (toml_raw_in(entry,"enabled") && !active.ok)) {
            if (path.ok) free(path.u.s);
            if (name.ok) free(name.u.s);
            result=km_error(run->km,"unsupported native Codex skill configuration entry"); break;
        }
        bool matches=name.ok && !strcmp(name.u.s,id);
        if (name.ok && strpbrk(name.u.s,"*?["))
            result=km_error(run->km,"pattern-based native Codex skill selectors require explicit migration");
        if (!result && path.ok && path.u.s[0]!='/' && strncmp(path.u.s,"~/",2))
            result=km_error(run->km,"relative native Codex skill paths require explicit migration");
        if (!result && path.ok) matches=matches || path_matches(run->km,path.u.s,source,id);
        if (!result && active.ok && !active.u.b && matches) *enabled=false;
        if (path.ok) free(path.u.s);
        if (name.ok) free(name.u.s);
        if (run->km->error[0]) result=-1;
    }
    toml_free(root);
    return result;
}

static int permission_state(Km *km, json_object *rules, const char *id, bool *enabled, bool *matched) {
    if (!rules) return 0;
    if (!json_object_is_type(rules,json_type_array)) return km_error(km,"OpenCode skill import requires v2 permission arrays");
    for (size_t i=0;i<json_object_array_length(rules);i++) {
        json_object *rule=json_object_array_get_idx(rules,i);
        const char *action=km_string(rule,"action"), *resource=km_string(rule,"resource"), *effect=km_string(rule,"effect");
        if (!action || !resource || !effect) return km_error(km,"malformed native OpenCode skill permission rule");
        if (fnmatch(action,"skill",0) || fnmatch(resource,id,0)) continue;
        if (strcmp(effect,"allow") && strcmp(effect,"deny") && strcmp(effect,"ask"))
            return km_error(km,"unsupported native OpenCode skill permission effect");
        *enabled=strcmp(effect,"deny")!=0;
        *matched=true;
    }
    return 0;
}

int ad_skill_native_enabled(AdRun *run, const char *agent, const char *id, const char *source, bool *enabled) {
    *enabled=true;
    if (!strcmp(agent,"codex")) return codex_enabled(run,id,source,enabled);
    AdFile *file=!strcmp(agent,"claude") ? ad_file(run,agent,".claude/settings.json",false) : ad_native_file(run,agent);
    if (!file) return -1;
    json_object *doc=ad_parse(run->km,file->text);
    if (!doc) return -1;
    int result=0;
    if (!strcmp(agent,"claude")) {
        json_object *value=km_get(km_get(doc,"skillOverrides"),id);
        const char *state=value && json_object_is_type(value,json_type_string) ? json_object_get_string(value) : NULL;
        if (value && (!state || (strcmp(state,"on") && strcmp(state,"off") && strcmp(state,"name-only") && strcmp(state,"user-invocable-only"))))
            result=km_error(run->km,"unsupported native Claude skill visibility; explicit migration required");
        else if (state && !strcmp(state,"off")) *enabled=false;
    } else {
        bool matched=false;
        result=permission_state(run->km,km_get(doc,"permissions"),id,enabled,&matched);
        json_object *agents=km_get(doc,"agents");
        if (!result && agents) {
            if (!json_object_is_type(agents,json_type_object)) result=km_error(run->km,"unsupported OpenCode agent configuration");
            else { json_object_object_foreach(agents,name,agent_config) {
                (void)name; bool state=*enabled, specific=false;
                if (!result) result=permission_state(run->km,km_get(agent_config,"permissions"),id,&state,&specific);
                if (!result && specific && state!=*enabled)
                    result=km_error(run->km,"OpenCode named-agent skill visibility conflicts; explicit migration required");
            } }
        }
    }
    json_object_put(doc);
    return result;
}

static void package_fields(json_object *resource, KmSkill *skill, const char *path) {
    json_object_object_add(resource,"path",json_object_new_string(path));
    json_object_object_add(resource,"source",json_object_new_string(skill->source));
    json_object_object_add(resource,"revision",json_object_new_string(skill->revision));
    json_object_object_add(resource,"hash",json_object_new_string(skill->hash));
    json_object_object_add(resource,"description",json_object_new_string(skill->description));
    json_object_object_add(resource,"compatibility",json_object_new_string(skill->compatibility));
    json_object_object_add(resource,"license",json_object_new_string(skill->license));
    json_object_object_add(resource,"subdir",json_object_new_string(""));
    json_object_object_add(resource,"ref",json_object_new_string(""));
    json_object_object_add(resource,"enabled",json_object_new_boolean(true));
    json_object_object_add(resource,"agents",json_object_new_object());
}

static int add_binding(Km *km, json_object *resource, const char *agent, const char *path,
                       const char *target, bool owned) {
    const char *field=owned ? "discovery" : "origins";
    json_object *all=object_at(resource,field), *bindings=km_get(all,agent);
    if (!bindings) { bindings=json_object_new_array(); json_object_object_add(all,agent,bindings); }
    if (!json_object_is_type(bindings,json_type_array)) return km_error(km,"invalid imported skill bindings");
    for (size_t i=0;i<json_object_array_length(bindings);i++) {
        json_object *entry=json_object_array_get_idx(bindings,i);
        const char *existing=km_string(entry,"path");
        if (!existing) return km_error(km,"invalid imported skill binding");
        if (!strcmp(existing,path)) return 0;
    }
    json_object *entry=json_object_new_object();
    json_object_object_add(entry,"path",json_object_new_string(path));
    json_object_object_add(entry,"target",json_object_new_string(target));
    json_object_array_add(bindings,entry);
    return 0;
}

static int import_package(AdRun *run, const char *agent, const char *name,
                          const char *path, const char *target, bool owned) {
    KmSkill skill={0};
    if (km_skill_acquire(run->km,path,NULL,NULL,&skill)<0 || km_skill_inspect(run->km,&skill)<0) return -1;
    if (strcmp(name,skill.name)) return km_error(run->km,"native skill alias and metadata name differ; explicit migration required");
    json_object *resources=km_resources(run->km,"skill"), *resource=km_get(resources,name);
    if (resource && ad_skill_bindings_valid(run->km,resource,name)<0) return -1;
    const char *hash=km_string(resource,"hash");
    if (resource && (!hash || strcmp(hash,skill.hash))) return km_error(run->km,"skill import conflict: %s has different complete packages; reconcile source variants before import (sources preserved)",name);
    bool enabled=true;
    if (ad_skill_native_enabled(run,agent,name,path,&enabled)<0) return -1;
    if (!resource) {
        char installed[PATH_MAX];
        int result=run->apply ? km_skill_store(run->km,&skill,installed,sizeof(installed)) :
            km_path(run->km,installed,sizeof(installed),"%s/skills/%s/%s",run->km->data,name,skill.hash);
        if (result<0) return -1;
        resource=json_object_new_object();
        package_fields(resource,&skill,installed);
        json_object_object_add(resources,name,resource);
    }
    if (add_binding(run->km,resource,agent,path,target,false)<0 ||
        (owned && (add_binding(run->km,resource,agent,path,target,true)<0 || ad_adopt_skill_link(run,agent,path,target)<0))) return -1;
    json_object *agents=object_at(resource,"agents");
    if (!km_get(agents,agent)) json_object_object_add(agents,agent,json_object_new_string(enabled ? "on" : "off"));
    printf("%s: %s skill %s (%s)\n",run->apply ? "import" : "preview",agent,name,enabled ? "available" : "native off");
    return 0;
}

static int scan_root(AdRun *run, const char *agent, const char *relative, bool owned) {
    char root[PATH_MAX]; struct stat st;
    if (km_path(run->km,root,sizeof(root),"%s/%s",run->km->home,relative)<0) return -1;
    if (lstat(root,&st)<0) return errno==ENOENT ? 0 : km_error(run->km,"cannot inspect native skill discovery root");
    if (!S_ISDIR(st.st_mode)) return km_error(run->km,"symlinked or unsupported skill discovery root needs explicit migration");
    DIR *directory=opendir(root);
    if (!directory) return km_error(run->km,"cannot read native skill discovery root");
    int result=0; size_t count=0; struct dirent *entry;
    while (!result && (entry=readdir(directory))) {
        const char *name=entry->d_name;
        if (name[0]=='.' || (!strcmp(relative,".claude/skills") && !strcmp(name,"synced"))) continue;
        if (++count>4096) { result=km_error(run->km,"native skill discovery exceeds its entry limit"); break; }
        char path[PATH_MAX], metadata[PATH_MAX], target[PATH_MAX];
        if (km_path(run->km,path,sizeof(path),"%s/%s",root,name)<0 ||
            km_path(run->km,metadata,sizeof(metadata),"%s/SKILL.md",path)<0) { result=-1; break; }
        if (lstat(path,&st)<0) { result=km_error(run->km,"native skill changed during discovery"); break; }
        if (!S_ISLNK(st.st_mode) && !S_ISDIR(st.st_mode)) {
            if (strstr(name,".md")) result=km_error(run->km,"flat native skill files require explicit package migration");
            continue;
        }
        if (!km_valid_id(name) || stat(metadata,&st)<0 || !S_ISREG(st.st_mode)) {
            result=km_error(run->km,"nested or incomplete native skill package requires explicit migration"); break;
        }
        if (lstat(path,&st)<0) { result=km_error(run->km,"native skill changed during discovery"); break; }
        if (owned && !S_ISLNK(st.st_mode)) {
            result=km_error(run->km,"existing native skill directory requires explicit migration; source preserved"); break;
        }
        if (S_ISLNK(st.st_mode)) {
            ssize_t size=readlink(path,target,sizeof(target)-1);
            if (size<0 || (size_t)size>=sizeof(target)-1) { result=km_error(run->km,"cannot inspect native skill alias"); break; }
            target[size]=0;
        } else if (km_path(run->km,target,sizeof(target),"%s",path)<0) { result=-1; break; }
        result=import_package(run,agent,name,path,target,owned);
    }
    closedir(directory);
    return result;
}

int ad_import_skills(AdRun *run, const char *agent) {
    if (!strcmp(agent,"codex")) {
        if (scan_root(run,agent,".codex/skills",true)<0) return -1;
        return scan_root(run,agent,".agents/skills",true);
    }
    if (!strcmp(agent,"claude")) return scan_root(run,agent,".claude/skills",true);
    if (scan_root(run,agent,".config/opencode/skills",true)<0 ||
        scan_root(run,agent,".claude/skills",false)<0) return -1;
    return scan_root(run,agent,".agents/skills",false);
}
