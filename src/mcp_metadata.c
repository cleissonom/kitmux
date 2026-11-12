#include "mcp_metadata.h"
#include <stdlib.h>
#include <string.h>

int km_mcp_description_option(Km *km, KmMcpDescription *choice, const char *option, const char *value) {
    bool text = !strcmp(option, "--description"), file = !strcmp(option, "--description-file");
    if (!text && !file) return 0;
    if (choice->text || choice->file)
        return km_error(km, "Use one --description TEXT or --description-file FILE");
    if (text) choice->text = value;
    else choice->file = value;
    return 1;
}

static bool valid_utf8(json_object *value) {
    struct json_tokener *parser = json_tokener_new();
    if (!parser) return false;
    const char *encoded = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    if (!encoded) { json_tokener_free(parser); return false; }
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *parsed = json_tokener_parse_ex(parser, encoded, (int)strlen(encoded) + 1);
    bool valid = json_tokener_get_error(parser) == json_tokener_success;
    json_object_put(parsed); json_tokener_free(parser);
    return valid;
}

int km_mcp_description_apply(Km *km, json_object *resource, const KmMcpDescription *choice) {
    if (!choice->text && !choice->file) return 0;
    size_t length = choice->text ? strlen(choice->text) : 0;
    char *owned = choice->file ? km_read_file(km, choice->file, &length) : NULL;
    if (choice->file && !owned) return -1;
    const char *text = owned ? owned : choice->text;
    if (length > KM_MAX_FILE || memchr(text, '\0', length)) {
        free(owned); return km_error(km, "MCP description must be text without NUL bytes, no larger than 8 MiB");
    }
    json_object *value = json_object_new_string_len(text, (int)length);
    free(owned);
    if (!value || !valid_utf8(value)) {
        json_object_put(value); return km_error(km, "MCP description must be valid UTF-8 text");
    }
    json_object_object_add(resource, "description", value);
    return 0;
}
