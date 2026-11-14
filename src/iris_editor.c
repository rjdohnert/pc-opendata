#include "database_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

#define IRIS_MAX_LINES 4096
#define IRIS_LINE_CAP  1024
#define IRIS_CUT_MAX   256

struct IrisEditor {
    char* lines[IRIS_MAX_LINES];
    int line_count;
    int cur_row;
    int cur_col;
    int row_offset;
    int col_offset;
    int screen_rows;
    int screen_cols;
    bool modified;
    bool show_help;
    bool execute_on_exit;
    char filename[256];
    char status_msg[256];
    char last_search[128];
    char* cut_buffer[IRIS_CUT_MAX];
    int cut_count;
    bool last_was_cut;
};

static char* iris_strdup(const char* s) {
    size_t len = s ? strlen(s) : 0;
    char* copy = (char*)malloc(len + 1);
    if (!copy) return NULL;
    if (len > 0) memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static void iris_clear_lines(IrisEditor* ed) {
    for (int i = 0; i < ed->line_count; i++) {
        free(ed->lines[i]);
        ed->lines[i] = NULL;
    }
    ed->line_count = 0;
}

static void iris_clear_cut_buffer(IrisEditor* ed) {
    for (int i = 0; i < ed->cut_count; i++) {
        free(ed->cut_buffer[i]);
        ed->cut_buffer[i] = NULL;
    }
    ed->cut_count = 0;
}

static void iris_adjust_viewport(IrisEditor* ed) {
    int text_rows = ed->screen_rows - 4;
    if (text_rows < 1) text_rows = 1;
    int text_cols = ed->screen_cols > 4 ? ed->screen_cols : 80;

    if (ed->cur_row < 0) ed->cur_row = 0;
    if (ed->cur_row >= ed->line_count) ed->cur_row = ed->line_count - 1;

    int line_len = (int)strlen(ed->lines[ed->cur_row]);
    if (ed->cur_col < 0) ed->cur_col = 0;
    if (ed->cur_col > line_len) ed->cur_col = line_len;

    if (ed->cur_row < ed->row_offset) {
        ed->row_offset = ed->cur_row;
    } else if (ed->cur_row >= ed->row_offset + text_rows) {
        ed->row_offset = ed->cur_row - text_rows + 1;
    }

    if (ed->cur_col < ed->col_offset) {
        ed->col_offset = ed->cur_col;
    } else if (ed->cur_col >= ed->col_offset + text_cols) {
        ed->col_offset = ed->cur_col - text_cols + 1;
    }
}

IrisEditor* iris_create(void) {
    IrisEditor* ed = (IrisEditor*)calloc(1, sizeof(IrisEditor));
    if (!ed) return NULL;
    ed->screen_rows = 24;
    ed->screen_cols = 80;
    ed->lines[0] = iris_strdup("");
    if (!ed->lines[0]) {
        free(ed);
        return NULL;
    }
    ed->line_count = 1;
    snprintf(ed->filename, sizeof(ed->filename), "SQL Buffer");
    snprintf(ed->status_msg, sizeof(ed->status_msg), "IRIS 1.0 Ready - ^G for Help, ^X to Exit/Execute");
    return ed;
}

void iris_destroy(IrisEditor* ed) {
    if (!ed) return;
    iris_clear_lines(ed);
    iris_clear_cut_buffer(ed);
    free(ed);
}

void iris_set_filename(IrisEditor* ed, const char* filename) {
    if (!ed) return;
    if (filename && filename[0]) {
        snprintf(ed->filename, sizeof(ed->filename), "%s", filename);
    } else {
        snprintf(ed->filename, sizeof(ed->filename), "SQL Buffer");
    }
}

bool iris_load_text(IrisEditor* ed, const char* text) {
    if (!ed) return false;
    iris_clear_lines(ed);

    if (!text || !text[0]) {
        ed->lines[0] = iris_strdup("");
        ed->line_count = 1;
        ed->cur_row = 0;
        ed->cur_col = 0;
        ed->row_offset = 0;
        ed->col_offset = 0;
        ed->modified = false;
        return ed->lines[0] != NULL;
    }

    const char* p = text;
    while (*p && ed->line_count < IRIS_MAX_LINES) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;
        if (len >= IRIS_LINE_CAP) len = IRIS_LINE_CAP - 1;

        char* line = (char*)malloc(len + 1);
        if (!line) return false;
        memcpy(line, p, len);
        line[len] = '\0';
        ed->lines[ed->line_count++] = line;

        if (!nl) break;
        p = nl + 1;
    }

    if (ed->line_count == 0) {
        ed->lines[0] = iris_strdup("");
        if (!ed->lines[0]) return false;
        ed->line_count = 1;
    }

    ed->cur_row = 0;
    ed->cur_col = 0;
    ed->row_offset = 0;
    ed->col_offset = 0;
    ed->modified = false;
    snprintf(ed->status_msg, sizeof(ed->status_msg), "Read %d line%s",
             ed->line_count, ed->line_count == 1 ? "" : "s");
    return true;
}

