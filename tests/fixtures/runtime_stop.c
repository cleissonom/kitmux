#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    int ready[2], blocked[2];
    if (pipe(ready) || pipe(blocked)) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        close(ready[0]);
        if (setpgid(0, 0) || write(ready[1], "x", 1) != 1) _exit(2);
        for (;;) pause();
    }
    close(ready[1]);
    char byte, bytes[4096] = {0};
    if (read(ready[0], &byte, 1) != 1) return 2;
    close(ready[0]);
    if (rt_nonblock(blocked[1])) return 2;
    while (write(blocked[1], bytes, sizeof(bytes)) > 0) {}
    if (errno != EAGAIN || dup2(blocked[1], STDOUT_FILENO) < 0) return 2;
    close(blocked[1]);
    Km km = {0};
    Rt rt = {.km = &km, .child = child, .upstream_in = -1, .upstream_out = -1};
    rt.pending = json_object_new_object();
    for (int i = 0; i < RT_MAX_PENDING; i++) {
        char key[16]; snprintf(key, sizeof(key), "%d", i);
        json_object *record = json_object_new_object();
        json_object_object_add(record, "id", json_object_new_int(i));
        json_object_object_add(rt.pending, key, record);
    }
    fprintf(stderr, "%ld\n", (long)child); fflush(stderr);
    rt_stop(&rt, "Completion unknown; call not replayed");
    json_object_put(rt.pending);
    close(blocked[0]);
    return rt.child == 0 ? 0 : 1;
}
