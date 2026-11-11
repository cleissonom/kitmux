#include "adapters.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

AdFile *ad_native_file(AdRun *run, const char *agent) {
    if (!strcmp(agent,"codex")) return ad_file(run,agent,".codex/config.toml",true);
    if (!strcmp(agent,"claude")) return ad_file(run,agent,".claude.json",false);
    char a[PATH_MAX],b[PATH_MAX]; struct stat sa,sb;
    if (km_path(run->km,a,sizeof(a),"%s/.config/opencode/opencode.json",run->km->home)<0 ||
        km_path(run->km,b,sizeof(b),"%s/.config/opencode/opencode.jsonc",run->km->home)<0) return NULL;
    bool first=lstat(a,&sa)==0, second=lstat(b,&sb)==0;
    if (first && second) { km_error(run->km,"consolidate OpenCode JSON and JSONC configs before sync"); return NULL; }
    return ad_file(run,agent,second ? ".config/opencode/opencode.jsonc" : ".config/opencode/opencode.json",false);
}

static json_object *clone(json_object *obj) {
    json_object *copy=NULL;
    if (obj) json_object_deep_copy(obj,&copy,NULL);
    return copy ? copy : json_object_new_object();
}

static int executable(Km *km, char *path, size_t size) {
#ifdef __APPLE__
    uint32_t length=(uint32_t)size;
    if (_NSGetExecutablePath(path,&length)!=0) return km_error(km,"cannot resolve Kitmux executable path");
#else
    ssize_t length=readlink("/proc/self/exe",path,size-1);
    if (length<0) return km_error(km,"cannot resolve Kitmux executable path");
    path[length]=0;
#endif
    char *resolved=realpath(path,NULL);
    if (!resolved) return km_error(km,"cannot resolve Kitmux executable path");
    int result=km_path(km,path,size,"%s",resolved); free(resolved); return result;
}

static json_object *command_array(Km *km, json_object *resource, const char *id, const char *agent) {
    const char *mode=km_string(resource,"mode");
    if (!mode || strcmp(mode,"managed")) return json_object_get(km_get(resource,"command"));
    char path[PATH_MAX];
    if (executable(km,path,sizeof(path))<0) return NULL;
    json_object *array=json_object_new_array();
    json_object_array_add(array,json_object_new_string(path));
    if (km->isolated) {
        json_object_array_add(array,json_object_new_string("--home"));
        json_object_array_add(array,json_object_new_string(km->home));
    }
    json_object_array_add(array,json_object_new_string("bridge"));
    json_object_array_add(array,json_object_new_string(id));
    json_object_array_add(array,json_object_new_string("--agent"));
    json_object_array_add(array,json_object_new_string(agent)); return array;
}

static json_object *render_native(Km *km, json_object *resource, json_object *execution,
                                  const char *id, const char *agent, bool enabled) {
    json_object *original=km_get(km_get(resource,"native"),agent), *native=clone(original);
    const char *transport=km_string(execution,"transport"), *url=km_string(execution,"url");
    bool opencode=!strcmp(agent,"opencode"), codex=!strcmp(agent,"codex");
    if (transport && !strcmp(transport,"http")) {
        if (!url) { json_object_put(native); km_error(km,"HTTP MCP is missing its URL"); return NULL; }
        const char *old=km_string(native,"url");
        if (old && strcmp(old,url)) { json_object_put(native); km_error(km,"changed imported endpoint requires explicit re-import"); return NULL; }
        json_object_object_add(native,"url",json_object_new_string(url));
        if (!codex && !km_get(native,"type")) json_object_object_add(native,"type",json_object_new_string(opencode ? "remote" : "http"));
    } else {
        json_object *array=command_array(km,execution,id,agent);
        if (!array || !json_object_is_type(array,json_type_array) || !json_object_array_length(array)) {
            if (array) json_object_put(array);
            json_object_put(native); km_error(km,"stdio MCP has no command"); return NULL;
        }
        if (opencode) {
            json_object_object_add(native,"type",json_object_new_string("local"));
            json_object_object_add(native,"command",array);
        } else {
            json_object_object_add(native,"command",json_object_get(json_object_array_get_idx(array,0)));
            json_object *args=json_object_new_array();
            for (size_t i=1;i<json_object_array_length(array);i++)
                json_object_array_add(args,json_object_get(json_object_array_get_idx(array,i)));
            if (json_object_array_length(args) || km_get(native,"args")) json_object_object_add(native,"args",args);
            else json_object_put(args);
            json_object_put(array);
        }
    }
    json_object *env=km_get(execution,"env");
    json_object_object_del(native,opencode ? "environment" : "env");
    if (env) json_object_object_add(native,opencode ? "environment" : "env",json_object_get(env));
    const char *cwd=km_string(execution,"cwd");
    json_object_object_del(native,"cwd");
    if (cwd) json_object_object_add(native,"cwd",json_object_new_string(cwd));
    bool managed=km_string(execution,"mode") && !strcmp(km_string(execution,"mode"),"managed");
    if (codex && (!original || managed || !enabled || km_get(original,"enabled")))
        json_object_object_add(native,"enabled",json_object_new_boolean(enabled));
    if (opencode && (!original || managed || !enabled || km_get(original,"disabled")))
        json_object_object_add(native,"disabled",json_object_new_boolean(!enabled));
    return native;
}

