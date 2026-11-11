#ifndef KITMUX_RUNTIME_H
#define KITMUX_RUNTIME_H
#include "kitmux.h"
#include <sys/types.h>

#define RT_MAX_LINE (1024u * 1024u)
#define RT_MAX_PENDING 64
typedef struct { char *bytes; size_t length; } RtReader;
typedef struct {
    Km *km;
    const char *id;
    const char *agent;
    bool enabled;
    bool initialized;
    bool control_bound;
    pid_t child;
    int upstream_in;
    int upstream_out;
    char upstream_protocol[16];
    int control;
    char socket_path[PATH_MAX];
    json_object *execution;
    json_object *initialize;
    json_object *pending;
    RtReader downstream;
    RtReader upstream;
    dev_t catalog_dev;
    ino_t catalog_inode;
} Rt;

double rt_clock(void);
int rt_nonblock(int fd);
int rt_send(Km *km, int fd, json_object *message);
int rt_error(Rt *rt, json_object *id, int code, const char *message);
int rt_result(Rt *rt, json_object *id, json_object *result);
int rt_read(Km *km, int fd, RtReader *reader);
int rt_next(Km *km, RtReader *reader, json_object **message);
void rt_fail_pending(Rt *rt, const char *message);
int rt_start(Rt *rt);
void rt_stop(Rt *rt, const char *message);
int rt_discover(Rt *rt);
int rt_refresh(Rt *rt, bool force);
int rt_client(Rt *rt, json_object *message);
int rt_server(Rt *rt, json_object *message);
int rt_changed(Rt *rt);
bool rt_legacy_version(const char *version);
int rt_control_open(Rt *rt);
void rt_control_accept(Rt *rt);
int rt_reconnect(Km *km, const char *id, const char *agent, bool json);
json_object *rt_control_connections(Km *km, const char *id, const char *agent,
                                    const char *command, int timeout_ms, bool *scan_complete);
json_object *rt_connection_status(Rt *rt);
json_object *rt_status(Km *km, const char *id, const char *agent);
#endif
