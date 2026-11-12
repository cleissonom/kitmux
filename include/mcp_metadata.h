#ifndef KITMUX_MCP_METADATA_H
#define KITMUX_MCP_METADATA_H
#include "kitmux.h"

typedef struct {
    const char *text;
    const char *file;
} KmMcpDescription;

/* Returns 1 for a recognized option, 0 for another option, or -1 on error. */
int km_mcp_description_option(Km *km, KmMcpDescription *choice, const char *option, const char *value);
int km_mcp_description_apply(Km *km, json_object *resource, const KmMcpDescription *choice);
#endif
