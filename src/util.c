#include "kitmux.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int km_error(Km *km, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(km->error, sizeof(km->error), format, ap);
    va_end(ap);
    return -1;
}

int km_path(Km *km, char *out, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(out, size, format, ap);
    va_end(ap);
    return n < 0 || (size_t)n >= size ? km_error(km, "Path is too long") : 0;
}

static int ensure_directory(Km *km, const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : km_error(km, "Not a real directory: %s", path);
    if (errno != ENOENT || mkdir(path, 0700) < 0)
        return km_error(km, "Cannot create directory %s: %s", path, strerror(errno));
    return 0;
}

int km_mkdirs(Km *km, const char *path) {
    char copy[PATH_MAX];
    if (km_path(km, copy, sizeof(copy), "%s", path) < 0) return -1;
    if (copy[0] != '/') return km_error(km, "Expected absolute directory: %s", path);
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (ensure_directory(km, copy) < 0) return -1;
        *p = '/';
    }
    return ensure_directory(km, copy);
}

char *km_read_file(Km *km, const char *path, size_t *length) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { km_error(km, "Cannot read %s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > KM_MAX_FILE) {
        close(fd); km_error(km, "Expected regular file no larger than 8 MiB: %s", path); return NULL;
    }
    size_t size = (size_t)st.st_size, used = 0;
    char *data = malloc(size + 1);
    if (!data) { close(fd); km_error(km, "Out of memory"); return NULL; }
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); close(fd); km_error(km, "File changed or read failed: %s", path); return NULL; }
        used += (size_t)n;
    }
    data[size] = '\0';
    close(fd);
    if (length) *length = size;
    return data;
}

static int writable_destination(Km *km, const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0)
        return S_ISREG(st.st_mode) ? 0 : km_error(km, "Refusing to replace non-regular file: %s", path);
    return errno == ENOENT ? 0 : km_error(km, "Cannot inspect %s: %s", path, strerror(errno));
}

static int write_all(int fd, const char *data, size_t length) {
    size_t used = 0;
    while (used < length) {
        ssize_t n = write(fd, data + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

int km_write_file(Km *km, const char *path, const void *data, size_t length, unsigned mode) {
    if (length > KM_MAX_FILE) return km_error(km, "File exceeds the 8 MiB write limit; original preserved");
    char parent[PATH_MAX], temp[PATH_MAX];
    if (km_path(km, parent, sizeof(parent), "%s", path) < 0 ||
        km_path(km, temp, sizeof(temp), "%s.tmp.XXXXXX", path) < 0) return -1;
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return km_error(km, "Invalid destination: %s", path);
    *slash = '\0';
    if (km_mkdirs(km, parent) < 0 || writable_destination(km, path) < 0) return -1;
    int fd = mkstemp(temp);
    if (fd < 0) return km_error(km, "Cannot stage %s: %s", path, strerror(errno));
    int result = fchmod(fd, (mode_t)mode) || write_all(fd, data, length) || fsync(fd);
    if (close(fd) < 0) result = -1;
    if (result || writable_destination(km, path) < 0 || rename(temp, path) < 0) {
        int saved = errno; unlink(temp);
        return km_error(km, "Cannot save %s: %s", path, strerror(saved));
    }
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) { result = fsync(dir); close(dir); }
    return result < 0 ? km_error(km, "Saved %s but directory sync failed", path) : 0;
}

json_object *km_get(json_object *obj, const char *key) {
    json_object *value = NULL;
    if (obj && json_object_is_type(obj, json_type_object)) json_object_object_get_ex(obj, key, &value);
    return value;
}

const char *km_string(json_object *obj, const char *key) {
    json_object *value = km_get(obj, key);
    return value && json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

bool km_valid_id(const char *id) {
    if (!id || !*id || strlen(id) > 64) return false;
    for (size_t i = 0; id[i]; ++i) {
        unsigned char c = (unsigned char)id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (i && (c == '-' || c == '_')) continue;
        return false;
    }
    return true;
}
