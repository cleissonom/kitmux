#include "adapters.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *object_at(json_object *parent, const char *key) {
    json_object *obj=km_get(parent,key);
    if (!obj) { obj=json_object_new_object(); json_object_object_add(parent,key,obj); }
    return obj;
}

int ad_open(AdRun *run, Km *km, bool apply) {
    memset(run,0,sizeof(*run)); run->km=km; run->apply=apply;
    char path[PATH_MAX]; struct stat st;
    if (km_path(km,path,sizeof(path),"%s/adapters.json",km->state)<0) return -1;
    if (lstat(path,&st)==0) {
        char *text=km_read_file(km,path,NULL);
        if (!text) return -1;
        run->state=ad_parse(km,text); free(text);
        if (!run->state) return -1;
    } else if (errno==ENOENT) {
        run->state=json_object_new_object();
        json_object_object_add(run->state,"version",json_object_new_int(1));
        json_object_object_add(run->state,"files",json_object_new_object());
    }
    else return km_error(km,"cannot inspect adapter ownership record");
    if (ad_validate_state(km,run->state)<0) return -1;
    object_at(run->state,"files");
    run->links=json_object_new_array();
    run->subagent_actions=json_object_new_array(); return 0;
}

AdFile *ad_file(AdRun *run, const char *agent, const char *relative, bool toml) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/%s",run->km->home,relative)<0) return NULL;
    for (AdFile *f=run->files;f;f=f->next) if (!strcmp(f->path,path)) return f;
    AdFile *file=calloc(1,sizeof(*file));
    if (!file) { km_error(run->km,"out of memory"); return NULL; }
    snprintf(file->path,sizeof(file->path),"%s",path);
    snprintf(file->agent,sizeof(file->agent),"%s",agent); file->toml=toml;
    struct stat st;
    if (lstat(path,&st)==0) {
        file->existed=true;
        file->original=km_read_file(run->km,path,NULL);
        if (!file->original) { free(file); return NULL; }
    } else if (errno==ENOENT) file->original=strdup(toml ? "" : "{}\n");
    else { km_error(run->km,"cannot inspect native configuration for %s",agent); free(file); return NULL; }
    file->text=strdup(file->original);
    if (!file->text) { free(file->original); free(file); km_error(run->km,"out of memory"); return NULL; }
    if (!toml) {
        json_object *doc=ad_parse(run->km,file->text);
        bool valid=doc && json_object_is_type(doc,json_type_object);
        if (valid && ad_native_shape(run->km,agent,path,doc)<0) valid=false;
        if (doc) json_object_put(doc);
        if (!valid) { free(file->text); free(file->original); free(file); return NULL; }
    }
    file->next=run->files; run->files=file; return file;
}

static const char *record_text(json_object *record, const char *field) {
    return km_string(record,field);
}

static json_object *file_record(AdRun *run, AdFile *file) {
    json_object *record=object_at(object_at(run->state,"files"),file->path);
    json_object_object_add(record,"agent",json_object_new_string(file->agent));
    json_object_object_add(record,"toml",json_object_new_boolean(file->toml));
    return object_at(record,"entries");
}

int ad_change(AdRun *run, AdFile *file, const char *key, const char *desired, bool adopt) {
    Km *km=run->km;
    char *current=file->toml ? ad_toml_raw(km,file->text,key) : ad_json_raw(km,file->text,key);
    if (km->error[0]) { free(current); return -1; }
    json_object *entries=file_record(run,file), *record=km_get(entries,key);
    if (record) {
        bool pending=json_object_get_boolean(km_get(run->state,"pending"));
        if (!ad_same(km,current,record_text(record,"after"),file->toml) &&
            !(pending && ad_same(km,current,record_text(record,"previous"),file->toml))) {
            free(current); return km_error(km,"configuration conflict for %s (%s); preserve your edit before retrying",file->agent,key);
        }
    } else {
        if (current && !adopt) { free(current); return km_error(km,"unowned %s configuration collision (%s); import it before adoption",file->agent,key); }
        record=json_object_new_object();
        if (current) json_object_object_add(record,"before",json_object_new_string(current));
        json_object_object_add(entries,key,record);
    }
    json_object_object_del(record,"previous");
    if (current) json_object_object_add(record,"previous",json_object_new_string(current));
    json_object_object_del(record,"after");
    if (desired) json_object_object_add(record,"after",json_object_new_string(desired));
    int result=0;
    if (!ad_same(km,current,desired,file->toml))
        result=file->toml ? ad_toml_edit(km,&file->text,key,desired) : ad_json_edit(km,&file->text,key,desired);
    free(current); return result;
}

static int write_state(AdRun *run, bool pending) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/adapters.json",run->km->state)<0) return -1;
    json_object_object_add(run->state,"pending",json_object_new_boolean(pending));
    const char *text=json_object_to_json_string_ext(run->state,JSON_C_TO_STRING_PRETTY);
    return km_write_file(run->km,path,text,strlen(text),0600);
}

static int check_file(AdRun *run, AdFile *file) {
    struct stat st; bool exists=lstat(file->path,&st)==0;
    if (!exists && errno!=ENOENT) return km_error(run->km,"cannot recheck native configuration");
    if (exists!=file->existed) return km_error(run->km,"native configuration changed during sync; retry");
    if (!exists) return 0;
    char *now=km_read_file(run->km,file->path,NULL);
    if (!now) return -1;
    bool same=!strcmp(now,file->original); free(now);
    return same ? 0 : km_error(run->km,"native configuration changed during sync; retry");
}

