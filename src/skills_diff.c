#include "skills_internal.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int compare_file(Km *km, const char *left, const char *right, const char *relative) {
    size_t left_size, right_size;
    char *before = km_read_file(km, left, &left_size);
    if (!before) return -1;
    char *after = km_read_file(km, right, &right_size);
    if (!after) { free(before); return -1; }
    if (left_size != right_size || memcmp(before, after, left_size)) printf("  changed  %s\n", relative);
    free(before);
    free(after);
    return 0;
}

static int compare_entry(Km *km, const char *old_root, const char *new_root,
                          const char *relative, unsigned depth);

static int compare_directory(Km *km, const char *old_root, const char *new_root,
                              const char *relative, unsigned depth) {
    char old_path[PATH_MAX], new_path[PATH_MAX];
    if (km_path(km, old_path, sizeof(old_path), "%s/%s", old_root, relative) ||
        km_path(km, new_path, sizeof(new_path), "%s/%s", new_root, relative)) return -1;
    struct dirent **entries = NULL;
    int count = scandir(old_path, &entries, NULL, alphasort);
    if (count < 0) return km_error(km, "cannot inspect previous skill package");
    int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name;
        if (!result && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, ".git")) {
            char child[PATH_MAX];
            result = km_path(km, child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", name);
            if (!result) result = compare_entry(km, old_root, new_root, child, depth + 1);
        }
        free(entries[i]);
    }
    free(entries);
    if (result) return result;
    count = scandir(new_path, &entries, NULL, alphasort);
    if (count < 0) return km_error(km, "cannot inspect updated skill package");
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name;
        if (!result && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, ".git")) {
            char child[PATH_MAX], old_child[PATH_MAX];
            result = km_path(km, child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", name);
            if (!result) result = km_path(km, old_child, sizeof(old_child), "%s/%s", old_root, child);
            struct stat st;
            if (!result && lstat(old_child, &st)) {
                if (errno == ENOENT) printf("  added    %s\n", child);
                else result = km_error(km, "cannot inspect previous skill entry");
            }
        }
        free(entries[i]);
    }
    free(entries);
    return result;
}

static int compare_entry(Km *km, const char *old_root, const char *new_root,
                          const char *relative, unsigned depth) {
    if (depth > 32) return km_error(km, "previous skill package exceeds nesting limit");
    char old_path[PATH_MAX], new_path[PATH_MAX];
    struct stat before, after;
    if (km_path(km, old_path, sizeof(old_path), "%s/%s", old_root, relative) ||
        km_path(km, new_path, sizeof(new_path), "%s/%s", new_root, relative)) return -1;
    if (lstat(old_path, &before)) return km_error(km, "cannot inspect previous skill entry");
    if (S_ISLNK(before.st_mode)) return km_error(km, "previous skill package contains a symlink");
    if (lstat(new_path, &after)) {
        if (errno != ENOENT) return km_error(km, "cannot inspect updated skill entry");
        printf("  removed  %s\n", relative);
        return 0;
    }
    if (S_ISLNK(after.st_mode)) return km_error(km, "updated skill package contains a symlink");
    if ((before.st_mode & S_IFMT) != (after.st_mode & S_IFMT)) {
        printf("  changed  %s (file type)\n", relative);
        return 0;
    }
    if (S_ISDIR(before.st_mode)) return compare_directory(km, old_root, new_root, relative, depth);
    if (!S_ISREG(before.st_mode)) return km_error(km, "previous skill package contains a special file");
    if ((before.st_mode & 0111) != (after.st_mode & 0111)) printf("  changed  %s (executable mode)\n", relative);
    return compare_file(km, old_path, new_path, relative);
}

int km_skill_diff(Km *km, const char *old_root, const char *new_root) {
    return compare_entry(km, old_root, new_root, "", 0);
}