bool iris_load_file(IrisEditor* ed, const char* filepath) {
    if (!ed || !filepath || !filepath[0]) return false;
    iris_set_filename(ed, filepath);
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        iris_load_text(ed, "");
        snprintf(ed->status_msg, sizeof(ed->status_msg), "New File: %s", filepath);
        return true;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 10 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    bool ok = iris_load_text(ed, buf);
    free(buf);
    return ok;
}

static bool iris_insert_line_at(IrisEditor* ed, int idx, const char* text) {
    if (ed->line_count >= IRIS_MAX_LINES || idx < 0 || idx > ed->line_count) return false;
    char* copy = iris_strdup(text ? text : "");
    if (!copy) return false;
    for (int i = ed->line_count; i > idx; i--) {
        ed->lines[i] = ed->lines[i - 1];
    }
    ed->lines[idx] = copy;
    ed->line_count++;
    return true;
}

static void iris_delete_line_at(IrisEditor* ed, int idx) {
    if (idx < 0 || idx >= ed->line_count) return;
    free(ed->lines[idx]);
    for (int i = idx; i < ed->line_count - 1; i++) {
        ed->lines[i] = ed->lines[i + 1];
    }
    ed->line_count--;
    if (ed->line_count == 0) {
        ed->lines[0] = iris_strdup("");
        if (ed->lines[0]) ed->line_count = 1;
    }
}

bool iris_insert_file(IrisEditor* ed, const char* filepath) {
    if (!ed || !filepath || !filepath[0]) return false;
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Cannot open \"%s\" for reading", filepath);
        return false;
    }
    char line_buf[IRIS_LINE_CAP];
    int inserted = 0;
    while (fgets(line_buf, sizeof(line_buf), f) && ed->line_count < IRIS_MAX_LINES) {
        size_t len = strlen(line_buf);
        while (len > 0 && (line_buf[len - 1] == '\n' || line_buf[len - 1] == '\r')) {
            line_buf[--len] = '\0';
        }
        if (iris_insert_line_at(ed, ed->cur_row + 1 + inserted, line_buf)) {
            inserted++;
        }
    }
    fclose(f);
    ed->modified = true;
    snprintf(ed->status_msg, sizeof(ed->status_msg), "Read %d line%s from %s",
             inserted, inserted == 1 ? "" : "s", filepath);
    return true;
}

bool iris_save_file(IrisEditor* ed, const char* filepath) {
    if (!ed) return false;
    const char* target = (filepath && filepath[0]) ? filepath : ed->filename;
    if (!target || !target[0] || !strcmp(target, "SQL Buffer")) {
        target = "iris_buffer.sql";
    }
    FILE* f = fopen(target, "wb");
    if (!f) {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Error writing %s", target);
        return false;
    }
    for (int i = 0; i < ed->line_count; i++) {
        fputs(ed->lines[i], f);
        if (i + 1 < ed->line_count) fputc('\n', f);
    }
    fclose(f);
    ed->modified = false;
    if (filepath && filepath[0]) {
        snprintf(ed->filename, sizeof(ed->filename), "%s", filepath);
    }
    snprintf(ed->status_msg, sizeof(ed->status_msg), "Wrote %d line%s to %s",
             ed->line_count, ed->line_count == 1 ? "" : "s", target);
    return true;
}

size_t iris_export_text(const IrisEditor* ed, char* out_buf, size_t out_cap) {
    if (!ed || !out_buf || out_cap == 0) return 0;
    size_t pos = 0;
    out_buf[0] = '\0';
    for (int i = 0; i < ed->line_count; i++) {
        size_t len = strlen(ed->lines[i]);
        if (pos + len >= out_cap) {
            len = out_cap - pos - 1;
        }
        if (len > 0) {
            memcpy(out_buf + pos, ed->lines[i], len);
            pos += len;
        }
        if (i + 1 < ed->line_count && pos + 1 < out_cap) {
            out_buf[pos++] = '\n';
        }
    }
    out_buf[pos] = '\0';
    return pos;
}