json_object *ad_native(Km *km, json_object *resource, const char *id, const char *agent, bool enabled) {
    json_object *execution=km_mcp_execution(resource,agent);
    if (!execution) { km_error(km,"cannot resolve MCP execution for %s",agent); return NULL; }
    json_object *native=render_native(km,resource,execution,id,agent,enabled);
    json_object_put(execution); return native;
}

static void native_key(char *key, size_t size, const char *agent, const char *id) {
    snprintf(key,size,"%s%s",!strcmp(agent,"claude") ? "mcpServers/" : !strcmp(agent,"opencode") ? "mcp/servers/" : "",id);
}

static const char *original_raw(AdRun *run, AdFile *file, const char *key) {
    json_object *record=km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
    return km_string(record,"before");
}

static char *render_json(Km *km, json_object *native, const char *base) {
    char *result=strdup(base ? base : "{}");
    if (!result) { km_error(km,"out of memory"); return NULL; }
    json_object *existing_doc=ad_parse(km,result);
    if (!existing_doc) { free(result); return NULL; }
    json_object_object_foreach(existing_doc,old_key,old_value) {
        (void)old_value; json_object *present=NULL;
        if (!json_object_object_get_ex(native,old_key,&present) && ad_json_edit(km,&result,old_key,NULL)<0) {
            json_object_put(existing_doc); free(result); return NULL;
        }
    }
    json_object_put(existing_doc);
    json_object_object_foreach(native,key,value) {
        char *existing=ad_json_raw(km,result,key);
        const char *desired=json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);
        bool same=ad_same(km,existing,desired,false); free(existing);
        if (!same && ad_json_edit(km,&result,key,desired)<0) { free(result); return NULL; }
    }
    return result;
}

static int preserve_explicit_defaults(Km *km, json_object *native, const char *current,
                                      const char *agent, bool enabled) {
    const char *key=!strcmp(agent,"codex") ? "enabled" : !strcmp(agent,"opencode") ? "disabled" : NULL;
    if (!key || !current || km_get(native,key)) return 0;
    json_object *parsed=!strcmp(agent,"codex") ? ad_toml_parse(km,current) : ad_parse(km,current);
    if (!parsed) return -1;
    if (km_get(parsed,key)) json_object_object_add(native,key,json_object_new_boolean(!strcmp(agent,"codex") ? enabled : !enabled));
    json_object_put(parsed); return 0;
}

