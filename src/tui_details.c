#include "tui.h"
#include "subagents.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* A profile can have shared instructions and three effective native variants. */
#define DOCUMENT_LIMIT (4u * KM_MAX_FILE + 65536u)

typedef struct {
    Km *km;
    char *text;
    size_t length, capacity;
    bool failed;
} Document;

static void append(Document *doc, const char *format, ...) {
    if (doc->failed) return;
    va_list args, copy;
    va_start(args, format); va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || (size_t)needed > DOCUMENT_LIMIT - doc->length) doc->failed = true;
    size_t total = doc->length + (needed > 0 ? (size_t)needed : 0) + 1;
    if (!doc->failed && total > doc->capacity) {
        size_t capacity = total > doc->capacity * 2 ? total : doc->capacity * 2;
        char *text = realloc(doc->text, capacity);
        if (!text) doc->failed = true;
        else { doc->text = text; doc->capacity = capacity; }
    }
    if (!doc->failed) { vsnprintf(doc->text + doc->length, total - doc->length, format, args); doc->length = total - 1; }
    va_end(args);
}

static void availability(Document *doc, KmUi *ui, json_object *item, const char *id) {
    append(doc, "## Availability\n\n");
    for (size_t i = 0; i < KM_AGENT_COUNT; i++) {
        char reason[192] = "";
        bool compatible = ui->tab != UI_SUBAGENTS || km_subagent_compatible(item, km_agents[i], reason, sizeof(reason));
        bool enabled = compatible && km_effective(ui->km, ui_kind(ui), id, km_agents[i], reason, sizeof(reason));
        append(doc, "%s: %s - %s\n", km_agents[i], compatible ? enabled ? "ON" : "OFF" : "N/A", reason);
    }
    append(doc, "\n");
}

static void mcp_document(Document *doc, json_object *item) {
    const char *transport = km_string(item, "transport"), *mode = km_string(item, "mode");
    append(doc, "Transport: %s\nRuntime mode: %s\n\n", transport ? transport : "unknown", mode ? mode : "direct");
    const char *description = km_string(item, "description");
    append(doc, "%s\n", description && *description ? description :
           "No description supplied. Add a Markdown description with mcp edit.");
    /* Endpoint arguments, native snapshots and environment values can contain credentials. */
}

static void skill_document(Document *doc, json_object *item) {
    const char *source = km_string(item, "source"), *revision = km_string(item, "revision");
    bool remote = source && (strstr(source, "://") || strchr(source, '@'));
    append(doc, "Source: %s\nRevision: %s\n\n", remote ? "Remote repository (URL credentials/options hidden)" :
           source ? source : "Unknown", revision ? revision : "Unknown");
    const char *root = km_string(item, "path");
    char path[PATH_MAX]; size_t length = 0;
    if (!root || km_path(doc->km, path, sizeof(path), "%s/SKILL.md", root) < 0) { doc->failed = true; return; }
    char *text = km_read_file(doc->km, path, &length);
    if (!text) { doc->failed = true; return; }
    if (memchr(text, '\0', length)) { free(text); km_error(doc->km, "Installed SKILL.md contains NUL bytes"); doc->failed = true; return; }
    append(doc, "## SKILL.md\n\n%s\n", text);
    free(text);
}

static void variant_document(Document *doc, KmUi *ui, json_object *item, const char *id, const char *client) {
    json_object *native = km_get(km_get(item, "native"), client);
    if (!native) return;
    char *text = km_subagent_render(ui->km, item, id, client);
    json_object *parsed = text ? km_subagent_parse(ui->km, text, km_string(native, "format"), id) : NULL;
    free(text);
    if (!parsed) { doc->failed = true; return; }
    append(doc, "## %s variant\n\n%s\n\n%s\n\n", client,
           km_string(parsed, "description"), km_string(parsed, "instructions"));
    json_object_put(parsed);
}

static void subagent_document(Document *doc, KmUi *ui, json_object *item, const char *id) {
    append(doc, "## Shared description\n\n%s\n\n## Shared instructions\n\n%s\n\n",
           km_string(item, "description"), km_string(item, "instructions"));
    for (size_t i = 0; i < KM_AGENT_COUNT && !doc->failed; i++)
        variant_document(doc, ui, item, id, km_agents[i]);
    append(doc, "Native reload required after changes; running work stays native.\n");
}

void ui_resource_details(KmUi *ui) {
    const char *id = ui_current(ui);
    json_object *item = ui_current_item(ui);
    if (!ui_resource_selected(ui) || !id || !item) return;
    Document doc = {.km = ui->km};
    ui->km->error[0] = '\0';
    append(&doc, "# %s: %s\n\n", ui_kind(ui), id);
    if (ui->tab == UI_MCPS) mcp_document(&doc, item);
    if (ui->tab == UI_SKILLS) skill_document(&doc, item);
    availability(&doc, ui, item, id);
    if (ui->tab == UI_SUBAGENTS) subagent_document(&doc, ui, item, id);
    if (doc.failed) ui_notice(ui, "%.480s", *ui->km->error ? ui->km->error : "Cannot open details: document exceeds the size limit or memory is unavailable.");
    else {
        char title[128]; snprintf(title, sizeof(title), "Resource details: %s", id);
        ui_document_view(ui, title, doc.text, true);
    }
    free(doc.text);
}