static const char* iris_stristr(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return NULL;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        bool match = true;
        for (size_t i = 0; i < nlen; i++) {
            if (!p[i] || tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) {
                match = false;
                break;
            }
        }
        if (match) return p;
    }
    return NULL;
}

bool iris_search(IrisEditor* ed, const char* query) {
    if (!ed) return false;
    if (query && query[0]) {
        snprintf(ed->last_search, sizeof(ed->last_search), "%s", query);
    }
    if (!ed->last_search[0]) {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "No search pattern set");
        return false;
    }

    for (int step = 0; step <= ed->line_count; step++) {
        int r = (ed->cur_row + step) % ed->line_count;
        const char* line = ed->lines[r];
        int start_c = 0;
        if (step == 0) {
            int line_len = (int)strlen(line);
            start_c = (ed->cur_col < line_len) ? (ed->cur_col + 1) : line_len;
        }
        const char* found = iris_stristr(line + start_c, ed->last_search);
        if (found) {
            ed->cur_row = r;
            ed->cur_col = (int)(found - line);
            iris_adjust_viewport(ed);
            snprintf(ed->status_msg, sizeof(ed->status_msg), "Found \"%s\" at line %d, col %d",
                     ed->last_search, ed->cur_row + 1, ed->cur_col + 1);
            return true;
        }
    }
    snprintf(ed->status_msg, sizeof(ed->status_msg), "\"%s\" not found", ed->last_search);
    return false;
}

