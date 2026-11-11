#ifndef KITMUX_SUBAGENTS_H
#define KITMUX_SUBAGENTS_H
#include "kitmux.h"

/* Native snapshots retain exact source text and format (codex, claude,
 * opencode, opencode-json). Their description/instructions are the canonical
 * values at attachment, not necessarily the values in that client's text.
 * Equal canonical values preserve native variants; a later shared edit changes
 * only the edited common field. Rebase these two values when attaching a
 * variant to an existing resource. Imported definitions are not portable.
 */
json_object *km_subagent_parse(Km *km, const char *text, const char *format, const char *id);
char *km_subagent_render(Km *km, json_object *resource, const char *id, const char *client);
int km_subagent_validate(Km *km, const char *id, json_object *resource);
bool km_subagent_compatible(json_object *resource, const char *client, char *reason, size_t size);

/* Codec internals. Parsed documents contain description, instructions, optional
 * name and disabled. Editors change only the requested common fields.
 */
json_object *km_subagent_markdown(Km *km, const char *text, const char *client);
char *km_subagent_markdown_mode(Km *km, const char *text);
char *km_subagent_markdown_edit(Km *km, const char *text, const char *client,
                               const char *id, const char *description, const char *instructions);
json_object *km_subagent_toml(Km *km, const char *text);
char *km_subagent_toml_edit(Km *km, const char *text, const char *id,
                           const char *description, const char *instructions);
#endif
