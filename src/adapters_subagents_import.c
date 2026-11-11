#include "adapters.h"
#include "subagents.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static json_object *object_at(json_object *parent, const char *key) {
    json_object *value=km_get(parent,key);
    if (!value) { value=json_object_new_object(); json_object_object_add(parent,key,value); }
    return value;
}

static int set_origin(Km *km, json_object *resource, const char *agent, const char *id,
                      const char *path, const char *key) {
    json_object *origins=object_at(resource,"origins"), *old=km_get(origins,agent);
    if (old && (strcmp(km_string(old,"path"),path) ||
        (!!km_string(old,"key")!=!!key) || (key && strcmp(km_string(old,"key"),key))))
        return km_error(km,"Multiple native definitions for %s (%s); consolidate their sources before import",agent,id);
    json_object *origin=json_object_new_object();
    json_object_object_add(origin,"path",json_object_new_string(path));
    if (key) json_object_object_add(origin,"key",json_object_new_string(key));
    json_object_object_add(origins,agent,origin); return 0;
}

static bool owned(AdRun *run, const char *path, const char *key) {
    if (!key) return km_get(km_get(run->state,"subagent_files"),path)!=NULL;
    return km_get(km_get(km_get(km_get(run->state,"files"),path),"entries"),key)!=NULL;
}

static json_object *merge_definition(AdRun *run, const char *agent, const char *id,
                                     const char *text, const char *format, const char *path, const char *key) {
    json_object *parsed=km_subagent_parse(run->km,text,format,id);
    if (!parsed) return NULL;
    json_object *resources=km_resources(run->km,"subagent"), *resource=km_get(resources,id);
    bool managed=owned(run,path,key);
    if (!resource) { resource=parsed; json_object_object_add(resources,id,resource); }
    else {
        json_object *variant=km_get(km_get(parsed,"native"),agent), *existing=km_get(km_get(resource,"native"),agent);
        if (!managed && existing && strcmp(km_string(existing,"text"),text)) {
            json_object_put(parsed); km_error(run->km,"Subagent import conflicts with an existing %s variant (%s)",agent,id); return NULL;
        }
        if (!managed || !existing) {
            json_object_object_add(variant,"description",json_object_new_string(km_string(resource,"description")));
            json_object_object_add(variant,"instructions",json_object_new_string(km_string(resource,"instructions")));
            json_object_object_add(object_at(resource,"native"),agent,json_object_get(variant));
        }
        json_object_put(parsed);
    }
    if (set_origin(run->km,resource,agent,id,path,key)<0) return NULL;
    json_object *assignments=object_at(resource,"agents");
    if (!managed || !km_get(assignments,agent)) {
        bool disabled=json_object_get_boolean(km_get(km_get(km_get(resource,"native"),agent),"disabled"));
        json_object_object_add(assignments,agent,json_object_new_string(disabled ? "off" : "on"));
    }
    return resource;
}

static int import_file(AdRun *run, const char *agent, const char *id, const char *path) {
    size_t length; char *text=km_read_file(run->km,path,&length);
    if (!text) return -1;
    if (strlen(text)!=length) { free(text); return km_error(run->km,"Native subagent contains unsupported NUL bytes"); }
    if (!strcmp(agent,"opencode")) {
        char *mode=km_subagent_markdown_mode(run->km,text);
        if (!mode) { free(text); return -1; }
        bool subagent=!strcmp(mode,"subagent"); free(mode);
        if (!subagent) {
            printf("import: opencode primary/all agent skipped (subagents only)\n");
            free(text); return 0;
        }
    }
    json_object *resource=merge_definition(run,agent,id,text,agent,path,NULL);
    int result=resource ? ad_subagent_change(run,agent,path,id,agent,text,true) : -1;
    free(text);
    if (!result) printf("import: %s subagent %s (%s)\n",agent,id,km_effective(run->km,"subagent",id,agent,NULL,0) ? "available" : "disabled");
    return result;
}