void iris_justify_sql(IrisEditor* ed) {
    if (!ed) return;
    size_t raw_cap = 65536;
    char* raw = (char*)malloc(raw_cap);
    if (!raw) return;
    size_t len = iris_export_text(ed, raw, raw_cap);

    char* norm = (char*)malloc(len * 2 + 64);
    if (!norm) {
        free(raw);
        return;
    }

    size_t out_i = 0;
    bool in_str = false;
    bool last_space = true;
    for (size_t i = 0; i < len; i++) {
        char c = raw[i];
        if (c == '\'') {
            in_str = !in_str;
            norm[out_i++] = c;
            last_space = false;
        } else if (!in_str && isspace((unsigned char)c)) {
            if (!last_space) {
                norm[out_i++] = ' ';
                last_space = true;
            }
        } else {
            norm[out_i++] = c;
            last_space = false;
        }
    }
    while (out_i > 0 && norm[out_i - 1] == ' ') out_i--;
    norm[out_i] = '\0';

    static const char* clauses[] = {
        " FROM ", " WHERE ", " LEFT JOIN ", " INNER JOIN ", " JOIN ",
        " GROUP BY ", " ORDER BY ", " LIMIT ", " VALUES ", " SET "
    };
    char* formatted = (char*)malloc(out_i * 2 + 64);
    if (!formatted) {
        free(norm);
        free(raw);
        return;
    }
    size_t f_i = 0;
    in_str = false;
    for (size_t i = 0; i < out_i; ) {
        if (norm[i] == '\'') {
            in_str = !in_str;
            formatted[f_i++] = norm[i++];
            continue;
        }
        bool matched_clause = false;
        if (!in_str && norm[i] == ' ') {
            for (size_t k = 0; k < sizeof(clauses) / sizeof(clauses[0]); k++) {
                size_t clen = strlen(clauses[k]);
                bool match = true;
                for (size_t m = 0; m < clen; m++) {
                    if (!norm[i + m] ||
                        toupper((unsigned char)norm[i + m]) != (unsigned char)clauses[k][m]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    formatted[f_i++] = '\n';
                    for (size_t m = 1; m < clen; m++) {
                        formatted[f_i++] = clauses[k][m];
                    }
                    i += clen;
                    matched_clause = true;
                    break;
                }
            }
        }
        if (!matched_clause) {
            formatted[f_i++] = norm[i++];
        }
    }
    formatted[f_i] = '\0';

    iris_load_text(ed, formatted);
    ed->modified = true;
    snprintf(ed->status_msg, sizeof(ed->status_msg), "Justified SQL buffer (%d line%s)",
             ed->line_count, ed->line_count == 1 ? "" : "s");
    free(formatted);
    free(norm);
    free(raw);
}

bool iris_feed_key(IrisEditor* ed, int key) {
    if (!ed) return false;

    if (ed->show_help && key != IRIS_KEY_CTRL_G && key != IRIS_KEY_CTRL_X) {
        ed->show_help = false;
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Exited Help");
        return true;
    }

    if (key != IRIS_KEY_CTRL_K) {
        ed->last_was_cut = false;
    }

    switch (key) {
    case IRIS_KEY_CTRL_X:
        ed->execute_on_exit = false;
        return false;
    case IRIS_KEY_CTRL_T:
        ed->execute_on_exit = true;
        return false;
    case IRIS_KEY_CTRL_G:
        ed->show_help = !ed->show_help;
        snprintf(ed->status_msg, sizeof(ed->status_msg),
                 ed->show_help ? "Viewing IRIS Help (Press ^G or any key to return)" : "Exited Help");
        return true;
    case IRIS_KEY_CTRL_O:
        iris_save_file(ed, ed->filename);
        return true;
    case IRIS_KEY_CTRL_R:
        iris_insert_file(ed, ed->filename);
        return true;
    case IRIS_KEY_CTRL_W:
        iris_search(ed, NULL);
        return true;
    case IRIS_KEY_CTRL_J:
        iris_justify_sql(ed);
        return true;
    case IRIS_KEY_CTRL_C: {
        int line_len = (int)strlen(ed->lines[ed->cur_row]);
        int pct = (ed->line_count > 0) ? ((ed->cur_row + 1) * 100 / ed->line_count) : 100;
        snprintf(ed->status_msg, sizeof(ed->status_msg),
                 "line %d/%d (%d%%), col %d/%d",
                 ed->cur_row + 1, ed->line_count, pct,
                 ed->cur_col + 1, line_len + 1);
        return true;
    }
    case IRIS_KEY_CTRL_K: {
        if (!ed->last_was_cut) {
            iris_clear_cut_buffer(ed);
            ed->last_was_cut = true;
        }
        if (ed->cut_count < IRIS_CUT_MAX) {
            ed->cut_buffer[ed->cut_count++] = iris_strdup(ed->lines[ed->cur_row]);
        }
        iris_delete_line_at(ed, ed->cur_row);
        if (ed->cur_row >= ed->line_count) ed->cur_row = ed->line_count - 1;
        ed->cur_col = 0;
        ed->modified = true;
        iris_adjust_viewport(ed);
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Cut %d line%s",
                 ed->cut_count, ed->cut_count == 1 ? "" : "s");
        return true;
    }
    case IRIS_KEY_CTRL_U: {
        if (ed->cut_count == 0) {
            snprintf(ed->status_msg, sizeof(ed->status_msg), "Cutbuffer is empty");
            return true;
        }
        for (int i = 0; i < ed->cut_count; i++) {
            iris_insert_line_at(ed, ed->cur_row + i, ed->cut_buffer[i]);
        }
        ed->cur_row += ed->cut_count;
        if (ed->cur_row >= ed->line_count) ed->cur_row = ed->line_count - 1;
        ed->cur_col = 0;
        ed->modified = true;
        iris_adjust_viewport(ed);
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Uncut %d line%s",
                 ed->cut_count, ed->cut_count == 1 ? "" : "s");
        return true;
    }
    case IRIS_KEY_UP:
        if (ed->cur_row > 0) ed->cur_row--;
        iris_adjust_viewport(ed);
        return true;
    case IRIS_KEY_DOWN:
        if (ed->cur_row + 1 < ed->line_count) ed->cur_row++;
        iris_adjust_viewport(ed);
        return true;
    case IRIS_KEY_LEFT:
        if (ed->cur_col > 0) {
            ed->cur_col--;
        } else if (ed->cur_row > 0) {
            ed->cur_row--;
            ed->cur_col = (int)strlen(ed->lines[ed->cur_row]);
        }
        iris_adjust_viewport(ed);
        return true;
    case IRIS_KEY_RIGHT: {
        int len = (int)strlen(ed->lines[ed->cur_row]);
        if (ed->cur_col < len) {
            ed->cur_col++;
        } else if (ed->cur_row + 1 < ed->line_count) {
            ed->cur_row++;
            ed->cur_col = 0;
        }
        iris_adjust_viewport(ed);
        return true;
    }
    case IRIS_KEY_HOME:
        ed->cur_col = 0;
        iris_adjust_viewport(ed);
        return true;
    case IRIS_KEY_END:
        ed->cur_col = (int)strlen(ed->lines[ed->cur_row]);
        iris_adjust_viewport(ed);
        return true;
    case IRIS_KEY_CTRL_Y:
    case IRIS_KEY_PAGE_UP: {
        int step = ed->screen_rows - 4;
        if (step < 1) step = 1;
        ed->cur_row -= step;
        if (ed->cur_row < 0) ed->cur_row = 0;
        iris_adjust_viewport(ed);
        return true;
    }
    case IRIS_KEY_CTRL_V:
    case IRIS_KEY_PAGE_DOWN: {
        int step = ed->screen_rows - 4;
        if (step < 1) step = 1;
        ed->cur_row += step;
        if (ed->cur_row >= ed->line_count) ed->cur_row = ed->line_count - 1;
        iris_adjust_viewport(ed);
        return true;
    }
    case IRIS_KEY_ENTER: {
        char* cur = ed->lines[ed->cur_row];
        int len = (int)strlen(cur);
        if (ed->cur_col > len) ed->cur_col = len;
        if (!iris_insert_line_at(ed, ed->cur_row + 1, cur + ed->cur_col)) return true;
        ed->lines[ed->cur_row][ed->cur_col] = '\0';
        ed->cur_row++;
        ed->cur_col = 0;
        ed->modified = true;
        iris_adjust_viewport(ed);
        return true;
    }
    case IRIS_KEY_BACKSPACE:
    case 127: {
        if (ed->cur_col > 0) {
            char* cur = ed->lines[ed->cur_row];
            size_t len = strlen(cur);
            memmove(cur + ed->cur_col - 1, cur + ed->cur_col, len - (size_t)ed->cur_col + 1);
            ed->cur_col--;
            ed->modified = true;
        } else if (ed->cur_row > 0) {
            char* prev = ed->lines[ed->cur_row - 1];
            char* cur = ed->lines[ed->cur_row];
            size_t prev_len = strlen(prev);
            size_t cur_len = strlen(cur);
            if (prev_len + cur_len < IRIS_LINE_CAP) {
                char* merged = (char*)malloc(prev_len + cur_len + 1);
                if (merged) {
                    memcpy(merged, prev, prev_len);
                    memcpy(merged + prev_len, cur, cur_len + 1);
                    free(ed->lines[ed->cur_row - 1]);
                    ed->lines[ed->cur_row - 1] = merged;
                    iris_delete_line_at(ed, ed->cur_row);
                    ed->cur_row--;
                    ed->cur_col = (int)prev_len;
                    ed->modified = true;
                }
            }
        }
        iris_adjust_viewport(ed);
        return true;
    }
    case IRIS_KEY_DELETE: {
        char* cur = ed->lines[ed->cur_row];
        int len = (int)strlen(cur);
        if (ed->cur_col < len) {
            memmove(cur + ed->cur_col, cur + ed->cur_col + 1, (size_t)(len - ed->cur_col));
            ed->modified = true;
        } else if (ed->cur_row + 1 < ed->line_count) {
            char* next = ed->lines[ed->cur_row + 1];
            size_t next_len = strlen(next);
            if ((size_t)len + next_len < IRIS_LINE_CAP) {
                char* merged = (char*)malloc((size_t)len + next_len + 1);
                if (merged) {
                    memcpy(merged, cur, (size_t)len);
                    memcpy(merged + len, next, next_len + 1);
                    free(ed->lines[ed->cur_row]);
                    ed->lines[ed->cur_row] = merged;
                    iris_delete_line_at(ed, ed->cur_row + 1);
                    ed->modified = true;
                }
            }
        }
        iris_adjust_viewport(ed);
        return true;
    }
    case IRIS_KEY_TAB: {
        for (int s = 0; s < 4; s++) {
            iris_feed_key(ed, ' ');
        }
        return true;
    }
    default:
        if (key >= 32 && key <= 126) {
            char* cur = ed->lines[ed->cur_row];
            size_t len = strlen(cur);
            if (len + 1 < IRIS_LINE_CAP) {
                char* next_buf = (char*)malloc(len + 2);
                if (next_buf) {
                    memcpy(next_buf, cur, (size_t)ed->cur_col);
                    next_buf[ed->cur_col] = (char)key;
                    memcpy(next_buf + ed->cur_col + 1, cur + ed->cur_col, len - (size_t)ed->cur_col + 1);
                    free(ed->lines[ed->cur_row]);
                    ed->lines[ed->cur_row] = next_buf;
                    ed->cur_col++;
                    ed->modified = true;
                    iris_adjust_viewport(ed);
                }
            }
        }
        return true;
    }
}

