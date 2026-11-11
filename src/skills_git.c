#include "skills_internal.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static long long milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int spawn_git(Km *km, char *const arguments[], int output, pid_t *pid) {
    char *argv[40] = {"git", "-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false",
                      "-c", "protocol.ext.allow=never", "-c", "protocol.file.allow=always"};
    size_t offset = 9;
    for (size_t i = 0; arguments[i]; ++i) {
        if (offset + 1 >= sizeof(argv) / sizeof(*argv)) return km_error(km, "too many Git arguments");
        argv[offset++] = arguments[i];
    }
    argv[offset] = NULL;
    size_t env_count = 0;
    while (environ[env_count]) ++env_count;
    char **environment = calloc(env_count + 2, sizeof(*environment));
    if (!environment) return km_error(km, "cannot allocate Git environment");
    size_t next = 0;
    for (size_t i = 0; i < env_count; ++i)
        if (strncmp(environ[i], "GIT_TERMINAL_PROMPT=", 20)) environment[next++] = environ[i];
    environment[next] = "GIT_TERMINAL_PROMPT=0";
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    int result = posix_spawnp(pid, "git", &actions, &attributes, argv, environment);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    free(environment);
    return result ? km_error(km, "cannot start Git: %s", strerror(result)) : 0;
}

static char *run_git(Km *km, char *const arguments[], size_t *size) {
    int pipefd[2];
    if (pipe(pipefd)) { km_error(km, "cannot create Git output pipe"); return NULL; }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    pid_t pid;
    if (spawn_git(km, arguments, pipefd[1], &pid)) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    close(pipefd[1]);
    char *buffer = malloc(KM_MAX_FILE + 1);
    size_t used = 0;
    bool failed = !buffer;
    long long deadline = milliseconds() + 60000;
    while (!failed) {
        long long remaining = deadline - milliseconds();
        if (remaining <= 0) { km_error(km, "Git operation timed out"); failed = true; break; }
        struct pollfd input = {.fd = pipefd[0], .events = POLLIN};
        int ready = poll(&input, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { km_error(km, "Git operation timed out or failed"); failed = true; break; }
        char chunk[16384];
        ssize_t got = read(pipefd[0], chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR) continue;
        if (!got) break;
        if (got < 0 || (size_t)got > KM_MAX_FILE - used) {
            km_error(km, "Git output exceeds the skill file limit"); failed = true; break;
        }
        memcpy(buffer + used, chunk, (size_t)got);
        used += (size_t)got;
    }
    close(pipefd[0]);
    if (failed) kill(-pid, SIGKILL);
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) { status = -1; break; }
    if (failed || status == -1 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        free(buffer);
        if (!*km->error) km_error(km, "Git operation failed; verify the source, revision, and authentication");
        return NULL;
    }
    buffer[used] = 0;
    if (size) *size = used;
    return buffer;
}

static bool safe_subdir(const char *path) {
    if (!path || !*path) return true;
    if (*path == '/' || strstr(path, "\\") || strstr(path, "//")) return false;
    const char *start = path;
    for (const char *p = path;; ++p) {
        if (*p && (unsigned char)*p < 32) return false;
        if (*p && *p != '/') continue;
        size_t size = (size_t)(p - start);
        if (!size || (size == 1 && *start == '.') || (size == 2 && !memcmp(start, "..", 2))) return false;
        if (!*p) break;
        start = p + 1;
    }
    return true;
}

static bool safe_ref(const char *ref) {
    if (!ref || !*ref || strlen(ref) > 128 || ref[0] == '-' || strstr(ref, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)ref; *p; ++p)
        if (!isalnum(*p) && !strchr("_-/.", *p)) return false;
    return true;
}

static bool safe_https(const char *source) {
    if (strncmp(source, "https://", 8)) return false;
    const char *host = source + 8, *slash = strchr(host, '/');
    if (!slash || slash == host || memchr(host, '@', (size_t)(slash - host))) return false;
    for (const unsigned char *p = (const unsigned char *)source; *p; ++p)
        if (*p <= 32 || strchr("?#\\", *p)) return false;
    return true;
}

static int set_source(Km *km, const char *source, KmSkill *skill, bool *local) {
    struct stat st;
    *local = !stat(source, &st) && S_ISDIR(st.st_mode);
    if (*local) {
        if (!realpath(source, skill->source)) return km_error(km, "cannot resolve local skill source");
        return 0;
    }
    if (safe_https(source)) return km_path(km, skill->source, sizeof(skill->source), "%s", source);
    if (*source == '.' || *source == '/') return km_error(km, "local skill source does not exist");
    const char *slash = strchr(source, '/');
    if (!slash || slash == source || strchr(slash + 1, '/') || !slash[1])
        return km_error(km, "use an existing local directory, HTTPS Git URL, or owner/repository");
    for (const unsigned char *p = (const unsigned char *)source; *p; ++p)
        if (!isalnum(*p) && !strchr("_-/.", *p)) return km_error(km, "invalid skill repository source");
    if (strstr(source, "..") || source[0] == '-' || slash[1] == '-') return km_error(km, "invalid skill repository source");
    return km_path(km, skill->source, sizeof(skill->source), "https://github.com/%s", source);
}

