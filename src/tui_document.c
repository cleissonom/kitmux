#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1
#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

typedef struct {
    const char *start, *end;
    int style, indent;
} DocumentRow;

typedef struct {
    DocumentRow *rows;
    size_t count, capacity;
} DocumentRows;

static size_t character(const char *text, size_t remaining, wchar_t *value) {
    mbstate_t state = {0};
    size_t bytes = mbrtowc(value, text, remaining, &state);
    if (bytes == (size_t)-1 || bytes == (size_t)-2 || !bytes) { *value = L'?'; return 1; }
    if (*value != L'\t' && (iswcntrl(*value) || wcwidth(*value) < 0)) *value = L' ';
    return bytes;
}

static int character_width(wchar_t value, int column) {
    return value == L'\t' ? 4 - column % 4 : wcwidth(value);
}

static bool add_row(DocumentRows *rows, const char *start, const char *end, int style, int indent) {
    if (rows->count == rows->capacity) {
        size_t capacity = rows->capacity ? rows->capacity * 2 : 128;
        if (capacity > 4u * KM_MAX_FILE + 65536u) return false;
        DocumentRow *data = realloc(rows->rows, capacity * sizeof(*data));
        if (!data) return false;
        rows->rows = data; rows->capacity = capacity;
    }
    rows->rows[rows->count++] = (DocumentRow){start, end, style, indent};
    return true;
}

static const char *wrap_end(const char *text, const char *end, int width) {
    const char *cursor = text, *space = NULL;
    int column = 0, space_column = 0;
    while (cursor < end) {
        wchar_t value;
        size_t bytes = character(cursor, (size_t)(end - cursor), &value);
        int cells = character_width(value, column);
        if (column + cells > width) break;
        if (value == L' ' || value == L'\t') { space = cursor; space_column = column; }
        column += cells; cursor += bytes;
    }
    if (cursor < end && space && space_column > width / 2) return space;
    return cursor > text ? cursor : text + 1;
}

static bool wrap_line(DocumentRows *rows, const char *text, const char *end, int width, int style, int indent) {
    if (text == end) return add_row(rows, text, end, style, indent);
    while (text < end) {
        const char *next = wrap_end(text, end, width - indent);
        if (!add_row(rows, text, next, style, indent)) return false;
        text = next;
        while (text < end && *text == ' ') text++;
    }
    return true;
}

static const char *heading(const char *text, const char *end) {
    const char *cursor = text;
    while (cursor < end && cursor - text < 6 && *cursor == '#') cursor++;
    return cursor > text && cursor < end && *cursor == ' ' ? cursor + 1 : text;
}

static bool layout(DocumentRows *rows, const char *text, int width, bool markdown) {
    rows->count = 0; char fence = 0;
    do {
        const char *end = strchr(text, '\n');
        if (!end) end = text + strlen(text);
        bool marker = markdown && end - text >= 3 &&
                      (!strncmp(text, "```", 3) || !strncmp(text, "~~~", 3));
        if (marker && (!fence || fence == *text)) fence = fence ? 0 : *text;
        else {
            const char *start = markdown && !fence ? heading(text, end) : text;
            int style = fence ? A_DIM : start != text ? A_BOLD : A_NORMAL;
            if (!wrap_line(rows, start, end, width, style, fence ? 2 : 0)) return false;
        }
        text = *end ? end + 1 : end;
    } while (*text);
    return true;
}

static void draw_row(const DocumentRow *line, int row) {
    move(row, 3 + line->indent); attron(line->style);
    int column = 0;
    for (const char *text = line->start; text < line->end;) {
        wchar_t value;
        text += character(text, (size_t)(line->end - text), &value);
        int width = character_width(value, column);
        if (value == L'\t') for (int i = 0; i < width; i++) addch(' ');
        else addnwstr(&value, 1);
        column += width;
    }
    attroff(line->style);
}

static void draw_document(KmUi *ui, const char *title, const DocumentRows *rows, size_t top, bool markdown) {
    erase(); attron(A_BOLD); mvaddnstr(2, 3, title, COLS - 6); attroff(A_BOLD);
    mvhline(3, 3, ACS_HLINE, COLS - 6);
    size_t visible = (size_t)(LINES - 9);
    for (size_t i = 0; i < visible && top + i < rows->count; i++) draw_row(&rows->rows[top + i], 5 + (int)i);
    mvaddnstr(LINES - 2, 3, !markdown && ui->truncated ? "Output truncated. Inspect full output with the CLI. Esc returns." :
              "j/k scroll  PgUp/PgDn page  g/G ends  Enter/Esc returns", COLS - 6);
    refresh();
}

static size_t navigate(int key, size_t top, size_t count) {
    size_t page = (size_t)(LINES - 9), last = count > page ? count - page : 0;
    if (key == 'g' || key == KEY_HOME) return 0;
    if (key == 'G' || key == KEY_END) return last;
    if (key == 'j' || key == KEY_DOWN) return top < last ? top + 1 : last;
    if (key == 'k' || key == KEY_UP) return top ? top - 1 : 0;
    if (key == KEY_NPAGE || key == ' ') return page < last - top ? top + page : last;
    if (key == KEY_PPAGE) return top > page ? top - page : 0;
    return top < last ? top : last;
}

void ui_document_view(KmUi *ui, const char *title, const char *text, bool markdown) {
    DocumentRows rows = {0}; size_t top = 0;
    int width = 0;
    while (!km_ui_interrupted && ui_modal_ready()) {
        if (width != COLS - 6) {
            const char *anchor = rows.count ? rows.rows[top].start : text;
            width = COLS - 6;
            if (!layout(&rows, text, width, markdown)) { ui_notice(ui, "Cannot lay out document: memory limit reached."); break; }
            top = 0;
            while (top + 1 < rows.count && rows.rows[top + 1].start <= anchor) top++;
            top = navigate(ERR, top, rows.count);
        }
        draw_document(ui, title, &rows, top, markdown);
        int key = getch();
        if (key == 27 || key == 'q' || key == '\n' || key == KEY_ENTER) break;
        top = navigate(key, top, rows.count);
    }
    free(rows.rows);
}