int iris_get_line_count(const IrisEditor* ed) {
    return ed ? ed->line_count : 0;
}

const char* iris_get_line(const IrisEditor* ed, int line_idx) {
    if (!ed || line_idx < 0 || line_idx >= ed->line_count) return NULL;
    return ed->lines[line_idx];
}

void iris_get_cursor(const IrisEditor* ed, int* out_row, int* out_col) {
    if (!ed) return;
    if (out_row) *out_row = ed->cur_row;
    if (out_col) *out_col = ed->cur_col;
}

const char* iris_get_status(const IrisEditor* ed) {
    return ed ? ed->status_msg : "";
}

static void iris_buf_append(char* buf, size_t cap, size_t* pos, const char* s) {
    if (!buf || !s || *pos >= cap) return;
    size_t len = strlen(s);
    if (*pos + len >= cap) len = cap - *pos - 1;
    memcpy(buf + *pos, s, len);
    *pos += len;
    buf[*pos] = '\0';
}

static void iris_render_internal(const IrisEditor* ed, char* buf, size_t cap, bool use_ansi) {
    if (!ed || !buf || cap == 0) return;
    size_t pos = 0;
    buf[0] = '\0';

    if (use_ansi) {
        iris_buf_append(buf, cap, &pos, "\033[?25l\033[H\033[7m");
    }

    /* Top Header Bar (Nano-style inverted banner) */
    char header[512];
    const char* mod_tag = ed->modified ? "Modified" : "        ";
    int cols = ed->screen_cols > 40 ? ed->screen_cols : 80;
    if (cols > 250) cols = 250;
    snprintf(header, sizeof(header),
             "  IRIS 1.0 - PC/OpenData Editor   File: %-22.22s %10s",
             ed->filename, mod_tag);
    size_t hlen = strlen(header);
    while (hlen < (size_t)cols && hlen + 1 < sizeof(header)) {
        header[hlen++] = ' ';
    }
    header[hlen] = '\0';
    iris_buf_append(buf, cap, &pos, header);
    if (use_ansi) {
        iris_buf_append(buf, cap, &pos, "\033[0m\033[K");
    }
    iris_buf_append(buf, cap, &pos, "\n");

    int text_rows = ed->screen_rows - 4;
    if (text_rows < 1) text_rows = 1;

    static const char* help_lines[] = {
        "  IRIS 1.0 (GNU Nano-Styled SQL & Script Editor) Help Summary",
        "  -----------------------------------------------------------",
        "  ^G  (Ctrl+G)  Display this help text",
        "  ^O  (Ctrl+O)  WriteOut current buffer to disk",
        "  ^R  (Ctrl+R)  Read File into current cursor position",
        "  ^Y  (Ctrl+Y)  Previous Page (Page Up)",
        "  ^V  (Ctrl+V)  Next Page (Page Down)",
        "  ^K  (Ctrl+K)  Cut current line into cutbuffer",
        "  ^U  (Ctrl+U)  UnCut (paste) lines from cutbuffer",
        "  ^W  (Ctrl+W)  Where Is (search forward in buffer)",
        "  ^J  (Ctrl+J)  Justify / Format SQL clauses cleanly",
        "  ^C  (Ctrl+C)  Report current line and column cursor position",
        "  ^T  (Ctrl+T)  Return buffer to OpenData SQL> and execute",
        "  ^X  (Ctrl+X)  Exit IRIS editor"
    };
    int help_count = (int)(sizeof(help_lines) / sizeof(help_lines[0]));

    for (int r = 0; r < text_rows; r++) {
        if (ed->show_help) {
            if (r < help_count) {
                iris_buf_append(buf, cap, &pos, help_lines[r]);
            }
        } else {
            int file_row = ed->row_offset + r;
            if (file_row < ed->line_count) {
                const char* line = ed->lines[file_row];
                int line_len = (int)strlen(line);
                if (ed->col_offset < line_len) {
                    char slice[256];
                    int avail = line_len - ed->col_offset;
                    if (avail > cols) avail = cols;
                    if (avail >= (int)sizeof(slice)) avail = (int)sizeof(slice) - 1;
                    /* Strip control characters so a malicious file cannot inject
                       ANSI escape sequences into the user's terminal. */
                    int out = 0;
                    for (int k = 0; k < avail; k++) {
                        unsigned char ch = (unsigned char)line[ed->col_offset + k];
                        if (ch < 32 && ch != '\t') continue;
                        if (ch == 127) continue;
                        slice[out++] = (char)ch;
                    }
                    slice[out] = '\0';
                    iris_buf_append(buf, cap, &pos, slice);
                }
            }
        }
        if (use_ansi) {
            iris_buf_append(buf, cap, &pos, "\033[K");
        }
        iris_buf_append(buf, cap, &pos, "\n");
    }

    /* Status bar */
    char status_line[300];
    snprintf(status_line, sizeof(status_line), "  [ %s ]", ed->status_msg);
    iris_buf_append(buf, cap, &pos, status_line);
    if (use_ansi) {
        iris_buf_append(buf, cap, &pos, "\033[K");
    }
    iris_buf_append(buf, cap, &pos, "\n");

    /* 2-Row GNU Nano Shortcut Footer */
    if (use_ansi) {
        iris_buf_append(buf, cap, &pos,
            "\033[7m^G\033[0m Get Help  \033[7m^O\033[0m WriteOut  \033[7m^R\033[0m Read File \033[7m^Y\033[0m Prev Page \033[7m^K\033[0m Cut Text  \033[7m^C\033[0m Cur Pos\033[K\n"
            "\033[7m^X\033[0m Exit/Exec \033[7m^J\033[0m Justify   \033[7m^W\033[0m Where Is  \033[7m^V\033[0m Next Page \033[7m^U\033[0m UnCutTxt  \033[7m^T\033[0m To SQL\033[K");
        int vis_row = (ed->cur_row - ed->row_offset) + 2;
        int vis_col = (ed->cur_col - ed->col_offset) + 1;
        if (vis_row < 2) vis_row = 2;
        if (vis_col < 1) vis_col = 1;
        char cup[64];
        snprintf(cup, sizeof(cup), "\033[%d;%dH\033[?25h", vis_row, vis_col);
        iris_buf_append(buf, cap, &pos, cup);
    } else {
        iris_buf_append(buf, cap, &pos,
            "^G Get Help   ^O WriteOut   ^R Read File  ^Y Prev Page  ^K Cut Text   ^C Cur Pos\n"
            "^X Exit/Exec  ^J Justify    ^W Where Is   ^V Next Page  ^U UnCut Text ^T To SQL\n");
    }
}