int km_skill_acquire(Km *km, const char *source, const char *subdir,
                     const char *ref, KmSkill *skill) {
    memset(skill, 0, sizeof(*skill));
    if (!safe_subdir(subdir)) return km_error(km, "skill subdirectory must be a safe relative path");
    if (ref && !safe_ref(ref)) return km_error(km, "invalid Git revision");
    bool local;
    if (set_source(km, source, skill, &local) ||
        km_path(km, skill->subdir, sizeof(skill->subdir), "%s", subdir ? subdir : "") ||
        km_path(km, skill->ref, sizeof(skill->ref), "%s", ref ? ref : "HEAD")) return -1;
    if (!local || ref) return km_skill_git_package(km, skill);
    char selected[PATH_MAX];
    if (km_path(km, selected, sizeof(selected), "%s%s%s", skill->source, *skill->subdir ? "/" : "", skill->subdir)) return -1;
    if (!realpath(selected, skill->root)) return km_error(km, "cannot resolve skill subdirectory");
    size_t source_size = strlen(skill->source);
    if (strncmp(skill->root, skill->source, source_size) ||
        (skill->root[source_size] && skill->root[source_size] != '/'))
        return km_error(km, "skill subdirectory symlink escapes the source");
    size_t root_size = strlen(skill->root);
    if (!strcmp(skill->root, "/") ||
        (!strncmp(km->data, skill->root, root_size) && (km->data[root_size] == '/' || !km->data[root_size])))
        return km_error(km, "skill source cannot contain the Kitmux package store");
    skill->ref[0] = 0;
    return 0;
}

static int write_blob(Km *km, const char *repo, const char *root,
                      const char *relative, const char *oid, bool executable) {
    char *args[] = {"-C", (char *)repo, "cat-file", "blob", (char *)oid, NULL};
    size_t length;
    char *data = run_git(km, args, &length);
    if (!data) return -1;
    char path[PATH_MAX], parent[PATH_MAX];
    int result = km_path(km, path, sizeof(path), "%s/%s", root, relative);
    if (!result) {
        memcpy(parent, path, strlen(path) + 1);
        *strrchr(parent, '/') = 0;
        result = km_mkdirs(km, parent);
    }
    if (!result) result = km_write_file(km, path, data, length, executable ? 0700 : 0600);
    free(data);
    return result;
}

static int extract_entries(Km *km, KmSkill *skill, const char *repo, char *entries, size_t length) {
    size_t used = 0, count = 0, total = 0, prefix = strlen(skill->subdir);
    while (used < length) {
        char *entry = entries + used;
        size_t remaining = length - used;
        char *end = memchr(entry, 0, remaining);
        if (!end || ++count > 4096) return km_error(km, "Git skill has an invalid or excessive file list");
        char *tab = memchr(entry, '\t', (size_t)(end - entry));
        if (!tab) return km_error(km, "invalid Git tree entry");
        *tab = 0;
        char mode[8], type[8], oid[65];
        if (sscanf(entry, "%7s %7s %64s", mode, type, oid) != 3 || strcmp(type, "blob") ||
            (strcmp(mode, "100644") && strcmp(mode, "100755")))
            return km_error(km, "Git skill symlinks, submodules, and special files are unsupported");
        const char *relative = tab + 1;
        if (prefix) {
            if (strncmp(relative, skill->subdir, prefix) || relative[prefix] != '/')
                return km_error(km, "Git skill path does not match the requested subdirectory");
            relative += prefix + 1;
        }
        if (!safe_subdir(relative)) return km_error(km, "unsafe path in Git skill package");
        if (write_blob(km, repo, skill->root, relative, oid, !strcmp(mode, "100755"))) return -1;
        char path[PATH_MAX];
        struct stat st;
        if (km_path(km, path, sizeof(path), "%s/%s", skill->root, relative) || stat(path, &st)) return -1;
        total += (size_t)st.st_size;
        if (total > 32u * 1024u * 1024u) return km_error(km, "Git skill package exceeds the total size limit");
        used += (size_t)(end - entry) + 1;
    }
    return count ? 0 : km_error(km, "Git skill subdirectory is empty or missing");
}

int km_skill_git_package(Km *km, KmSkill *skill) {
    char base[PATH_MAX], repo[PATH_MAX];
    if (km_path(km, base, sizeof(base), "%s/skills", km->data) || km_mkdirs(km, base) ||
        km_path(km, skill->temporary, sizeof(skill->temporary), "%s/.fetch-XXXXXX", base)) return -1;
    if (!mkdtemp(skill->temporary)) { skill->temporary[0] = 0; return km_error(km, "cannot stage Git skill"); }
    if (km_path(km, repo, sizeof(repo), "%s/repository", skill->temporary) ||
        km_path(km, skill->root, sizeof(skill->root), "%s/package", skill->temporary) ||
        km_mkdirs(km, skill->root)) return -1;
    char *init[] = {"init", "--bare", "--quiet", repo, NULL};
    char *output = run_git(km, init, NULL);
    if (!output) return -1;
    free(output);
    char *fetch[] = {"-C", repo, "fetch", "--depth=1", "--no-tags", "--no-recurse-submodules",
                     "--", skill->source, skill->ref, NULL};
    output = run_git(km, fetch, NULL);
    if (!output) return -1;
    free(output);
    char *resolve[] = {"-C", repo, "rev-parse", "--verify", "FETCH_HEAD^{commit}", NULL};
    output = run_git(km, resolve, NULL);
    if (!output) return -1;
    output[strcspn(output, "\r\n")] = 0;
    size_t commit_size = strlen(output);
    bool valid = commit_size == 40 || commit_size == 64;
    for (size_t i = 0; i < commit_size; ++i) if (!isxdigit((unsigned char)output[i])) valid = false;
    int result = valid ? km_path(km, skill->revision, sizeof(skill->revision), "%s", output) :
                        km_error(km, "Git returned an invalid pinned revision");
    free(output);
    if (result) return result;
    char *list[] = {"--literal-pathspecs", "-C", repo, "ls-tree", "-rz", "--full-tree",
                    skill->revision, "--", *skill->subdir ? skill->subdir : ".", NULL};
    size_t length;
    output = run_git(km, list, &length);
    if (!output) return -1;
    result = extract_entries(km, skill, repo, output, length);
    free(output);
    return result;
}
