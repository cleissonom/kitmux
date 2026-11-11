#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static json_object *definition(Rt *rt) {
    return rt->execution;
}

static int spawn_child(Rt *rt) {
    json_object *server = definition(rt), *command = km_get(server, "command");
    size_t count = json_object_array_length(command);
    if (!count || count > 256) return km_error(rt->km, "Managed MCP requires 1–256 command arguments");
    char *arguments[257];
    for (size_t i = 0; i < count; i++) {
        json_object *value = json_object_array_get_idx(command, i);
        if (!json_object_is_type(value, json_type_string)) return km_error(rt->km, "Invalid MCP command argument");
        arguments[i] = (char *)json_object_get_string(value);
    }
    arguments[count] = NULL;
    int input[2], output[2];
    if (pipe(input)) return km_error(rt->km, "Cannot create MCP input pipe");
    if (pipe(output)) { close(input[0]); close(input[1]); return km_error(rt->km, "Cannot create MCP output pipe"); }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd < 0 || dup2(nullfd, STDERR_FILENO) < 0) _exit(126);
        close(nullfd); close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        const char *cwd = km_string(server, "cwd");
        if (cwd && chdir(cwd)) _exit(126);
        json_object *environment = km_get(server, "env");
        if (environment) {
            json_object_object_foreach(environment, name, value) {
                if (!json_object_is_type(value, json_type_string) || setenv(name, json_object_get_string(value), 1)) _exit(126);
            }
        }
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    if (pid < 0) { close(input[1]); close(output[0]); return km_error(rt->km, "Cannot start MCP process"); }
    setpgid(pid, pid);
    rt->child = pid; rt->upstream_in = input[1]; rt->upstream_out = output[0];
    fcntl(input[1], F_SETFD, FD_CLOEXEC); fcntl(output[0], F_SETFD, FD_CLOEXEC);
    rt_nonblock(input[1]); rt_nonblock(output[0]);
    return 0;
}

static void stop_child(Rt *rt) {
    if (rt->child <= 0) return;
    /* Keep the leader unreaped while signalling its group. Its zombie reserves
     * the PID, so an exited leader cannot make us target a reused process ID. */
    siginfo_t observed = {0};
    int waiting;
    do { waiting = waitid(P_PID, (id_t)rt->child, &observed, WEXITED | WNOHANG | WNOWAIT); }
    while (waiting < 0 && errno == EINTR);
    if (waiting == 0) {
        kill(-rt->child, SIGTERM);
        double until = rt_clock() + .3;
        while (rt_clock() < until) {
            struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
        }
        kill(-rt->child, SIGKILL);
        while (waitpid(rt->child, NULL, 0) < 0 && errno == EINTR) {}
    }
    rt->child = 0;
}

void rt_stop(Rt *rt, const char *message) {
    if (rt->upstream_in >= 0) close(rt->upstream_in);
    if (rt->upstream_out >= 0) close(rt->upstream_out);
    rt->upstream_in = rt->upstream_out = -1;
    rt->upstream.length = 0;
    rt->upstream_protocol[0] = '\0';
    /* Client backpressure must never delay stopping the owned process group. */
    stop_child(rt);
    if (message) rt_fail_pending(rt, message);
}

static int wait_reply(Rt *rt, const char *id, json_object **answer) {
    double deadline = rt_clock() + 5;
    for (int skipped = 0; skipped < 64 && rt_clock() < deadline;) {
        json_object *packet = NULL;
        int ready = rt_next(rt->km, &rt->upstream, &packet);
        if (ready < 0) return -1;
        if (ready) {
            const char *reply_id = km_string(packet, "id");
            json_object *value = km_get(packet, "id");
            if (reply_id && strlen(reply_id) == (size_t)json_object_get_string_len(value) && !strcmp(reply_id, id)) {
                *answer = packet;
                return 0;
            }
            /* Base-protocol pings and rejected optional callbacks must still
             * get responses while initialization/discovery is in progress. */
            if (km_string(packet, "method") && rt_server(rt, packet)) { json_object_put(packet); return -1; }
            json_object_put(packet); skipped++; continue;
        }
        struct pollfd fd = {rt->upstream_out, POLLIN, 0};
        int n = poll(&fd, 1, (int)((deadline - rt_clock()) * 1000));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || rt_read(rt->km, rt->upstream_out, &rt->upstream) <= 0)
            return km_error(rt->km, "MCP handshake failed or timed out; use direct mode for diagnostics");
    }
    return km_error(rt->km, "MCP handshake exceeded its message/time limit");
}