void iris_render_to_buffer(const IrisEditor* ed, char* out_buf, size_t out_cap) {
    iris_render_internal(ed, out_buf, out_cap, false);
}

static int iris_read_tty_key(void) {
#ifdef _WIN32
    int ch = _getch();
    if (ch == 0 || ch == 0xE0) {
        int ext = _getch();
        switch (ext) {
        case 72: return IRIS_KEY_UP;
        case 80: return IRIS_KEY_DOWN;
        case 75: return IRIS_KEY_LEFT;
        case 77: return IRIS_KEY_RIGHT;
        case 71: return IRIS_KEY_HOME;
        case 79: return IRIS_KEY_END;
        case 73: return IRIS_KEY_PAGE_UP;
        case 81: return IRIS_KEY_PAGE_DOWN;
        case 83: return IRIS_KEY_DELETE;
        default: return 0;
        }
    }
    return ch;
#else
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) <= 0) return IRIS_KEY_CTRL_X;
    if (c == 27) {
        unsigned char seq[3] = {0};
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return 27;
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) <= 0) return 27;
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1': return IRIS_KEY_HOME;
                    case '3': return IRIS_KEY_DELETE;
                    case '4': return IRIS_KEY_END;
                    case '5': return IRIS_KEY_PAGE_UP;
                    case '6': return IRIS_KEY_PAGE_DOWN;
                    case '7': return IRIS_KEY_HOME;
                    case '8': return IRIS_KEY_END;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return IRIS_KEY_UP;
                case 'B': return IRIS_KEY_DOWN;
                case 'C': return IRIS_KEY_RIGHT;
                case 'D': return IRIS_KEY_LEFT;
                case 'H': return IRIS_KEY_HOME;
                case 'F': return IRIS_KEY_END;
                }
            }
        }
        return 27;
    }
    return (int)c;
