#include "kitmux.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *scoped_copy(json_object *legacy, const char *kind, const char *name) {
    json_object *group = NULL;
    if (json_object_deep_copy(legacy, &group, NULL) != 0) return NULL;
    json_object_object_add(group, "kind", json_object_new_string(kind));
    json_object_object_add(group, "name", json_object_new_string(name));
    json_object *members = json_object_new_array(), *old = km_get(legacy, "members");
    for (size_t i = 0; i < json_object_array_length(old); ++i) {
        const char *member = json_object_get_string(json_object_array_get_idx(old, i));
        if (!strncmp(member, kind, strlen(kind)) && member[strlen(kind)] == ':')
            json_object_array_add(members, json_object_new_string(member));
    }
    json_object_object_add(group, "members", members);
    return group;
}

static json_object *migration_groups(Km *km) {
    json_object *result = json_object_new_object();
    const char *kinds[] = {"mcp", "skill", "subagent"};
    json_object_object_foreach(km_get(km->catalog, "groups"), name, legacy) {
        bool empty = !json_object_array_length(km_get(legacy, "members"));
        for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
            char key[80];
            json_object *group = scoped_copy(legacy, kinds[i], name);
            if (!group) { json_object_put(result); km_error(km, "Cannot copy group migration"); return NULL; }
            if (!json_object_array_length(km_get(group, "members")) && !(empty && i == 0)) {
                json_object_put(group); continue;
            }
            if (km_path(km, key, sizeof(key), "%s:%s", kinds[i], name) < 0 || km_get(result, key)) {
                json_object_put(group); json_object_put(result); km_error(km, "Group migration collision; original preserved"); return NULL;
            }
            json_object_object_add(result, key, group);
        }
    }
    return result;
}

static int write_backup(Km *km, const char *path, const char *text, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) {
        size_t length = 0; char *old = km_read_file(km, path, &length);
        bool same = old && length == size && !memcmp(old, text, size);
        free(old);
        return same ? 0 : km_error(km, "Existing group backup differs; preserve it before retrying migration");
    }
    if (fd < 0) return km_error(km, "Cannot create private group backup");
    size_t used = 0;
    while (used < size) {
        ssize_t written = write(fd, text + used, size - used);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); unlink(path); return km_error(km, "Cannot write private group backup"); }
        used += (size_t)written;
    }
    int result = fsync(fd);
    if (close(fd) < 0) result = -1;
    return result < 0 ? km_error(km, "Cannot sync private group backup; original preserved") : 0;
}

static int backup_catalog(Km *km) {
    char source[PATH_MAX], directory[PATH_MAX], backup[PATH_MAX];
    long long revision = (long long)json_object_get_int64(km_get(km->catalog, "revision"));
    if (km_path(km, source, sizeof(source), "%s/catalog.json", km->config) < 0 ||
        km_path(km, directory, sizeof(directory), "%s/migrations", km->state) < 0 ||
        km_path(km, backup, sizeof(backup), "%s/groups-v3-r%lld-catalog.json", directory, revision) < 0) return -1;
    size_t size; char *text = km_read_file(km, source, &size);
    if (!text) return -1;
    int result = km_mkdirs(km, directory);
    if (!result) result = write_backup(km, backup, text, size);
    free(text);
    if (!result) {
        int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return km_error(km, "Cannot open group backup directory; original preserved");
        result = fsync(fd);
        close(fd);
        if (result < 0) return km_error(km, "Cannot sync group backup directory; original preserved");
        printf("Original catalog backup: %s\n", backup);
    }
    return result;
}

int km_groups_migrate(Km *km, int argc, char **argv) {
    if (argc != 1 || (strcmp(argv[0], "--preview") && strcmp(argv[0], "--apply")))
        return km_error(km, "Usage: group migrate --preview|--apply");
    if (!km_groups_need_migration(km)) { puts("Groups already scoped; no migration needed."); return 0; }
    json_object *groups = migration_groups(km);
    if (!groups) return -1;
    if (!strcmp(argv[0], "--preview")) {
        puts("Split legacy groups by resource type; assignments and availability are preserved.");
        puts("Empty legacy groups are preserved under MCPs.");
        json_object_object_foreach(groups, key, group)
            printf("  %s: %zu members\n", key, json_object_array_length(km_get(group, "members")));
        json_object_put(groups); return 0;
    }
    if (backup_catalog(km) < 0) { json_object_put(groups); return -1; }
    json_object *old = json_object_get(km_get(km->catalog, "groups"));
    int version = json_object_get_int(km_get(km->catalog, "version"));
    json_object_object_add(km->catalog, "groups", groups);
    json_object_object_add(km->catalog, "version", json_object_new_int(3));
    int result = km_save(km);
    if (result < 0) {
        json_object_object_add(km->catalog, "groups", old);
        json_object_object_add(km->catalog, "version", json_object_new_int(version));
    } else json_object_put(old);
    if (!result) puts("Migrated groups by resource type; original catalog backed up privately.");
    return result;
}