static bool nonempty_text(json_object *object, const char *key) {
    json_object *value = km_get(object, key);
    const char *text = km_string(object, key);
    return text && *text && strlen(text) == (size_t)json_object_get_string_len(value);
}

static bool tools_capabilities(json_object *caps) {
    if (!json_object_is_type(caps, json_type_object) || json_object_object_length(caps) != 1) return false;
    json_object *tools = km_get(caps, "tools");
    if (!json_object_is_type(tools, json_type_object)) return false;
    json_object_object_foreach(tools, name, value) {
        if (strcmp(name, "listChanged") || !json_object_is_type(value, json_type_boolean)) return false;
    }
    return true;
}

static json_object *request(const char *method, const char *id) {
    json_object *packet = json_object_new_object();
    json_object_object_add(packet, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(packet, "method", json_object_new_string(method));
    if (id) json_object_object_add(packet, "id", json_object_new_string(id));
    return packet;
}

static int handshake(Rt *rt) {
    json_object *packet = request("initialize", "kitmux/initialize"), *params = NULL, *answer = NULL;
    if (json_object_deep_copy(rt->initialize, &params, NULL)) {
        json_object_put(packet);
        return km_error(rt->km, "Cannot retain upstream MCP initialization");
    }
    json_object_object_add(params, "capabilities", json_object_new_object());
    json_object_object_add(packet, "params", params);
    int status = rt_send(rt->km, rt->upstream_in, packet);
    json_object_put(packet);
    if (status || wait_reply(rt, "kitmux/initialize", &answer)) return -1;
    json_object *result = km_get(answer, "result"), *caps = km_get(result, "capabilities"), *reserved = NULL;
    const char *version = km_string(result, "protocolVersion");
    json_object *info = km_get(result, "serverInfo");
    bool compatible = !json_object_object_get_ex(answer, "method", &reserved) &&
                      !json_object_object_get_ex(answer, "error", &reserved) &&
                      nonempty_text(result, "protocolVersion") && rt_legacy_version(version) &&
                      tools_capabilities(caps) && nonempty_text(info, "name") && nonempty_text(info, "version");
    if (compatible) snprintf(rt->upstream_protocol, sizeof(rt->upstream_protocol), "%s", version);
    json_object_put(answer);
    if (!compatible) return km_error(rt->km, "Managed bridge requires a tools-only legacy MCP endpoint; use direct mode");
    packet = request("notifications/initialized", NULL);
    status = rt_send(rt->km, rt->upstream_in, packet);
    json_object_put(packet);
    return status;
}

int rt_start(Rt *rt) {
    if (rt->child > 0) return 0;
    if (!rt->initialize) return km_error(rt->km, "Waiting for the client's MCP initialization");
    if (spawn_child(rt)) return -1;
    if (!handshake(rt)) return 0;
    rt_stop(rt, NULL);
    return -1;
}

int rt_discover(Rt *rt) {
    if (rt_start(rt)) return -1;
    json_object *packet = request("tools/list", "kitmux/discovery"), *answer = NULL;
    int status = rt_send(rt->km, rt->upstream_in, packet);
    json_object_put(packet);
    if (!status) status = wait_reply(rt, "kitmux/discovery", &answer);
    if (!status && !json_object_is_type(km_get(km_get(answer, "result"), "tools"), json_type_array))
        status = km_error(rt->km, "MCP discovery failed after reconnect");
    json_object_put(answer);
    if (status) rt_stop(rt, NULL);
    return status;
}