#endif
}

#ifdef _WIN32
static void win32_clear_entire_console(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == NULL) return;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;
    DWORD cell_count = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    DWORD count = 0;
    COORD home;
    home.X = 0;
    home.Y = 0;
    FillConsoleOutputCharacterA(hOut, ' ', cell_count, home, &count);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, cell_count, home, &count);
    SetConsoleCursorPosition(hOut, home);
}
#endif

static void iris_update_terminal_size(IrisEditor* ed) {
    if (!ed) return;
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
            int w = (int)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
            int h = (int)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
            if (w >= 20) ed->screen_cols = w;
            if (h >= 6) ed->screen_rows = h;
        }
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col >= 20) ed->screen_cols = (int)ws.ws_col;
        if (ws.ws_row >= 6) ed->screen_rows = (int)ws.ws_row;
    }
#endif
}

bool iris_run_interactive(const char* filepath, char* sql_buffer, size_t sql_buffer_cap, bool* out_execute_sql) {
    if (out_execute_sql) *out_execute_sql = false;
    IrisEditor* ed = iris_create();
    if (!ed) return false;

    if (filepath && filepath[0]) {
        iris_load_file(ed, filepath);
    } else if (sql_buffer && sql_buffer[0]) {
        iris_load_text(ed, sql_buffer);
    }

#ifdef _WIN32
    DWORD input_mode = 0, output_mode = 0;
    bool is_tty = _isatty(_fileno(stdin)) != 0 &&
        GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode) &&
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode);
#else
    bool is_tty = isatty(STDIN_FILENO) != 0;
