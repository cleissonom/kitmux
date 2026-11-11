#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void stop_signal(int signal_number) { (void)signal_number; interrupted = 1; }

static int drain(Rt *rt, bool downstream) {
    RtReader *reader = downstream ? &rt->downstream : &rt->upstream;
    for (;;) {
        json_object *packet = NULL;
        int next = rt_next(rt->km, reader, &packet);
        if (!next) return 0;
        if (next < 0) {
            if (downstream) { if (rt_error(rt, NULL, -32700, "Invalid JSON-RPC input")) return -1; continue; }
            rt_stop(rt, "Invalid MCP input; completion unknown, call not replayed");
            return rt_changed(rt);
        }
        int status = downstream ? rt_client(rt, packet) : rt_server(rt, packet);
        json_object_put(packet);
        if (status) return -1;
        if (!downstream && rt->upstream_out < 0) return 0;
    }
}

static int event_loop(Rt *rt) {
    while (!interrupted) {
        if (rt_refresh(rt, false) || drain(rt, true) || drain(rt, false)) return -1;
        struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {rt->control, POLLIN, 0}, {rt->upstream_out, POLLIN, 0}};
        int count = poll(fds, 3, 100);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return km_error(rt->km, "MCP bridge polling failed");
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            int input = rt_read(rt->km, STDIN_FILENO, &rt->downstream);
            if (input == 0) return 0;
            if (input < 0) return -1;
        }
        if (fds[1].revents & POLLIN) rt_control_accept(rt);
        if (fds[2].fd == rt->upstream_out && rt->upstream_out >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (rt_read(rt->km, rt->upstream_out, &rt->upstream) <= 0) {
                rt_stop(rt, "MCP disconnected; completion unknown, call not replayed");
                if (rt_changed(rt)) return -1;
            }
        }
    }
    return 0;
}

static void release(Rt *rt) {
    rt_stop(rt, NULL);
    if (rt->control >= 0) close(rt->control);
    if (rt->control_bound) unlink(rt->socket_path);
    json_object_put(rt->initialize);
    json_object_put(rt->pending);
    json_object_put(rt->execution);
    free(rt->upstream.bytes);
    free(rt->downstream.bytes);
}

static int bridge(Km *km, const char *id, const char *agent) {
    json_object *server = km_get(km_get(km->catalog, "mcps"), id);
    const char *mode = km_string(server, "mode"), *transport = km_string(server, "transport");
    if (!server || !mode || strcmp(mode, "managed") || !transport || strcmp(transport, "stdio"))
        return km_error(km, "Bridge requires an existing managed local stdio MCP");
    Rt rt = {.km = km, .id = id, .agent = agent, .upstream_in = -1, .upstream_out = -1, .control = -1};
    rt.pending = json_object_new_object();
    rt.downstream.bytes = malloc(RT_MAX_LINE + 1);
    rt.upstream.bytes = malloc(RT_MAX_LINE + 1);
    if (!rt.pending || !rt.downstream.bytes || !rt.upstream.bytes) { release(&rt); return km_error(km, "Cannot allocate MCP bridge buffers"); }
    if (rt_control_open(&rt) || rt_refresh(&rt, true)) { release(&rt); return -1; }
    struct sigaction handler = {.sa_handler = stop_signal}, ignore = {.sa_handler = SIG_IGN};
    struct sigaction child = {.sa_handler = SIG_DFL};
    struct sigaction old_int, old_term, old_pipe, old_child;
    sigemptyset(&handler.sa_mask); sigemptyset(&ignore.sa_mask);
    interrupted = 0;
    sigaction(SIGINT, &handler, &old_int); sigaction(SIGTERM, &handler, &old_term); sigaction(SIGPIPE, &ignore, &old_pipe);
    sigemptyset(&child.sa_mask); sigaction(SIGCHLD, &child, &old_child);
    int old_flags = fcntl(STDOUT_FILENO, F_GETFL);
    rt_nonblock(STDOUT_FILENO);
    int status = event_loop(&rt);
    release(&rt);
    fcntl(STDOUT_FILENO, F_SETFL, old_flags);
    sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL); sigaction(SIGPIPE, &old_pipe, NULL);
    sigaction(SIGCHLD, &old_child, NULL);
    return status;
}

int km_runtime_command(Km *km, int argc, char **argv) {
    if (argc < 2 || !km_valid_id(argv[1])) return km_error(km, "Usage: bridge|reconnect MCP --agent AGENT [--json]");
    const char *agent = NULL;
    bool json = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--agent") && i + 1 < argc && !agent) agent = argv[++i];
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else return km_error(km, "Unknown or duplicate runtime option: %s", argv[i]);
    }
    if (!agent || !km_valid_agent(agent)) return km_error(km, "Choose --agent codex, claude, or opencode");
    if (km_load(km)) return -1;
    if (!strcmp(argv[0], "bridge")) {
        if (json) return km_error(km, "bridge reserves stdout for the MCP protocol; --json is not applicable");
        return bridge(km, argv[1], agent);
    }
    if (!strcmp(argv[0], "reconnect")) return rt_reconnect(km, argv[1], agent, json);
    return km_error(km, "Unknown MCP runtime command: %s", argv[0]);
}
