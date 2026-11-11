#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static unsigned long long socket_key(const char *id, const char *agent) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const char *p = id; *p; p++) { hash ^= (unsigned char)*p; hash *= UINT64_C(1099511628211); }
    hash ^= '/'; hash *= UINT64_C(1099511628211);
    for (const char *p = agent; *p; p++) { hash ^= (unsigned char)*p; hash *= UINT64_C(1099511628211); }
    return (unsigned long long)hash;
}

static int socket_address(Km *km, struct sockaddr_un *address, const char *path) {
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address->sun_path)) return km_error(km, "Kitmux runtime socket path is too long; choose a shorter XDG_STATE_HOME");
    strcpy(address->sun_path, path);
    return 0;
}

int rt_control_open(Rt *rt) {
    char directory[PATH_MAX];
    struct stat st;
    if (km_path(rt->km, directory, sizeof(directory), "%s/r", rt->km->state) || km_mkdirs(rt->km, directory)) return -1;
    if (lstat(directory, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077))
        return km_error(rt->km, "Kitmux runtime directory must be a private directory owned by this user");
    if (km_path(rt->km, rt->socket_path, sizeof(rt->socket_path), "%s/%016llx.%ld", directory,
                socket_key(rt->id, rt->agent), (long)getpid())) return -1;
    struct sockaddr_un address;
    if (socket_address(rt->km, &address, rt->socket_path)) return -1;
    rt->control = socket(AF_UNIX, SOCK_STREAM, 0);
    if (rt->control < 0) return km_error(rt->km, "Cannot create MCP control socket");
    fcntl(rt->control, F_SETFD, FD_CLOEXEC);
    mode_t previous = umask(077);
    int status = bind(rt->control, (struct sockaddr *)&address, sizeof(address));
    umask(previous);
    if (status) return km_error(rt->km, "Cannot bind MCP control socket");
    rt->control_bound = true;
    if (listen(rt->control, 8) || rt_nonblock(rt->control))
        return km_error(rt->km, "Cannot bind MCP control socket");
    return 0;
}

static bool peer_user(int fd, pid_t *pid) {
    if (pid) *pid = 0;
#ifdef __APPLE__
    uid_t uid; gid_t gid;
    if (getpeereid(fd, &uid, &gid) || uid != getuid()) return false;
#if defined(LOCAL_PEERPID) && defined(SOL_LOCAL)
    if (pid) { socklen_t length = sizeof(*pid); getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, pid, &length); }
#endif
    return true;
#else
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) || credentials.uid != getuid()) return false;
    if (pid) *pid = credentials.pid;
    return true;
#endif
}

static int control_poll(int fd, short events, double deadline) {
    /* Compute once: preemption must never turn an expired deadline into the
     * negative timeout that poll interprets as an unbounded wait. */
    double remaining = deadline - rt_clock();
    if (remaining <= 0) return 0;
    struct pollfd ready = {fd, events, 0};
    return poll(&ready, 1, (int)(remaining * 1000) + 1);
}

static int control_line(int fd, char *bytes, size_t capacity, double deadline) {
    size_t length = 0;
    while (length + 1 < capacity && rt_clock() < deadline) {
        int status = control_poll(fd, POLLIN, deadline);
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0) return -1;
        ssize_t n = read(fd, bytes + length, capacity - length - 1);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return -1;
        if (memchr(bytes + length, '\0', (size_t)n)) return -1;
        length += (size_t)n;
        bytes[length] = '\0';
        char *newline = memchr(bytes, '\n', length);
        if (newline) { *newline = '\0'; return 0; }
    }
    return -1;
}

static json_object *control_reconnect(Rt *rt) {
    bool valid = rt_refresh(rt, true) == 0;
    const char *state = "failed", *message = "MCP catalog is unavailable";
    if (valid && !rt->enabled) { state = "disabled"; message = "MCP is disabled for this agent"; }
    else if (valid && !rt->initialized) { state = "action_required"; message = "Waiting for the client to initialize its bridge"; }
    else if (valid) {
        rt_stop(rt, "MCP reconnect interrupted the request; completion unknown, call not replayed");
        if (!rt_discover(rt) && !rt_changed(rt)) { state = "reconnected"; message = "Upstream discovery succeeded; client tool refresh may still be required"; }
        else message = rt->km->error;
    }
    json_object *reply = json_object_new_object();
    json_object_object_add(reply, "status", json_object_new_string(state));
    json_object_object_add(reply, "message", json_object_new_string(message));
    return reply;
}