#endif

    if (!is_tty) {
        char screen[16384];
        iris_render_internal(ed, screen, sizeof(screen), false);
        fputs(screen, stdout);
        fflush(stdout);
        if (sql_buffer && sql_buffer_cap > 0) {
            iris_export_text(ed, sql_buffer, sql_buffer_cap);
        }
        iris_destroy(ed);
        return true;
    }

    iris_update_terminal_size(ed);

#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD orig_out_mode = 0, orig_in_mode = 0;
    bool has_orig_modes = false;
    bool vt_enabled = false;
    if (hOut != INVALID_HANDLE_VALUE && hIn != INVALID_HANDLE_VALUE &&
        GetConsoleMode(hOut, &orig_out_mode) && GetConsoleMode(hIn, &orig_in_mode)) {
        has_orig_modes = true;
        DWORD out_mode = orig_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (SetConsoleMode(hOut, out_mode)) {
            vt_enabled = true;
        }
        DWORD in_mode = orig_in_mode | ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(hIn, in_mode);
    }
#else
    struct termios orig_term, raw_term;
    bool raw_enabled = (tcgetattr(STDIN_FILENO, &orig_term) == 0);
    if (raw_enabled) {
        raw_term = orig_term;
        raw_term.c_iflag &= (tcflag_t)~(IXON | ICRNL);
        raw_term.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);
    }
#endif

    /* Enter Alternate Screen Buffer, hide cursor, clear, and home */
    fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout);
    fflush(stdout);

    char* screen = (char*)malloc(32768);
    bool keep_running = true;
    while (keep_running) {
        iris_update_terminal_size(ed);
        if (screen) {
            iris_render_internal(ed, screen, 32768, true);
            fputs(screen, stdout);
            fflush(stdout);
        }
        int key = iris_read_tty_key();
        keep_running = iris_feed_key(ed, key);
    }
    free(screen);

    /* Clear alternate buffer, restore Normal Screen Buffer, and show cursor */
    fputs("\033[0m\033[2J\033[H\033[?1049l\033[?25h", stdout);
    fflush(stdout);

#ifdef _WIN32
    if (has_orig_modes) {
        SetConsoleMode(hOut, orig_out_mode);
        SetConsoleMode(hIn, orig_in_mode);
    }
    if (!vt_enabled) {
        win32_clear_entire_console();
    }
#else
    if (raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
    }
#endif

    if (filepath && filepath[0] && ed->modified) {
        iris_save_file(ed, filepath);
    }
    if (out_execute_sql) {
        *out_execute_sql = (ed->execute_on_exit || (!filepath || !filepath[0]));
    }
    if (sql_buffer && sql_buffer_cap > 0) {
        iris_export_text(ed, sql_buffer, sql_buffer_cap);
    }
    iris_destroy(ed);
    return true;
}
