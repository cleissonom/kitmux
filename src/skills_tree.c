#include "skills_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#define SKILL_MAX_ENTRIES 4096u
#define SKILL_MAX_TOTAL (32u * 1024u * 1024u)

typedef struct {
    Km *km;
    const char *root;
    const char *destination;
    EVP_MD_CTX *digest;
    size_t count;
    size_t bytes;
} Tree;

static int entry_order(const struct dirent **left, const struct dirent **right) {
    return strcmp((*left)->d_name, (*right)->d_name);
}

static bool safe_entry_name(const char *name) {
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '\\') return false;
    return true;
}

static int sync_directory(Km *km, const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return km_error(km, "cannot open installed skill directory for sync: %s", strerror(errno));
    int result = fsync(fd);
    int saved = errno;
    close(fd);
    return result ? km_error(km, "cannot sync installed skill directory: %s", strerror(saved)) : 0;
}

static int sync_ancestry(Km *km, const char *parent) {
    char path[PATH_MAX];
    if (km_path(km, path, sizeof(path), "%s", parent)) return -1;
    for (;;) {
        if (sync_directory(km, path)) return -1;
        if (!strcmp(path, km->home) || !strcmp(path, "/")) return 0;
        char *slash = strrchr(path, '/');
        if (!slash) return km_error(km, "skill store parent is not absolute");
        if (slash == path) path[1] = 0;
        else *slash = 0;
    }
}

static int copy_bytes(Tree *tree, int input, int output, size_t expected) {
    unsigned char buffer[16384];
    size_t total = 0;
    ssize_t got;
    while ((got = read(input, buffer, sizeof(buffer))) != 0) {
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 || (size_t)got > expected - total)
            return km_error(tree->km, "skill file changed while reading");
        total += (size_t)got;
        if (EVP_DigestUpdate(tree->digest, buffer, (size_t)got) != 1)
            return km_error(tree->km, "cannot hash skill package");
        for (ssize_t offset = 0; output >= 0 && offset < got;) {
            ssize_t written = write(output, buffer + offset, (size_t)(got - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return km_error(tree->km, "cannot write skill package: %s", strerror(errno));
            offset += written;
        }
    }
    return total == expected ? 0 : km_error(tree->km, "skill file changed while reading");
}

static int visit_file(Tree *tree, const char *relative, const char *path,
                      const struct stat *before) {
    if (before->st_size < 0 || (unsigned long long)before->st_size > KM_MAX_FILE ||
        (size_t)before->st_size > SKILL_MAX_TOTAL - tree->bytes)
        return km_error(tree->km, "skill package exceeds file or total size limit");
    int input = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat actual;
    if (input < 0) return km_error(tree->km, "cannot read skill file: %s", strerror(errno));
    if (fstat(input, &actual) || !S_ISREG(actual.st_mode) || actual.st_ino != before->st_ino ||
        actual.st_dev != before->st_dev || actual.st_size != before->st_size) {
        close(input);
        return km_error(tree->km, "skill file changed while reading");
    }
    char header[PATH_MAX + 64];
    int count = snprintf(header, sizeof(header), "F:%s:%lld:%u\n", relative,
                         (long long)actual.st_size, (unsigned)!!(actual.st_mode & 0111));
    if (count < 0 || (size_t)count >= sizeof(header) ||
        EVP_DigestUpdate(tree->digest, header, (size_t)count) != 1) {
        close(input);
        return km_error(tree->km, "cannot hash skill package");
    }
    int output = -1;
    char target[PATH_MAX];
    if (tree->destination) {
        if (km_path(tree->km, target, sizeof(target), "%s/%s", tree->destination, relative)) {
            close(input);
            return -1;
        }
        output = open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                      actual.st_mode & 0111 ? 0700 : 0600);
        if (output < 0) {
            close(input);
            return km_error(tree->km, "cannot create skill file: %s", strerror(errno));
        }
    }
    int result = copy_bytes(tree, input, output, (size_t)actual.st_size);
    if (output >= 0 && !result && fsync(output))
        result = km_error(tree->km, "cannot sync installed skill file: %s", strerror(errno));
    if (output >= 0 && close(output) && !result)
        result = km_error(tree->km, "cannot close installed skill file");
    close(input);
    tree->bytes += (size_t)actual.st_size;
    return result;
}