void rt_control_accept(Rt *rt) {
    int fd = accept(rt->control, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    rt_nonblock(fd);
    char command[32];
    if (!peer_user(fd, NULL) || control_line(fd, command, sizeof(command), rt_clock() + 1)) {
        close(fd); return;
    }
    json_object *reply = NULL;
    if (!strcmp(command, "status")) reply = rt_connection_status(rt);
    else if (!strcmp(command, "reconnect")) reply = control_reconnect(rt);
    if (!reply) { close(fd); return; }
    json_object_object_add(reply, "control_version", json_object_new_int(1));
    json_object_object_add(reply, "id", json_object_new_string(rt->id));
    json_object_object_add(reply, "agent", json_object_new_string(rt->agent));
    json_object_object_add(reply, "bridge_pid", json_object_new_int64(getpid()));
    rt_send(rt->km, fd, reply);
    json_object_put(reply);
    close(fd);
}

static const char *reload_advice(const char *agent) {
    if (!strcmp(agent, "claude")) return "Use Reconnect in Claude Code's /mcp menu; direct connections belong to the client";
    if (!strcmp(agent, "opencode")) return "Use OpenCode's /mcps menu to disconnect and connect; direct connections belong to the client";
    return "Reload or restart the Codex client; Kitmux cannot reconnect an arbitrary native client connection";
}

static int report(Km *km, const char *id, const char *state, const char *message, int count, bool json) {
    json_object *reply = json_object_new_object();
    json_object_object_add(reply, "status", json_object_new_string(state));
    json_object_object_add(reply, "message", json_object_new_string(message));
    json_object_object_add(reply, "connections", json_object_new_int(count));
    if (json) puts(json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN));
    else printf("%s: %s (%d connections). %s\n", id, state, count, message);
    json_object_put(reply);
    return !strcmp(state, "reconnected") ? 0 : km_error(km, "%s", message);
}

static int control_write(int fd, const char *command, double deadline) {
    char bytes[32];
    size_t left = (size_t)snprintf(bytes, sizeof(bytes), "%s\n", command), offset = 0;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
    int enabled = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    while (left && rt_clock() < deadline) {
        int ready = control_poll(fd, POLLOUT, deadline);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t sent = send(fd, bytes + offset, left, flags);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (sent <= 0) return -1;
        left -= (size_t)sent; offset += (size_t)sent;
    }
    return left ? -1 : 0;
}

static bool valid_number(json_object *reply, const char *key, int64_t minimum, int64_t maximum) {
    json_object *value = km_get(reply, key);
    return json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= minimum && json_object_get_int64(value) <= maximum;
}

static bool valid_reply(json_object *reply, const char *id, const char *agent, const char *command, pid_t peer_pid) {
    const char *reply_id = km_string(reply, "id"), *reply_agent = km_string(reply, "agent");
    if (!json_object_is_type(reply, json_type_object) || !reply_id || strcmp(reply_id, id) ||
        !reply_agent || strcmp(reply_agent, agent) || !valid_number(reply, "control_version", 1, 1) ||
        !valid_number(reply, "bridge_pid", 1, INT_MAX) || (peer_pid > 0 && json_object_get_int64(km_get(reply, "bridge_pid")) != peer_pid)) return false;
    if (!strcmp(command, "reconnect")) return km_string(reply, "status") != NULL;
    const char *state = km_string(reply, "state");
    bool running = state && !strcmp(state, "running");
    bool stopped = state && (!strcmp(state, "waiting") || !strcmp(state, "idle") || !strcmp(state, "disabled"));
    return (running || stopped) && valid_number(reply, "upstream_pid", running ? 1 : 0, running ? INT_MAX : 0) &&
           valid_number(reply, "pending", 0, RT_MAX_PENDING);
}

static json_object *parse_reply(const char *line) {
    json_tokener *parser = json_tokener_new();
    if (!parser) return NULL;
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *reply = json_tokener_parse_ex(parser, line, (int)strlen(line));
    bool valid = json_tokener_get_error(parser) == json_tokener_success;
    size_t used = json_tokener_get_parse_end(parser);
    while (line[used] && isspace((unsigned char)line[used])) used++;
    valid = valid && !line[used] && json_object_is_type(reply, json_type_object);
    json_tokener_free(parser);
    if (!valid) { json_object_put(reply); return NULL; }
    return reply;
}

