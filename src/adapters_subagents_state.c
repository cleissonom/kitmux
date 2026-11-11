#include "adapters.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool same(const char *left, const char *right) {
    return (!left || !right) ? left==right : !strcmp(left,right);
}

static int read_current(Km *km, const char *path, char **text) {
    struct stat st; *text=NULL;
    if (ad_subagent_parent_safe(km,path)<0) return -1;
    if (lstat(path,&st)<0) return errno==ENOENT ? 0 : km_error(km,"Cannot inspect native subagent definition");
    if (!S_ISREG(st.st_mode)) return km_error(km,"Native subagent must be a regular file; symlinks are not adopted");
    size_t length;
    *text=km_read_file(km,path,&length);
    if (!*text) return -1;
    if (strlen(*text)!=length) { free(*text); *text=NULL; return km_error(km,"Native subagent contains unsupported NUL bytes"); }
    return 0;
}

static json_object *records(AdRun *run) {
    json_object *map=km_get(run->state,"subagent_files");
    if (!map) {
        map=json_object_new_object();
        json_object_object_add(run->state,"subagent_files",map);
        json_object_object_add(run->state,"version",json_object_new_int(2));
    }
    return map;
}

static void set_optional(json_object *object, const char *key, const char *value) {
    json_object_object_del(object,key);
    if (value) json_object_object_add(object,key,json_object_new_string(value));
}

int ad_subagent_change(AdRun *run, const char *agent, const char *path,
                       const char *id, const char *format, const char *desired, bool adopt) {
    if (!ad_subagent_path_valid(run->km,path,agent,id)) return km_error(run->km,"Unsafe native subagent target");
    char *current=NULL;
    if (read_current(run->km,path,&current)<0) return -1;
    json_object *map=records(run), *record=km_get(map,path);
    if (record) {
        bool pending=json_object_get_boolean(km_get(run->state,"pending"));
        if (!same(current,km_string(record,"after")) && !(pending && same(current,km_string(record,"previous"))) &&
            !(run->refresh && adopt && same(current,desired))) {
            free(current); return km_error(run->km,"Native subagent conflict for %s (%s); preserve your edit before retrying",agent,id);
        }
    } else {
        if (current && !adopt) { free(current); return km_error(run->km,"Unowned subagent collision for %s (%s); import it before adoption",agent,id); }
        record=json_object_new_object();
        json_object_object_add(record,"agent",json_object_new_string(agent));
        json_object_object_add(record,"id",json_object_new_string(id));
        json_object_object_add(record,"format",json_object_new_string(format));
        set_optional(record,"before",current);
        json_object_object_add(map,path,record);
    }
    set_optional(record,"previous",current); set_optional(record,"after",desired);
    json_object *action=json_object_new_object();
    json_object_object_add(action,"path",json_object_new_string(path));
    json_object_object_add(action,"agent",json_object_new_string(agent));
    json_object_object_add(action,"id",json_object_new_string(id));
    set_optional(action,"before",current); set_optional(action,"after",desired);
    json_object_array_add(run->subagent_actions,action);
    free(current); return 0;
}

static int check_action(AdRun *run, json_object *action) {
    char *current=NULL;
    if (read_current(run->km,km_string(action,"path"),&current)<0) return -1;
    bool equal=same(current,km_string(action,"before")); free(current);
    return equal ? 0 : km_error(run->km,"Native subagent changed during sync; retry without overwriting the owner edit");
}

int ad_subagent_check_actions(AdRun *run) {
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++)
        if (check_action(run,json_object_array_get_idx(run->subagent_actions,i))<0) return -1;
    return 0;
}

static int sync_parent(Km *km, const char *path) {
    char parent[PATH_MAX];
    if (km_path(km,parent,sizeof(parent),"%s",path)<0) return -1;
    *strrchr(parent,'/')=0;
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return km_error(km,"Cannot open native subagent directory for sync");
    int result=fsync(fd); close(fd);
    return result<0 ? km_error(km,"Native subagent directory sync failed") : 0;
}

static int apply_action(AdRun *run, json_object *action) {
    const char *path=km_string(action,"path"), *desired=km_string(action,"after");
    const char *before=km_string(action,"before");
    if (check_action(run,action)<0) return -1;
    if (same(before,desired)) return 0;
    if (desired) {
        if (km_write_file(run->km,path,desired,strlen(desired),0600)<0) return -1;
    } else if (before && (unlink(path)<0 || sync_parent(run->km,path)<0))
        return km_error(run->km,"Cannot detach owned native subagent definition");
    printf("%s: subagent %s %s; reload required\n",km_string(action,"agent"),km_string(action,"id"),desired ? "available" : "disabled");
    return 0;
}

int ad_subagent_apply_actions(AdRun *run) {
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++)
        if (apply_action(run,json_object_array_get_idx(run->subagent_actions,i))<0) return -1;
    return 0;
}

void ad_subagent_preview(AdRun *run) {
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++) {
        json_object *action=json_object_array_get_idx(run->subagent_actions,i);
        if (!same(km_string(action,"before"),km_string(action,"after")))
            printf("preview: %s subagent %s %s\n",km_string(action,"agent"),km_string(action,"id"),km_string(action,"after") ? "available" : "disabled");
    }
}

int ad_restore_subagents(AdRun *run, const char *agent) {
    json_object *map=km_get(run->state,"subagent_files");
    if (!map) return 0;
    json_object_object_foreach(map,path,record) {
        const char *owner=km_string(record,"agent");
        if (strcmp(owner,agent)) continue;
        const char *raw=km_string(record,"before"); char *copy=raw ? strdup(raw) : NULL;
        int result=ad_subagent_change(run,owner,path,km_string(record,"id"),km_string(record,"format"),copy,true);
        free(copy); if (result<0) return -1;
    }
    return 0;
}
