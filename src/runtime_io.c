#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double rt_clock(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

int rt_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int write_bytes(Km *km, int fd, const char *bytes, size_t length) {
    double deadline = rt_clock() + 5;
    while (length) {
        double remaining = deadline - rt_clock();
        if (remaining <= 0) return km_error(km, "MCP output timed out");
        struct pollfd item = {fd, POLLOUT, 0};
        int ready = poll(&item, 1, (int)(remaining * 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || rt_clock() >= deadline)
            return km_error(km, "MCP output timed out");
        ssize_t n = write(fd, bytes, length > 4096 ? 4096 : length);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return km_error(km, "MCP connection closed while writing");
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

int rt_send(Km *km, int fd, json_object *message) {
    const char *text = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
    if (strlen(text) > RT_MAX_LINE) return km_error(km, "MCP message exceeds 1 MiB");
    if (write_bytes(km, fd, text, strlen(text))) return -1;
    return write_bytes(km, fd, "\n", 1);
}

static json_object *envelope(json_object *id) {
    json_object *packet = json_object_new_object();
    json_object_object_add(packet, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(packet, "id", json_object_get(id));
    return packet;
}

int rt_error(Rt *rt, json_object *id, int code, const char *message) {
    json_object *packet = envelope(id), *error = json_object_new_object();
    json_object_object_add(error, "code", json_object_new_int(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(packet, "error", error);
    int result = rt_send(rt->km, STDOUT_FILENO, packet);
    json_object_put(packet);
    return result;
}

int rt_result(Rt *rt, json_object *id, json_object *result) {
    json_object *packet = envelope(id);
    json_object_object_add(packet, "result", result);
    int status = rt_send(rt->km, STDOUT_FILENO, packet);
    json_object_put(packet);
    return status;
}

int rt_read(Km *km, int fd, RtReader *reader) {
    char bytes[4096];
    size_t room = RT_MAX_LINE - reader->length;
    if (!room) return km_error(km, "MCP message exceeds 1 MiB");
    ssize_t n = read(fd, bytes, room < sizeof(bytes) ? room : sizeof(bytes));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 1;
    if (n <= 0) return n == 0 ? 0 : km_error(km, "MCP input failed");
    if (reader->length + (size_t)n > RT_MAX_LINE)
        return km_error(km, "MCP message exceeds 1 MiB");
    if (memchr(bytes, '\0', (size_t)n)) return km_error(km, "MCP input contains a NUL byte");
    memcpy(reader->bytes + reader->length, bytes, (size_t)n);
    reader->length += (size_t)n;
    return 1;
}

static bool valid_envelope(json_object *message) {
    if (!json_object_is_type(message, json_type_object)) return false;
    const char *version = km_string(message, "jsonrpc");
    json_object *id = NULL;
    bool has_id = json_object_object_get_ex(message, "id", &id);
    enum json_type type = json_object_get_type(id);
    return json_object_is_type(message, json_type_object) && version && !strcmp(version, "2.0") &&
           (!has_id || type == json_type_null || type == json_type_int || type == json_type_string);
}

int rt_next(Km *km, RtReader *reader, json_object **message) {
    char *newline = memchr(reader->bytes, '\n', reader->length);
    if (!newline) return 0;
    size_t length = (size_t)(newline - reader->bytes);
    json_tokener *parser = json_tokener_new();
    if (!parser) return km_error(km, "Cannot allocate MCP parser");
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    *message = json_tokener_parse_ex(parser, reader->bytes, (int)length);
    bool valid = json_tokener_get_error(parser) == json_tokener_success;
    size_t used = json_tokener_get_parse_end(parser);
    while (used < length && isspace((unsigned char)reader->bytes[used])) used++;
    valid = valid && used == length && valid_envelope(*message);
    json_tokener_free(parser);
    memmove(reader->bytes, newline + 1, reader->length - length - 1);
    reader->length -= length + 1;
    if (valid) return 1;
    json_object_put(*message);
    *message = NULL;
    return km_error(km, "Invalid MCP JSON-RPC message");
}

void rt_fail_pending(Rt *rt, const char *message) {
    json_object_object_foreach(rt->pending, key, record) {
        (void)key;
        if (!json_object_get_boolean(km_get(record, "cancelled")) &&
            rt_error(rt, km_get(record, "id"), -32000, message)) break;
    }
    json_object_put(rt->pending);
    rt->pending = json_object_new_object();
}