static json_object *query_socket(Km *km, const char *path, const char *id, const char *agent, const char *command, double deadline, bool *attempted) {
    *attempted = false;
    if (rt_clock() >= deadline) return NULL;
    *attempted = true;
    struct sockaddr_un address;
    if (socket_address(km, &address, path)) return NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    rt_nonblock(fd);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int connected = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (connected && errno == EINPROGRESS) {
        int error = 0; socklen_t length = sizeof(error);
        connected = control_poll(fd, POLLOUT, deadline) > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) && !error ? 0 : -1;
    }
    pid_t peer_pid;
    if (connected || !peer_user(fd, &peer_pid) || control_write(fd, command, deadline)) { close(fd); return NULL; }
    char line[4096];
    int status = control_line(fd, line, sizeof(line), deadline);
    close(fd);
    if (status) return NULL;
    json_object *reply = parse_reply(line);
    if (!valid_reply(reply, id, agent, command, peer_pid)) { json_object_put(reply); return NULL; }
    json_object_object_add(reply, "reachable", json_object_new_boolean(true));
    return reply;
}

json_object *rt_control_connections(Km *km, const char *id, const char *agent, const char *command, int timeout_ms, bool *scan_complete) {
    *scan_complete = true;
    json_object *connections = json_object_new_array();
    if (!connections) return NULL;
    char directory_path[PATH_MAX], path[PATH_MAX], prefix[32];
    if (km_path(km, directory_path, sizeof(directory_path), "%s/r", km->state)) { *scan_complete = false; return connections; }
    int fd = open(directory_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { *scan_complete = errno == ENOENT; return connections; }
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 077)) {
        close(fd); *scan_complete = false; return connections;
    }
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); *scan_complete = false; return connections; }
    snprintf(prefix, sizeof(prefix), "%016llx.", socket_key(id, agent));
    struct dirent *entry;
    size_t candidates = 0;
    double deadline = rt_clock() + timeout_ms / 1000.0;
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, prefix, strlen(prefix))) continue;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) continue;
        if (rt_clock() >= deadline) { *scan_complete = false; break; }
        if (++candidates > 64) { *scan_complete = false; break; }
        json_object *reply = NULL;
        if (!km_path(km, path, sizeof(path), "%s/%s", directory_path, entry->d_name)) {
            bool attempted;
            reply = query_socket(km, path, id, agent, command, deadline, &attempted);
            if (!attempted) { *scan_complete = false; break; }
        }
        if (!reply) {
            reply = json_object_new_object();
            json_object_object_add(reply, "reachable", json_object_new_boolean(false));
            json_object_object_add(reply, "state", json_object_new_string("unreachable"));
        }
        json_object_array_add(connections, reply);
    }
    closedir(directory);
    return connections;
}

int rt_reconnect(Km *km, const char *id, const char *agent, bool json) {
    json_object *server = km_get(km_get(km->catalog, "mcps"), id);
    if (!server) return km_error(km, "Unknown MCP: %s", id);
    const char *mode = km_string(server, "mode");
    if (!mode || strcmp(mode, "managed")) return report(km, id, "action_required", reload_advice(agent), 0, json);
    if (!km_effective(km, "mcp", id, agent, NULL, 0))
        return report(km, id, "disabled", "MCP is disabled for this agent; reconnect does not change availability", 0, json);
    bool complete;
    json_object *connections = rt_control_connections(km, id, agent, "reconnect", 12000, &complete);
    if (!connections) return km_error(km, "Cannot allocate MCP connection observations");
    int count = 0, failed = 0;
    for (size_t i = 0; i < json_object_array_length(connections); i++) {
        json_object *reply = json_object_array_get_idx(connections, i);
        const char *state = km_string(reply, "status");
        if (json_object_get_boolean(km_get(reply, "reachable")) && state && !strcmp(state, "reconnected")) count++; else failed++;
    }
    json_object_put(connections);
    if (failed || !complete) return report(km, id, count ? "partial" : "failed", "At least one managed connection could not reconnect; disabled resources remain stopped", count, json);
    if (!count) return report(km, id, "action_required", "No active managed bridge found for this agent; start or reload its client", 0, json);
    return report(km, id, "reconnected", "Upstream discovery succeeded; client tool refresh may still be required", count, json);
}