static int walk(Tree *tree, const char *relative, unsigned depth) {
    if (depth > 32 || ++tree->count > SKILL_MAX_ENTRIES)
        return km_error(tree->km, "skill package has too many entries or nested directories");
    char path[PATH_MAX];
    if (km_path(tree->km, path, sizeof(path), "%s%s%s", tree->root,
                 *relative ? "/" : "", relative)) return -1;
    struct stat st;
    if (lstat(path, &st)) return km_error(tree->km, "cannot inspect skill package: %s", strerror(errno));
    if (S_ISLNK(st.st_mode)) return km_error(tree->km, "symlinks are not supported in skill packages");
    if (S_ISREG(st.st_mode)) return visit_file(tree, relative, path, &st);
    if (!S_ISDIR(st.st_mode)) return km_error(tree->km, "special files are not supported in skill packages");
    if (EVP_DigestUpdate(tree->digest, relative, strlen(relative) + 1) != 1)
        return km_error(tree->km, "cannot hash skill directory");
    if (tree->destination && *relative) {
        char target[PATH_MAX];
        if (km_path(tree->km, target, sizeof(target), "%s/%s", tree->destination, relative) ||
            km_mkdirs(tree->km, target)) return -1;
    }
    struct dirent **entries = NULL;
    int count = scandir(path, &entries, NULL, entry_order);
    if (count < 0) return km_error(tree->km, "cannot list skill package: %s", strerror(errno));
    int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name;
        if (!result && !safe_entry_name(name)) result = km_error(tree->km, "skill package has an unsafe filename");
        if (!result && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, ".git")) {
            char child[PATH_MAX];
            if (km_path(tree->km, child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", name))
                result = -1;
            else result = walk(tree, child, depth + 1);
        }
        free(entries[i]);
    }
    free(entries);
    if (!result && tree->destination) {
        char target[PATH_MAX];
        result = km_path(tree->km, target, sizeof(target), "%s%s%s", tree->destination, *relative ? "/" : "", relative);
        if (!result) result = sync_directory(tree->km, target);
    }
    return result;
}

static int hash_tree(Km *km, const char *root, const char *destination, char hash[65]) {
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (!digest) return km_error(km, "cannot allocate skill hash");
    Tree tree = {.km = km, .root = root, .destination = destination, .digest = digest};
    int result = EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1 ? walk(&tree, "", 0) : -1;
    unsigned char bytes[32];
    unsigned count = 0;
    if (!result && (EVP_DigestFinal_ex(digest, bytes, &count) != 1 || count != 32)) result = -1;
    EVP_MD_CTX_free(digest);
    if (result) return *km->error ? -1 : km_error(km, "cannot hash skill package");
    for (unsigned i = 0; i < count; ++i) snprintf(hash + i * 2, 3, "%02x", bytes[i]);
    return 0;
}

static bool skill_name(const char *name) {
    size_t length = strlen(name);
    if (!length || length > 64 || name[0] == '-' || name[length - 1] == '-') return false;
    for (size_t i = 0; i < length; ++i) {
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-')) return false;
        if (i && name[i] == '-' && name[i - 1] == '-') return false;
    }
    return true;
}

static int frontmatter_fields(Km *km, yaml_document_t *document, KmSkill *skill) {
    yaml_node_t *root = yaml_document_get_root_node(document);
    if (!root || root->type != YAML_MAPPING_NODE) return km_error(km, "skill frontmatter must be a YAML mapping");
    for (yaml_node_pair_t *pair = root->data.mapping.pairs.start; pair < root->data.mapping.pairs.top; ++pair) {
        yaml_node_t *key = yaml_document_get_node(document, pair->key);
        yaml_node_t *value = yaml_document_get_node(document, pair->value);
        if (!key || key->type != YAML_SCALAR_NODE) return km_error(km, "skill frontmatter keys must be strings");
        if (memchr(key->data.scalar.value, 0, key->data.scalar.length)) return km_error(km, "skill frontmatter keys cannot contain NUL");
        for (yaml_node_pair_t *earlier = root->data.mapping.pairs.start; earlier < pair; ++earlier) {
            yaml_node_t *other = yaml_document_get_node(document, earlier->key);
            if (other && other->type == YAML_SCALAR_NODE && key->data.scalar.length == other->data.scalar.length &&
                !memcmp(key->data.scalar.value, other->data.scalar.value, key->data.scalar.length))
                return km_error(km, "duplicate key in skill frontmatter");
        }
        bool name = !strcmp((char *)key->data.scalar.value, "name");
        bool description = !strcmp((char *)key->data.scalar.value, "description");
        bool compatibility = !strcmp((char *)key->data.scalar.value, "compatibility");
        bool license = !strcmp((char *)key->data.scalar.value, "license");
        if (!name && !description && !compatibility && !license) continue;
        size_t maximum = name ? sizeof(skill->name) - 1 : description ? sizeof(skill->description) - 1 :
                         compatibility ? sizeof(skill->compatibility) - 1 : sizeof(skill->license) - 1;
        if (!value || value->type != YAML_SCALAR_NODE || !value->data.scalar.length ||
            value->data.scalar.length > maximum || memchr(value->data.scalar.value, 0, value->data.scalar.length))
            return km_error(km, "skill metadata fields must be non-empty scalar strings within standard limits");
        char *target = name ? skill->name : description ? skill->description :
                       compatibility ? skill->compatibility : skill->license;
        memcpy(target, value->data.scalar.value, value->data.scalar.length);
        target[value->data.scalar.length] = 0;
    }
    return skill_name(skill->name) && *skill->description ? 0 :
        km_error(km, "skill needs a valid lowercase name and non-empty description");
}

