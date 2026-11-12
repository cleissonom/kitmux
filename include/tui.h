#ifndef KITMUX_TUI_H
#define KITMUX_TUI_H
#include <signal.h>
#include <time.h>
#include "kitmux.h"
#include <curses.h>

enum { UI_MCPS, UI_SKILLS, UI_SUBAGENTS, UI_CLIENTS, UI_STATUS, UI_TAB_COUNT };
typedef enum { UI_RESOURCE, UI_GROUP, UI_UNGROUPED } KmUiRowType;

typedef struct {
    char *id;
    char *group;
    KmUiRowType type;
} KmUiRow;

typedef struct {
    Km *km;
    int tab, row, top, count;
    KmUiRow *rows;
    int capacity;
    char focus[65];
    bool focus_group;
    char filter[128];
    char notice[512];
    char output[32768];
    char snapshot[32768];
    int snapshot_top;
    time_t snapshot_at;
    bool snapshot_more, snapshot_truncated;
    json_object *marks;
    json_object *collapsed;
    bool colors, done, details, truncated;
} KmUi;

extern volatile sig_atomic_t km_ui_interrupted;
const char *ui_kind(const KmUi *ui);
const char *ui_current(const KmUi *ui);
const KmUiRow *ui_current_row(const KmUi *ui);
bool ui_resource_selected(const KmUi *ui);
bool ui_group_selected(const KmUi *ui);
json_object *ui_current_item(KmUi *ui);
json_object *ui_group_item(KmUi *ui, const char *id);
void ui_mark_key(const KmUiRow *row, char *key, size_t capacity);
bool ui_is_collapsed(KmUi *ui, const KmUiRow *row);
void ui_expand(KmUi *ui, bool expanded);
void ui_free_rows(KmUi *ui);
json_object *ui_item(KmUi *ui, const char *id);
void ui_rebuild(KmUi *ui);
void ui_draw(KmUi *ui);
void ui_notice(KmUi *ui, const char *format, ...);
int ui_command(KmUi *ui, int argc, char **argv);
bool ui_input(KmUi *ui, const char *title, const char *label,
              char *value, size_t capacity);
bool ui_confirm(KmUi *ui, const char *message);
bool ui_modal_ready(void);
bool ui_review(KmUi *ui, const char *title);
void ui_view(KmUi *ui, const char *title);
void ui_document_view(KmUi *ui, const char *title, const char *text, bool markdown);
void ui_resource_details(KmUi *ui);
int ui_choice(const char *title, const char *const *choices, int count, int initial);
const char *ui_text_lines(const char *text, int row, int rows, int offset);
void ui_refresh_status(KmUi *ui);
void ui_action(KmUi *ui, int key);
void ui_assign(KmUi *ui);
void ui_members(KmUi *ui);
void ui_add(KmUi *ui, bool editing);
void ui_add_group(KmUi *ui, bool editing);
void ui_duplicate(KmUi *ui);
void ui_sync(KmUi *ui, bool importing);
void ui_reconnect(KmUi *ui);
int ui_tokens(char *text, char **argv, int capacity);
#endif