static int sync_one(AdRun *run, AdFile *file, const char *agent, const char *id, json_object *resource) {
    char key[256],reason[256]; native_key(key,sizeof(key),agent,id);
    bool enabled=km_effective(run->km,"mcp",id,agent,reason,sizeof(reason));
    bool managed=km_string(resource,"mode") && !strcmp(km_string(resource,"mode"),"managed") &&
        km_string(resource,"transport") && !strcmp(km_string(resource,"transport"),"stdio");
    json_object *native=ad_native(run->km,resource,id,agent,enabled || managed);
    if (!native) return -1;
    char *current=file->toml ? ad_toml_raw(run->km,file->text,key) : ad_json_raw(run->km,file->text,key);
    if (preserve_explicit_defaults(run->km,native,current,agent,enabled || managed)<0) {
        free(current); json_object_put(native); return -1;
    }
    const char *original=original_raw(run,file,key);
    const char *base=original ? (current ? current : original) : NULL;
    char *desired=NULL;
    if (strcmp(agent,"claude") || enabled || managed)
        desired=file->toml ? ad_toml_render(run->km,native,id,base) : render_json(run->km,native,base);
    json_object_put(native);
    if (run->km->error[0]) { free(current); free(desired); return -1; }
    if (!ad_same(run->km,current,desired,file->toml))
        printf("%s: %s MCP %s -> %s%s\n",run->apply ? "apply" : "preview",agent,id,
               enabled ? "enabled" : "disabled",managed ? " (bridge remains registered)" : "");
    int result=ad_change(run,file,key,desired,false); free(current); free(desired); return result;
}

static int remove_missing(AdRun *run, AdFile *file, const char *agent) {
    json_object *record=km_get(km_get(run->state,"files"),file->path);
    json_object *entries=km_get(record,"entries");
    if (!entries) return 0;
    json_object_object_foreach(entries,key,entry) {
        (void)entry; const char *id=strrchr(key,'/'); id=id ? id+1 : key;
        if (km_get(km_resources(run->km,"mcp"),id)) continue;
        if (!strcmp(agent,"codex") || !strncmp(key,"mcpServers/",11) || !strncmp(key,"mcp/servers/",12))
            if (ad_change(run,file,key,NULL,false)<0) return -1;
    }
    return 0;
}

int ad_sync_mcp(AdRun *run, const char *agent) {
    AdFile *file=ad_native_file(run,agent);
    if (!file) return -1;
    if (file->toml) { json_object *ids=ad_toml_ids(run->km,file->text); if (!ids) return -1; json_object_put(ids); }
    if (!strcmp(agent,"opencode")) {
        json_object *doc=ad_parse(run->km,file->text), *mcp=km_get(doc,"mcp");
        if (mcp) { json_object_object_foreach(mcp,key,value) {
            (void)value;
            if (strcmp(key,"servers")) { json_object_put(doc); return km_error(run->km,"legacy OpenCode MCP layout needs explicit migration before sync"); }
        } }
        if (doc) json_object_put(doc);
    }
    json_object_object_foreach(km_resources(run->km,"mcp"),id,resource)
        if (sync_one(run,file,agent,id,resource)<0) return -1;
    return remove_missing(run,file,agent);
}

static json_object *canonical(Km *km, const char *agent, json_object *native) {
    if (ad_server_shape(km,agent,native)<0) return NULL;
    json_object *resource=json_object_new_object(), *command=km_get(native,"command");
    const char *url=km_string(native,"url");
    if (url) {
        json_object_object_add(resource,"transport",json_object_new_string("http"));
        json_object_object_add(resource,"url",json_object_new_string(url));
    } else if (command) {
        json_object *array=json_object_new_array();
        if (json_object_is_type(command,json_type_array)) {
            for (size_t i=0;i<json_object_array_length(command);i++)
                json_object_array_add(array,json_object_get(json_object_array_get_idx(command,i)));
        } else if (json_object_is_type(command,json_type_string)) {
            json_object_array_add(array,json_object_get(command));
            json_object *args=km_get(native,"args");
            for (size_t i=0;args && i<json_object_array_length(args);i++)
                json_object_array_add(array,json_object_get(json_object_array_get_idx(args,i)));
        }
        if (!json_object_array_length(array)) { json_object_put(array); goto unsupported; }
        json_object_object_add(resource,"transport",json_object_new_string("stdio"));
        json_object_object_add(resource,"command",array);
    } else goto unsupported;
    json_object *env=km_get(native,!strcmp(agent,"opencode") ? "environment" : "env");
    if (env) json_object_object_add(resource,"env",json_object_get(env));
    const char *cwd=km_string(native,"cwd");
    if (cwd) json_object_object_add(resource,"cwd",json_object_new_string(cwd));
    json_object_object_add(resource,"mode",json_object_new_string("direct"));
    json_object_object_add(resource,"enabled",json_object_new_boolean(true));
    json_object_object_add(resource,"agents",json_object_new_object());
    json_object_object_add(resource,"native",json_object_new_object()); return resource;
unsupported:
    json_object_put(resource); km_error(km,"unsupported native MCP definition for %s",agent); return NULL;
}