static int inspect_frontmatter(Km *km, KmSkill *skill) {
    char path[PATH_MAX];
    if (km_path(km, path, sizeof(path), "%s/SKILL.md", skill->root)) return -1;
    size_t size;
    char *text = km_read_file(km, path, &size);
    if (!text) return -1;
    size_t start = size >= 4 && !memcmp(text, "---\n", 4) ? 4 :
                   size >= 5 && !memcmp(text, "---\r\n", 5) ? 5 : 0;
    size_t end = start;
    while (start && end < size && end - start <= 65536) {
        if ((end == start || text[end - 1] == '\n') && end + 3 <= size && !memcmp(text + end, "---", 3) &&
            (end + 3 == size || text[end + 3] == '\n' || (end + 4 < size && !memcmp(text + end + 3, "\r\n", 2)))) break;
        ++end;
    }
    if (!start || end >= size || end - start > 65536) {
        free(text);
        return km_error(km, "SKILL.md needs bounded YAML frontmatter between --- lines");
    }
    yaml_parser_t parser;
    yaml_document_t document;
    if (!yaml_parser_initialize(&parser)) { free(text); return km_error(km, "cannot allocate YAML parser"); }
    yaml_parser_set_input_string(&parser, (unsigned char *)text + start, end - start);
    int loaded = yaml_parser_load(&parser, &document);
    int result = loaded ? frontmatter_fields(km, &document, skill) : km_error(km, "invalid YAML skill frontmatter");
    if (loaded) yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    free(text);
    return result;
}

int km_skill_inspect(Km *km, KmSkill *skill) {
    if (hash_tree(km, skill->root, NULL, skill->hash) || inspect_frontmatter(km, skill)) return -1;
    if (!*skill->revision) snprintf(skill->revision, sizeof(skill->revision), "local-%s", skill->hash);
    return 0;
}

int km_skill_remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path);
    DIR *directory = opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child) || km_skill_remove_tree(child)) result = -1;
    }
    closedir(directory);
    return result ? result : rmdir(path);
}

void km_skill_cleanup(KmSkill *skill) {
    if (*skill->temporary) km_skill_remove_tree(skill->temporary);
    skill->temporary[0] = 0;
}

int km_skill_store(Km *km, KmSkill *skill, char *out, size_t size) {
    char parent[PATH_MAX], staging[PATH_MAX];
    if (km_path(km, parent, sizeof(parent), "%s/skills/%s", km->data, skill->name) ||
        km_mkdirs(km, parent) || km_path(km, out, size, "%s/%s", parent, skill->hash)) return -1;
    struct stat st;
    if (!lstat(out, &st)) {
        char existing_hash[65];
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return km_error(km, "skill store path is not a regular directory");
        if (hash_tree(km, out, NULL, existing_hash)) return -1;
        return strcmp(existing_hash, skill->hash) ? km_error(km, "installed skill package was modified") : sync_ancestry(km, parent);
    }
    if (errno != ENOENT) return km_error(km, "cannot inspect skill store: %s", strerror(errno));
    if (km_path(km, staging, sizeof(staging), "%s/.install-XXXXXX", parent) || !mkdtemp(staging))
        return km_error(km, "cannot stage skill package: %s", strerror(errno));
    char copied_hash[65];
    int result = hash_tree(km, skill->root, staging, copied_hash);
    if (!result && strcmp(copied_hash, skill->hash)) result = km_error(km, "source changed during skill installation");
    if (!result && rename(staging, out)) result = km_error(km, "cannot publish skill package: %s", strerror(errno));
    if (result) km_skill_remove_tree(staging);
    return result ? result : sync_ancestry(km, parent);
}