static int sync_link_parent(Km *km, const char *path) {
    char parent[PATH_MAX];
    if (km_path(km,parent,sizeof(parent),"%s",path)<0) return -1;
    char *slash=strrchr(parent,'/');
    if (!slash || slash==parent) return km_error(km,"invalid skill projection parent");
    *slash=0;
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return km_error(km,"cannot open skill projection parent for sync");
    int result=fsync(fd); close(fd);
    return result<0 ? km_error(km,"skill projection directory sync failed") : 0;
}

static int apply_link(AdRun *run, json_object *link) {
    const char *path=km_string(link,"path"), *target=km_string(link,"target");
    const char *before=km_string(link,"before");
    struct stat st; char actual[PATH_MAX];
    bool exists=lstat(path,&st)==0;
    if (exists) {
        ssize_t n=readlink(path,actual,sizeof(actual)-1);
        if (n<0) return km_error(run->km,"skill projection conflict; expected an owned symlink");
        actual[n]=0;
        if (!before || strcmp(actual,before)) return km_error(run->km,"skill projection changed during sync");
    } else if (errno!=ENOENT || before) return km_error(run->km,"skill projection changed during sync");
    if (target && exists && !strcmp(actual,target)) return 0;
    if (!target) {
        if (!exists) return 0;
        if (unlink(path)<0) return km_error(run->km,"cannot detach owned skill");
        return sync_link_parent(run->km,path);
    }
    char parent[PATH_MAX], temporary[PATH_MAX];
    snprintf(parent,sizeof(parent),"%s",path); *strrchr(parent,'/')=0;
    if (km_mkdirs(run->km,parent)<0) return -1;
    if (km_path(run->km,temporary,sizeof(temporary),"%s.kitmux-%ld",path,(long)getpid())<0) return -1;
    if (symlink(target,temporary)<0) return km_error(run->km,"cannot stage skill symlink");
    if (rename(temporary,path)<0) { unlink(temporary); return km_error(run->km,"cannot apply skill symlink"); }
    return sync_link_parent(run->km,path);
}

int ad_finish(AdRun *run) {
    if (ad_subagent_check_actions(run)<0) return -1;
    for (AdFile *f=run->files;f;f=f->next) {
        if (check_file(run,f)<0) return -1;
        json_object *checked=f->toml ? ad_toml_ids(run->km,f->text) : ad_parse(run->km,f->text);
        if (!checked) return -1;
        int valid=f->toml ? 0 : ad_native_shape(run->km,f->agent,f->path,checked);
        json_object_put(checked);
        if (valid<0) return -1;
    }
    if (!run->apply) {
        for (AdFile *f=run->files;f;f=f->next)
            if (strcmp(f->original,f->text)) printf("preview: %s configuration update\n",f->agent);
        printf("preview: %zu skill projection actions; run with --apply\n",json_object_array_length(run->links));
        ad_subagent_preview(run);
        return 0;
    }
    if (write_state(run,true)<0) return -1;
    for (AdFile *f=run->files;f;f=f->next) {
        if (!strcmp(f->original,f->text)) continue;
        if (check_file(run,f)<0 || km_write_file(run->km,f->path,f->text,strlen(f->text),0600)<0) return -1;
        printf("%s: configuration applied; reload required\n",f->agent);
    }
    for (size_t i=0;i<json_object_array_length(run->links);i++)
        if (apply_link(run,json_object_array_get_idx(run->links,i))<0) return -1;
    if (ad_subagent_apply_actions(run)<0) return -1;
    if (run->management) {
        json_object *management=object_at(run->state,"managed_agents");
        json_object_object_foreach(run->management,agent,value)
            json_object_object_add(management,agent,json_object_get(value));
    }
    return write_state(run,false);
}

int ad_restore(AdRun *run, const char *agent) {
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        (void)path; const char *owner=km_string(record,"agent");
        if (agent && strcmp(agent,owner)) continue;
        if (strncmp(path,run->km->home,strlen(run->km->home)) || path[strlen(run->km->home)]!='/')
            return km_error(run->km,"ownership record points outside the selected home");
        const char *relative=path+strlen(run->km->home)+1;
        AdFile *file=ad_file(run,owner,relative,json_object_get_boolean(km_get(record,"toml")));
        if (!file) return -1;
        json_object *entries=km_get(record,"entries");
        json_object_object_foreach(entries,key,entry) {
            const char *raw=km_string(entry,"before"); char *copy=raw ? strdup(raw) : NULL;
            int result=ad_change(run,file,key,copy,true); free(copy);
            if (result<0) return -1;
        }
    }
    return 0;
}

void ad_cleanup(AdRun *run) {
    while (run->files) {
        AdFile *f=run->files; run->files=f->next;
        free(f->original); free(f->text); free(f);
    }
    if (run->state) json_object_put(run->state);
    if (run->links) json_object_put(run->links);
    if (run->management) json_object_put(run->management);
    if (run->subagent_actions) json_object_put(run->subagent_actions);
}