static bool compatible(json_object *a, json_object *b) {
    const char *keys[]={"transport","command","url","env","cwd"};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
        json_object *left=km_get(a,keys[i]), *right=km_get(b,keys[i]);
        if (!left && !right) continue;
        if (!left || !right || !json_object_equal(left,right)) return false;
    }
    return true;
}

static int adopt(AdRun *run, AdFile *file, const char *agent, const char *id, const char *raw, json_object *native) {
    if (!km_valid_id(id)) return km_error(run->km,"native MCP identifier is unsupported");
    json_object *existing=km_get(km_resources(run->km,"mcp"),id);
    char key[256]; native_key(key,sizeof(key),agent,id);
    json_object *record=km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
    /* Re-adopting our projection must never turn a bridge into its own upstream. */
    if (existing && record && ad_same(run->km,raw,km_string(record,"after"),file->toml))
        return ad_change(run,file,key,raw,true);
    json_object *resource=canonical(run->km,agent,native);
    if (!resource) return -1;
    if (existing) {
        json_object *variants=km_get(existing,"variants");
        if (!compatible(existing,resource)) {
            json_object *execution=km_mcp_execution(resource,agent);
            if (!execution) { json_object_put(resource); return km_error(run->km,"cannot retain imported MCP execution"); }
            json_object_object_del(execution,"mode");
            if (!variants) { variants=json_object_new_object(); json_object_object_add(existing,"variants",variants); }
            json_object_object_add(variants,agent,execution);
        } else if (variants) json_object_object_del(variants,agent);
    }
    if (existing) { json_object_put(resource); resource=existing; }
    else json_object_object_add(km_resources(run->km,"mcp"),id,resource);
    json_object *natives=km_get(resource,"native");
    if (!natives) { natives=json_object_new_object(); json_object_object_add(resource,"native",natives); }
    json_object_object_add(natives,agent,json_object_get(native));
    bool enabled=!strcmp(agent,"opencode") ? !json_object_get_boolean(km_get(native,"disabled")) :
        !km_get(native,"enabled") || json_object_get_boolean(km_get(native,"enabled"));
    json_object_object_add(km_get(resource,"agents"),agent,json_object_new_string(enabled ? "on" : "off"));
    return ad_change(run,file,key,raw,true);
}

int ad_import(AdRun *run, const char *agent) {
    AdFile *file=ad_native_file(run,agent);
    if (!file) return -1;
    json_object *ids=file->toml ? ad_toml_ids(run->km,file->text) : NULL;
    if (file->toml && !ids) return -1;
    json_object *doc=file->toml ? NULL : ad_parse(run->km,file->text);
    json_object *servers=doc ? (!strcmp(agent,"claude") ? km_get(doc,"mcpServers") : km_get(km_get(doc,"mcp"),"servers")) : NULL;
    if (!file->toml) {
        ids=json_object_new_array();
        if (servers) { json_object_object_foreach(servers,id,value) { (void)value; json_object_array_add(ids,json_object_new_string(id)); } }
    }
    int result=0;
    for (size_t i=0;i<json_object_array_length(ids);i++) {
        const char *id=json_object_get_string(json_object_array_get_idx(ids,i));
        char key[256]; native_key(key,sizeof(key),agent,id);
        char *raw=file->toml ? ad_toml_raw(run->km,file->text,id) : ad_json_raw(run->km,file->text,key);
        json_object *native=raw ? (file->toml ? ad_toml_parse(run->km,raw) : ad_parse(run->km,raw)) : NULL;
        if (!native) { free(raw); result=km_error(run->km,"cannot safely locate native MCP definition"); break; }
        result=adopt(run,file,agent,id,raw,native); free(raw); json_object_put(native);
        if (result<0) break;
        printf("%s: %s MCP %s\n",run->apply ? "import" : "preview",agent,id);
    }
    if (doc) json_object_put(doc);
    json_object_put(ids); return result;
}