static int scan_directory(AdRun *run, const char *agent) {
    const char *relative=!strcmp(agent,"codex") ? ".codex/agents" : !strcmp(agent,"claude") ? ".claude/agents" : ".config/opencode/agents";
    const char *suffix=!strcmp(agent,"codex") ? ".toml" : ".md";
    char root[PATH_MAX], sample[PATH_MAX]; struct stat st;
    if (km_path(run->km,root,sizeof(root),"%s/%s",run->km->home,relative)<0 ||
        km_path(run->km,sample,sizeof(sample),"%s/validation",root)<0 || ad_subagent_parent_safe(run->km,sample)<0) return -1;
    if (lstat(root,&st)<0) return errno==ENOENT ? 0 : km_error(run->km,"Cannot inspect native subagent directory");
    if (!S_ISDIR(st.st_mode)) return km_error(run->km,"Native subagent discovery must be a real directory");
    DIR *dir=opendir(root);
    if (!dir) return km_error(run->km,"Cannot read native subagent discovery directory");
    int result=0; struct dirent *entry;
    while ((entry=readdir(dir))) {
        size_t length=strlen(entry->d_name), extension=strlen(suffix);
        if (length<=extension || strcmp(entry->d_name+length-extension,suffix)) continue;
        char id[65], path[PATH_MAX];
        if (length-extension>=sizeof(id)) { result=km_error(run->km,"Native subagent filename needs an ID of at most 64 characters"); break; }
        memcpy(id,entry->d_name,length-extension); id[length-extension]=0;
        if (!km_valid_id(id)) { result=km_error(run->km,"Native subagent filename needs a valid Kitmux ID"); break; }
        if (km_path(run->km,path,sizeof(path),"%s/%s",root,entry->d_name)<0 || (result=import_file(run,agent,id,path))<0) break;
    }
    closedir(dir); return result;
}

static int import_config(AdRun *run) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/.config/opencode/opencode.json",run->km->home)<0 ||
        ad_subagent_parent_safe(run->km,path)<0) return -1;
    AdFile *file=ad_native_file(run,"opencode");
    if (!file) return -1;
    json_object *doc=ad_parse(run->km,file->text), *agents=km_get(doc,"agents");
    if (!doc) return -1;
    if (agents && !json_object_is_type(agents,json_type_object)) { json_object_put(doc); return km_error(run->km,"OpenCode agents must be an object"); }
    int result=0;
    if (agents) { json_object_object_foreach(agents,id,definition) {
        const char *mode=km_string(definition,"mode");
        if (!mode || strcmp(mode,"subagent")) { printf("import: opencode primary/all agent skipped (subagents only)\n"); continue; }
        char key[80];
        if (!km_valid_id(id) || !json_object_is_type(definition,json_type_object)) { result=km_error(run->km,"Unsupported native OpenCode subagent definition"); break; }
        if (km_path(run->km,key,sizeof(key),"agents/%s",id)<0) { result=-1; break; }
        char *raw=ad_json_raw(run->km,file->text,key);
        if (!raw) { result=-1; break; }
        json_object *resource=merge_definition(run,"opencode",id,raw,"opencode-json",file->path,key);
        result=resource ? ad_change(run,file,key,raw,true) : -1; free(raw);
        if (result<0) break;
        json_object_object_add(run->state,"version",json_object_new_int(2));
        printf("import: opencode subagent %s (%s)\n",id,km_effective(run->km,"subagent",id,"opencode",NULL,0) ? "available" : "disabled");
    }
    }
    json_object_put(doc); return result;
}

int ad_import_subagents(AdRun *run, const char *agent) {
    if (ad_subagent_flat_discovery(run->km,agent)<0) return -1;
    if (scan_directory(run,agent)<0) return -1;
    return !strcmp(agent,"opencode") ? import_config(run) : 0;
}
