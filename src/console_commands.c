#include "database_engine.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

#define MAX_HISTORY_ENTRIES 100
static char g_history[MAX_HISTORY_ENTRIES][512];
static int g_history_count = 0;
static char g_last_sql[MAX_QUERY_LEN] = "SELECT * FROM DUAL;";

static void shell_puts(const char* s);
static void shell_printf(const char* fmt, ...);

typedef enum {
    CONFIRM_ALLOW,
    CONFIRM_CANCEL,
    CONFIRM_DRYRUN,
    CONFIRM_ERROR
} ConfirmationResult;

static bool shell_has_interactive_input(void) {
    return g_interactive_shell;
}

void format_human_size(uint64_t bytes, char* buf, size_t buf_size);

void show_destructive_dry_run(const char* action, const char* target_name, const char* extra_info) {
    shell_puts("\n================================================================================");
    shell_puts("                    PC/OpenData Dry-Run Impact Analysis");
    shell_puts("================================================================================");
    shell_printf("  Operation:           %s\n", action ? action : "DESTRUCTIVE MUTATION");
    shell_printf("  Target Object:       %s\n", target_name && *target_name ? target_name : "(unspecified)");
    if (g_path[0]) {
        shell_printf("  Database File:       %s (Snapshot Generation: %llu)\n", g_path, (unsigned long long)g_generation);
    } else {
        shell_puts("  Database File:       (In-Memory Transient Session)");
    }
    
    if (action && (!strcmp(action, "DROP TABLE") || !strcmp(action, "TRUNCATE TABLE"))) {
        int idx = (g_db && target_name) ? find_table_index(g_db, target_name) : -1;
        if (idx >= 0) {
            const Table* tbl = &g_db->tables[idx];
            shell_printf("  Storage Engine:      %s\n", tbl->engine_type == ENGINE_DISK ? "DISK (B-Tree)" : "MEMORY");
            shell_printf("  Current Rows:        %d row(s)\n", tbl->row_count);
            shell_printf("  Columns (%d):         ", tbl->col_count);
            for (int c = 0; c < tbl->col_count; c++) {
                shell_printf("%s%s", tbl->columns[c].name, c + 1 < tbl->col_count ? ", " : "\n");
            }
            int idx_count = 0;
            if (g_db) {
                for (int i = 0; i < g_db->index_count; i++) {
                    if (!strcmp(g_db->indexes[i].table_name, target_name)) idx_count++;
                }
            }
            shell_printf("  Indexes to %s: %d index(es)\n",
                         !strcmp(action, "DROP TABLE") ? "Drop" : "Clear", idx_count);
            if (idx_count > 0 && g_db) {
                for (int i = 0; i < g_db->index_count; i++) {
                    if (!strcmp(g_db->indexes[i].table_name, target_name)) {
                        shell_printf("                         - %s\n", g_db->indexes[i].name);
                    }
                }
            }
            int fk_refs = 0;
            if (g_db) {
                for (int t = 0; t < g_db->table_count; t++) {
                    if (t == idx) continue;
                    const Table* other = &g_db->tables[t];
                    for (int c = 0; c < other->col_count; c++) {
                        if (!strcmp(other->columns[c].ref_table, target_name)) {
                            if (fk_refs == 0) shell_puts("  Foreign Key Impact:  Dependent tables referencing this table:");
                            shell_printf("                         - %s.%s -> %s.%s\n",
                                         other->name, other->columns[c].name, target_name, other->columns[c].ref_column);
                            fk_refs++;
                        }
                    }
                }
            }
            if (fk_refs == 0) {
                shell_puts("  Foreign Key Impact:  No foreign key dependencies found.");
            }
            size_t est_bytes = (size_t)tbl->row_count * (size_t)(tbl->col_count * 20);
            char sz_buf[32];
            format_human_size((uint64_t)est_bytes, sz_buf, sizeof(sz_buf));
            shell_printf("  Estimated Storage:   ~%s of row data\n", sz_buf);
            if (!strcmp(action, "DROP TABLE")) {
                shell_puts("  Schema Persistence:  Table schema and data will be permanently unlinked.");
            } else {
                shell_puts("  Schema Persistence:  Table schema and column definitions will be preserved.");
            }
        } else {
            shell_printf("  Notice:              Table '%s' does not exist in current database.\n", target_name ? target_name : "");
        }
    } else if (action && (!strcmp(action, "DELETE FROM") || !strcmp(action, "UPDATE"))) {
        int idx = (g_db && target_name) ? find_table_index(g_db, target_name) : -1;
        if (idx >= 0) {
            const Table* tbl = &g_db->tables[idx];
            shell_printf("  Target Table:        %s (%d total rows)\n", tbl->name, tbl->row_count);
            if (extra_info && *extra_info) {
                shell_printf("  Filter Condition:    %s\n", extra_info);
            } else {
                shell_puts("  Filter Condition:    None (UNCONDITIONAL - affects ALL rows!)");
            }
            if (!strcmp(action, "DELETE FROM")) {
                shell_printf("  Impact:              %s will be deleted.\n",
                             (extra_info && *extra_info) ? "Matching rows" : "ALL rows");
            } else {
                shell_printf("  Impact:              %s will be modified.\n",
                             (extra_info && *extra_info) ? "Matching rows" : "ALL rows");
            }
        }
    } else if (action && !strcmp(action, "DROP DATABASE")) {
        shell_printf("  Database Path:       %s\n", target_name ? target_name : "");
        if (target_name && snapshot_path_exists(target_name)) {
            shell_puts("  File Status:         File exists on disk.");
            shell_puts("  Impact:              Database snapshot file and WAL will be deleted permanently.");
        } else {
            shell_puts("  File Status:         File does not exist or cannot be accessed.");
        }
    } else if (action && !strcmp(action, "DROP USER")) {
        shell_printf("  User Account:        %s\n", target_name ? target_name : "");
        shell_puts("  Impact:              User credentials, permissions, and active sessions revoked.");
    }
    if (extra_info && *extra_info && action && strcmp(action, "DELETE FROM") && strcmp(action, "UPDATE")) {
        shell_printf("  Details:             %s\n", extra_info);
    }
    shell_puts("--------------------------------------------------------------------------------");
    shell_puts("RESULT: Dry-run test complete. Database file has NOT been modified.");
    shell_puts("================================================================================\n");
}

static ConfirmationResult confirm_destructive_action(const char* action,
                                                      const char* target,
                                                      const char* explanation,
                                                      const char* expected) {
    if (!shell_has_interactive_input() || !g_confirm_destructive) return CONFIRM_ALLOW;

    shell_puts("\n--------------------------------------------------------------------------------");
    shell_printf("WARNING: Destructive Operation Detected!\n");
    shell_printf("  Operation: %s\n", action ? action : "DESTRUCTIVE STATEMENT");
    if (target && *target) shell_printf("  Target:    %s\n", target);
    if (explanation && *explanation) shell_printf("  Details:   %s\n", explanation);
    shell_puts("--------------------------------------------------------------------------------");
    shell_printf("Proceed? Type 'yes' (or 'y') to continue, 'no' (or Enter) to cancel,\n"
                 "         or 'dry-run' (or 'test') to preview impact [y/N/test]: ");
    fflush(stdout);

    char response[96];
    if (!fgets(response, sizeof(response), stdin)) {
        shell_puts("Cancelled; database file has not been modified.");
        return ferror(stdin) ? CONFIRM_ERROR : CONFIRM_CANCEL;
    }
    size_t length = strlen(response);
    while (length > 0 && (response[length - 1] == '\r' || response[length - 1] == '\n'))
        response[--length] = '\0';
    while (length > 0 && is_sql_whitespace(response[length - 1]))
        response[--length] = '\0';
    const char* rptr = response;
    while (is_sql_whitespace(*rptr)) rptr++;

    if (!*rptr || !strcmp(rptr, "n") || !strcmp(rptr, "no") || !strcmp(rptr, "N") ||
        !strcmp(rptr, "NO") || !strcmp(rptr, "cancel") || !strcmp(rptr, "deny")) {
        shell_puts("Cancelled; database file has not been modified.");
        return CONFIRM_CANCEL;
    }

    if (!strcmp(rptr, "test") || !strcmp(rptr, "TEST") ||
        !strcmp(rptr, "dry-run") || !strcmp(rptr, "dryrun") ||
        !strcmp(rptr, "d") || !strcmp(rptr, "D") || !strcmp(rptr, "explain")) {
        return CONFIRM_DRYRUN;
    }

    if (!strcmp(rptr, "y") || !strcmp(rptr, "yes") || !strcmp(rptr, "Y") ||
        !strcmp(rptr, "YES") || (expected && !strcmp(rptr, expected))) {
        return CONFIRM_ALLOW;
    }

    shell_puts("Cancelled; unrecognized confirmation response.");
    return CONFIRM_CANCEL;
}

static bool handle_confirmation_with_dryrun(ConfirmationResult* conf,
                                             const char* action,
                                             const char* target,
                                             const char* extra) {
    if (*conf == CONFIRM_DRYRUN) {
        show_destructive_dry_run(action, target, extra);
        shell_printf("Would you like to execute '%s %s' now? [y/N]: ", action ? action : "", target ? target : "");
        fflush(stdout);
        char final_resp[32] = {0};
        if (fgets(final_resp, sizeof(final_resp), stdin)) {
            size_t flen = strlen(final_resp);
            while (flen > 0 && (final_resp[flen - 1] == '\r' || final_resp[flen - 1] == '\n')) final_resp[--flen] = '\0';
            const char* fr = final_resp;
            while (is_sql_whitespace(*fr)) fr++;
            if (!strcmp(fr, "y") || !strcmp(fr, "yes") || !strcmp(fr, "Y") || !strcmp(fr, "YES")) {
                *conf = CONFIRM_ALLOW;
                return true;
            }
        }
        shell_puts("Cancelled; database file has not been modified.");
        *conf = CONFIRM_CANCEL;
        return true;
    }
    return false;
}

static bool allow_or_report_confirmation(ConfirmationResult result) {
    if (result == CONFIRM_ERROR)
        return report_database_error("unable to read destructive-operation confirmation");
    return result == CONFIRM_ALLOW;
}

int edit_distance_capped(const char* a, const char* b, int cap) {
    if (!a || !b || cap < 0) return cap < 0 ? 0 : cap + 1;
    size_t la = strlen(a), lb = strlen(b);
    if (la >= MAX_NAME_LEN || lb >= MAX_NAME_LEN) return cap + 1;
    if (la > lb) {
        const char* ts = a; a = b; b = ts;
        size_t tl = la; la = lb; lb = tl;
    }
    if (lb - la > (size_t)cap) return cap + 1;
    int prev[MAX_NAME_LEN + 1], cur[MAX_NAME_LEN + 1];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        int row_min = cur[0];
        for (size_t j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
            if (prev[j - 1] + cost < best) best = prev[j - 1] + cost;
            cur[j] = best;
            if (best < row_min) row_min = best;
        }
        if (row_min > cap) return cap + 1;
        memcpy(prev, cur, (lb + 1) * sizeof(int));
    }
    return prev[lb];
}

static const char* suggest_word(const char* word,
                                const char* const* candidates, size_t count) {
    if (!word[0]) return NULL;
    int threshold = strlen(word) >= 6 ? 3 : 2;
    const char* best = NULL;
    int best_dist = threshold + 1;
    for (size_t i = 0; i < count; i++) {
        int d = edit_distance_capped(word, candidates[i], threshold);
        if (d < best_dist) {
            best_dist = d;
            best = candidates[i];
        }
    }
    return best;
}

static const char* suggest_command_word(const char* word) {
    static const char* const commands[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER",
        "TRUNCATE", "WITH", "EXPLAIN", "ANALYZE", "SHOW", "DESCRIBE", "DESC",
        "BROWSE", "HELP", "GUIDE", "STATUS", "IMPORT", "EXPORT", "COMMIT",
        "ROLLBACK", "BEGIN", "SAVEPOINT", "RELEASE", "CONNECT", "USE", "OPEN",
        "DISCONNECT", "EXIT", "QUIT", "BACKUP", "RESTART", "CHECK", "VACUUM",
        "COMPACT", "DUMP", "DIFF", "BENCHMARK", "WATCH", "HISTORY", "SPOOL",
        "SOURCE", "SET", "CLEAR", "CLS", "CANCEL", "PING", "LOAD", "SUPPORT",
        "CONNECTION", "KILL", "IRIS", "EDIT", "VERIFY", "SELFTEST", "SELF_TEST"
    };
    return suggest_word(word, commands, sizeof(commands) / sizeof(commands[0]));
}

static const char* suggest_show_word(const char* word) {
    static const char* const targets[] = {
        "TABLES", "DATABASES", "USERS", "USER", "SESSIONS", "PROCESSLIST",
        "SCHEMA", "CLUSTER", "ENGINE", "CREATE", "STATS", "CONSTRAINTS", "METRICS"
    };
    return suggest_word(word, targets, sizeof(targets) / sizeof(targets[0]));
}

static void shell_puts(const char* s) {
    if (!g_silent_mode) puts(s);
    if (g_spool_file) {
        fputs(s, g_spool_file);
        fputc('\n', g_spool_file);
        fflush(g_spool_file);
    }
}

static void shell_printf(const char* fmt, ...) {
    if (!g_silent_mode) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    if (g_spool_file) {
        va_list args2;
        va_start(args2, fmt);
        vfprintf(g_spool_file, fmt, args2);
        va_end(args2);
        fflush(g_spool_file);
    }
}


bool read_masked_password(const char* prompt, char* buffer, size_t max_len) {
    if (!prompt || !buffer || max_len == 0) return false;
    buffer[0] = '\0';
    fputs(prompt, stdout);
    fflush(stdout);
#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hStdin, &mode)) {
        SetConsoleMode(hStdin, mode & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
    }
    size_t idx = 0;
    for (;;) {
        DWORD read_bytes = 0;
        char ch = 0;
        if (!ReadFile(hStdin, &ch, 1, &read_bytes, NULL) || read_bytes == 0) break;
        if (ch == '\r' || ch == '\n') {
            break;
        } else if (ch == '\b' || ch == 127) {
            if (idx > 0) {
                idx--;
                buffer[idx] = '\0';
                fputs("\b \b", stdout);
                fflush(stdout);
            }
        } else if (ch == 3) {
            if (mode) SetConsoleMode(hStdin, mode);
            fputs("^C\n", stdout);
            fflush(stdout);
            buffer[0] = '\0';
            return false;
        } else if ((unsigned char)ch >= 32 && idx + 1 < max_len) {
            buffer[idx++] = ch;
            buffer[idx] = '\0';
            fputc('*', stdout);
            fflush(stdout);
        }
    }
    if (mode) SetConsoleMode(hStdin, mode);
    fputc('\n', stdout);
    fflush(stdout);
    return true;
#else
    if (fgets(buffer, (int)max_len, stdin)) {
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) buffer[--len] = '\0';
        return true;
    }
    return false;
#endif
}



const char* suggest_table_name(const Database* db, const char* typo) {
    if (!db || !typo || !*typo) return NULL;
    int best_dist = 4;
    const char* best_match = NULL;
    for (int t = 0; t < db->table_count; t++) {
        int d = edit_distance_capped(typo, db->tables[t].name, 3);
        if (d < best_dist) {
            best_dist = d;
            best_match = db->tables[t].name;
        }
    }
    return best_match;
}

const char* suggest_column_name(const Table* table, const char* typo) {
    if (!table || !typo || !*typo) return NULL;
    int best_dist = 4;
    const char* best_match = NULL;
    for (int c = 0; c < table->col_count; c++) {
        int d = edit_distance_capped(typo, table->columns[c].name, 3);
        if (d < best_dist) {
            best_dist = d;
            best_match = table->columns[c].name;
        }
    }
    return best_match;
}

bool report_table_not_found(const Database* db, const char* typo) {
    const char* suggestion = suggest_table_name(db, typo);
    if (suggestion) {
        return report_database_error("table does not exist: %s. Did you mean '%s'?", typo, suggestion);
    }
    return report_database_error("table does not exist: %s", typo);
}

bool report_column_not_found(const Table* table, const char* typo) {
    const char* suggestion = suggest_column_name(table, typo);
    if (suggestion) {
        return report_database_error("column '%s' not found in table '%s'. Did you mean '%s'?",
                              typo, table ? table->name : "", suggestion);
    }
    return report_database_error("column '%s' not found%s%s",
                              typo, table ? " in table " : "", table ? table->name : "");
}

void format_human_size(uint64_t bytes, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (bytes < 1024) {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        snprintf(buf, buf_size, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, buf_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, buf_size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

void format_relative_timestamp(time_t t, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (t == 0) { snprintf(buf, buf_size, "never"); return; }
    time_t now = time(NULL);
    long diff = (long)difftime(now, t);
    if (diff < 0) diff = 0;
    if (diff < 60) snprintf(buf, buf_size, "%ld sec%s ago", diff, diff == 1 ? "" : "s");
    else if (diff < 3600) snprintf(buf, buf_size, "%ld min%s ago", diff / 60, (diff / 60) == 1 ? "" : "s");
    else if (diff < 86400) snprintf(buf, buf_size, "%ld hour%s ago", diff / 3600, (diff / 3600) == 1 ? "" : "s");
    else snprintf(buf, buf_size, "%ld day%s ago", diff / 86400, (diff / 86400) == 1 ? "" : "s");
}

static void get_history_file_path(char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    if (home && *home) {
        snprintf(out, out_sz, "%s/.opendata_history", home);
    }
}

void load_history_file(void) {
    char path[PATH_CAP];
    get_history_file_path(path, sizeof(path));
    if (!path[0]) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        if (len > 0) record_history_entry(line);
    }
    fclose(f);
}


const char* get_history_entry(int index) {
    if (index >= 0 && index < g_history_count) return g_history[index];
    return NULL;
}

int get_history_count(void) {
    return g_history_count;
}

void save_history_file(void) {
    char path[PATH_CAP];
    get_history_file_path(path, sizeof(path));
    if (!path[0]) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_history_count; i++) {
        fprintf(f, "%s\n", g_history[i]);
    }
    fclose(f);
}

void record_history_entry(const char* line) {
    if (!line) return;
    while (is_sql_whitespace(*line)) line++;
    if (!*line) return;
    if (g_history_count > 0 && !strcmp(g_history[g_history_count - 1], line)) return;
    if (g_history_count < MAX_HISTORY_ENTRIES) {
        snprintf(g_history[g_history_count++], sizeof(g_history[0]), "%s", line);
    } else {
        for (int i = 1; i < MAX_HISTORY_ENTRIES; i++) {
            memcpy(g_history[i - 1], g_history[i], sizeof(g_history[0]));
        }
        snprintf(g_history[MAX_HISTORY_ENTRIES - 1], sizeof(g_history[0]), "%s", line);
    }
}

bool history_expand(const char* in, char* out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return false;
    while (is_sql_whitespace(*in)) in++;
    if (*in != '!' || g_history_count == 0) {
        snprintf(out, out_sz, "%s", in);
        return false;
    }
    const char* target = NULL;
    if (in[1] == '!') {
        target = g_history[g_history_count - 1];
    } else if (in[1] == '-' && is_ascii_digit(in[2])) {
        int back = (int)strtol(in + 2, NULL, 10);
        int idx = g_history_count - back;
        if (idx >= 0 && idx < g_history_count) target = g_history[idx];
    } else if (is_ascii_digit(in[1])) {
        int idx = (int)strtol(in + 1, NULL, 10) - 1;
        if (idx >= 0 && idx < g_history_count) target = g_history[idx];
    } else if (in[1] != '\0') {
        const char* prefix = in + 1;
        size_t plen = strlen(prefix);
        for (int i = g_history_count - 1; i >= 0; i--) {
            if (!strncmp(g_history[i], prefix, plen)) {
                target = g_history[i];
                break;
            }
        }
    }
    if (target) {
        snprintf(out, out_sz, "%s", target);
        shell_printf("%s\n", out);
        return true;
    }
    snprintf(out, out_sz, "%s", in);
    return false;
}

static bool str_contains_nocase(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    size_t nlen = strlen(needle);
    for (const char* h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] && (uppercase_ascii(h[i]) == uppercase_ascii(needle[i]))) i++;
        if (i == nlen) return true;
    }
    return false;
}

static bool is_terminal_stdin(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

#ifndef _WIN32
static struct termios s_orig_termios;
static bool s_raw_mode_active = false;

static void shell_disable_raw_mode(void) {
    if (s_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
        s_raw_mode_active = false;
    }
}

static bool shell_enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return false;
    if (s_raw_mode_active) return true;
    if (tcgetattr(STDIN_FILENO, &s_orig_termios) != 0) return false;
    struct termios raw = s_orig_termios;
    raw.c_lflag &= (tcflag_t)~(tcflag_t)(ECHO | ICANON | ISIG);
    raw.c_iflag &= (tcflag_t)~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
    s_raw_mode_active = true;
    return true;
}
#endif

int shell_read_key(void) {
#ifdef _WIN32
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        int ext = _getch();
        if (ext == 72) return SHELL_KEY_UP;
        if (ext == 80) return SHELL_KEY_DOWN;
        if (ext == 75) return SHELL_KEY_LEFT;
        if (ext == 77) return SHELL_KEY_RIGHT;
        if (ext == 71) return SHELL_KEY_HOME;
        if (ext == 79) return SHELL_KEY_END;
        if (ext == 83) return SHELL_KEY_DELETE;
        return SHELL_KEY_NONE;
    }
    if (c == '\r') return SHELL_KEY_ENTER;
    if (c == '\b') return SHELL_KEY_BACKSPACE;
    return c;
#else
    bool need_restore = false;
    if (!s_raw_mode_active && isatty(STDIN_FILENO)) {
        if (shell_enable_raw_mode()) need_restore = true;
    }
    char c = 0;
    int n = (int)read(STDIN_FILENO, &c, 1);
    int res = SHELL_KEY_NONE;
    if (n <= 0) {
        res = SHELL_KEY_CTRL_D;
    } else if (c == '\033') {
        char seq[3] = {0};
        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
            if (seq[0] == '[') {
                if (seq[1] == 'A') res = SHELL_KEY_UP;
                else if (seq[1] == 'B') res = SHELL_KEY_DOWN;
                else if (seq[1] == 'C') res = SHELL_KEY_RIGHT;
                else if (seq[1] == 'D') res = SHELL_KEY_LEFT;
                else if (seq[1] == 'H') res = SHELL_KEY_HOME;
                else if (seq[1] == 'F') res = SHELL_KEY_END;
                else if (seq[1] >= '1' && seq[1] <= '4') {
                    char t = 0;
                    if (read(STDIN_FILENO, &t, 1) > 0 && t == '~') {
                        if (seq[1] == '1') res = SHELL_KEY_HOME;
                        else if (seq[1] == '3') res = SHELL_KEY_DELETE;
                        else if (seq[1] == '4') res = SHELL_KEY_END;
                    }
                }
            }
        }
    } else if (c == '\r' || c == '\n') {
        res = SHELL_KEY_ENTER;
    } else if (c == 127 || c == '\b') {
        res = SHELL_KEY_BACKSPACE;
    } else {
        res = (unsigned char)c;
    }
    if (need_restore) shell_disable_raw_mode();
    return res;
#endif
}

char* read_interactive_line(const char* prompt, char* buf, size_t max_len) {
    if (!buf || max_len == 0) return NULL;
    buf[0] = '\0';
    if (!is_terminal_stdin()) {
        if (prompt && *prompt) {
            fputs(prompt, stdout);
            fflush(stdout);
        }
        if (!fgets(buf, (int)max_len, stdin)) return NULL;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) buf[--len] = '\0';
        return buf;
    }

    char* working = (char*)malloc(max_len);
    if (!working) {
        if (prompt && *prompt) {
            fputs(prompt, stdout);
            fflush(stdout);
        }
        if (!fgets(buf, (int)max_len, stdin)) return NULL;
        size_t flen = strlen(buf);
        while (flen > 0 && (buf[flen - 1] == '\r' || buf[flen - 1] == '\n')) buf[--flen] = '\0';
        return buf;
    }
    working[0] = '\0';

#ifndef _WIN32
    bool raw_active = shell_enable_raw_mode();
#endif

    if (prompt && *prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    size_t len = 0;
    size_t pos = 0;
    int hist_nav = g_history_count;

    for (;;) {
        int k = shell_read_key();
        if (k == SHELL_KEY_UP) {
            if (g_history_count > 0 && hist_nav > 0) {
                if (hist_nav == g_history_count) {
                    snprintf(working, max_len, "%s", buf);
                }
                hist_nav--;
                while (pos < len) { fputc(buf[pos++], stdout); }
                while (pos > 0) { fputs("\b \b", stdout); pos--; }
                snprintf(buf, max_len, "%s", g_history[hist_nav]);
                len = strlen(buf);
                pos = len;
                fputs(buf, stdout);
                fflush(stdout);
            }
            continue;
        }
        if (k == SHELL_KEY_DOWN) {
            if (hist_nav < g_history_count) {
                hist_nav++;
                while (pos < len) { fputc(buf[pos++], stdout); }
                while (pos > 0) { fputs("\b \b", stdout); pos--; }
                if (hist_nav == g_history_count) {
                    snprintf(buf, max_len, "%s", working);
                } else {
                    snprintf(buf, max_len, "%s", g_history[hist_nav]);
                }
                len = strlen(buf);
                pos = len;
                fputs(buf, stdout);
                fflush(stdout);
            }
            continue;
        }
        if (k == SHELL_KEY_LEFT) {
            if (pos > 0) {
                pos--;
                fputc('\b', stdout);
                fflush(stdout);
            }
            continue;
        }
        if (k == SHELL_KEY_RIGHT) {
            if (pos < len) {
                fputc(buf[pos++], stdout);
                fflush(stdout);
            }
            continue;
        }
        if (k == SHELL_KEY_HOME) {
            while (pos > 0) {
                fputc('\b', stdout);
                pos--;
            }
            fflush(stdout);
            continue;
        }
        if (k == SHELL_KEY_END) {
            while (pos < len) {
                fputc(buf[pos++], stdout);
            }
            fflush(stdout);
            continue;
        }
        if (k == SHELL_KEY_DELETE) {
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos);
                len--;
                for (size_t i = pos; i < len; i++) fputc(buf[i], stdout);
                fputc(' ', stdout);
                for (size_t i = pos; i <= len; i++) fputc('\b', stdout);
                fflush(stdout);
            }
            continue;
        }
        if (k == SHELL_KEY_ENTER || k == '\n') {
            fputc('\n', stdout);
            fflush(stdout);
            buf[len] = '\0';
            free(working);
#ifndef _WIN32
            if (raw_active) shell_disable_raw_mode();
#endif
            return buf;
        }
        if (k == SHELL_KEY_CTRL_C) {
            fputs("^C\n", stdout);
            fflush(stdout);
            buf[0] = '\0';
            free(working);
#ifndef _WIN32
            if (raw_active) shell_disable_raw_mode();
#endif
            return buf;
        }
        if (k == SHELL_KEY_CTRL_D) {
            if (len == 0) {
                free(working);
#ifndef _WIN32
                if (raw_active) shell_disable_raw_mode();
#endif
                return NULL;
            }
            continue;
        }
        if (k == SHELL_KEY_CTRL_R) {
            char term[64] = {0};
            size_t tlen = 0;
            int matched_idx = -1;
            int start_idx = g_history_count - 1;
            for (;;) {
                fputs("\r                                                                               \r", stdout);
                shell_printf("(reverse-i-search)'%s': %s", term, matched_idx >= 0 ? g_history[matched_idx] : "");
                fflush(stdout);
                int sk = shell_read_key();
                if (sk == SHELL_KEY_CTRL_R) {
                    if (matched_idx > 0) {
                        start_idx = matched_idx - 1;
                        matched_idx = -1;
                        for (int h = start_idx; h >= 0; h--) {
                            if (tlen == 0 || str_contains_nocase(g_history[h], term)) {
                                matched_idx = h;
                                break;
                            }
                        }
                    }
                } else if (sk == SHELL_KEY_BACKSPACE) {
                    if (tlen > 0) {
                        term[--tlen] = '\0';
                        matched_idx = -1;
                        for (int h = g_history_count - 1; h >= 0; h--) {
                            if (tlen == 0 || str_contains_nocase(g_history[h], term)) {
                                matched_idx = h;
                                break;
                            }
                        }
                    }
                } else if (sk == SHELL_KEY_ENTER || sk == '\n') {
                    fputs("\r                                                                               \r", stdout);
                    if (matched_idx >= 0) {
                        snprintf(buf, max_len, "%s", g_history[matched_idx]);
                        len = strlen(buf);
                        pos = len;
                    }
                    if (prompt && *prompt) fputs(prompt, stdout);
                    fputs(buf, stdout);
                    fputc('\n', stdout);
                    fflush(stdout);
                    free(working);
#ifndef _WIN32
                    if (raw_active) shell_disable_raw_mode();
#endif
                    return buf;
                } else if (sk == SHELL_KEY_CTRL_C || sk == '\033') {
                    fputs("\r                                                                               \r", stdout);
                    if (prompt && *prompt) fputs(prompt, stdout);
                    fputs(buf, stdout);
                    for (size_t i = pos; i < len; i++) fputc('\b', stdout);
                    fflush(stdout);
                    break;
                } else if (sk >= 32 && sk < 127 && tlen + 1 < sizeof(term)) {
                    term[tlen++] = (char)sk;
                    term[tlen] = '\0';
                    matched_idx = -1;
                    for (int h = g_history_count - 1; h >= 0; h--) {
                        if (str_contains_nocase(g_history[h], term)) {
                            matched_idx = h;
                            break;
                        }
                    }
                }
            }
            continue;
        }
        if (k == SHELL_KEY_BACKSPACE) {
            if (pos > 0) {
                if (pos == len) {
                    pos--;
                    len--;
                    buf[len] = '\0';
                    fputs("\b \b", stdout);
                    fflush(stdout);
                } else {
                    pos--;
                    memmove(buf + pos, buf + pos + 1, len - pos);
                    len--;
                    fputc('\b', stdout);
                    for (size_t i = pos; i < len; i++) fputc(buf[i], stdout);
                    fputc(' ', stdout);
                    for (size_t i = pos; i <= len; i++) fputc('\b', stdout);
                    fflush(stdout);
                }
            }
            continue;
        }
        if (k == SHELL_KEY_TAB || k == '\t') {
            size_t wstart = pos;
            while (wstart > 0 && (is_identifier_start(buf[wstart - 1]) ||
                                  is_ascii_digit(buf[wstart - 1]) ||
                                  buf[wstart - 1] == '_' ||
                                  buf[wstart - 1] == '.')) {
                wstart--;
            }
            size_t wlen = pos - wstart;
            if (wlen > 0) {
                char prefix[MAX_NAME_LEN];
                if (wlen < sizeof(prefix)) {
                    memcpy(prefix, buf + wstart, wlen);
                    prefix[wlen] = '\0';
                    static const char* const kws[] = {
                        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
                        "DELETE", "CREATE", "TABLE", "DROP", "ALTER", "ADD", "COLUMN", "INDEX",
                        "DATABASE", "DATABASES", "SHOW", "TABLES", "USERS", "DESCRIBE", "DESC", "BROWSE",
                        "EXPLAIN", "STATUS", "HELP", "COMMIT", "ROLLBACK", "BEGIN", "EXIT", "QUIT",
                        "IMPORT", "EXPORT", "BACKUP", "RESTORE", "FORMAT", "BOX", "MARKDOWN", "TSV",
                        "JSON", "CSV", "AUTOCOMMIT", "CLEAR", "DISCONNECT", "GUIDE", "ANALYZE", "DRYRUN", "TEST",
                        "LIMIT", "OFFSET", "ORDER", "BY", "GROUP", "HAVING", "JOIN", "LEFT", "RIGHT",
                        "INNER", "CROSS", "ON", "AS", "AND", "OR", "NOT", "IN", "IS", "NULL", "LIKE",
                        "DISTINCT", "COUNT", "SUM", "AVG", "MIN", "MAX", "VECTOR", "KNN_COSINE", "KNN_L2",
                        ".help", ".tables", ".schema", ".mode", ".pager", ".backup", ".restore",
                        ".import", ".export", ".dump", ".open", ".databases", ".users", ".status",
                        ".selftest", ".quit", ".exit"
                    };
                    const char* matches[128];
                    int match_count = 0;
                    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
                        bool prefix_match = true;
                        for (size_t c = 0; c < wlen; c++) {
                            if (uppercase_ascii(kws[i][c]) != uppercase_ascii(prefix[c])) {
                                prefix_match = false;
                                break;
                            }
                        }
                        if (prefix_match && match_count < 128) {
                            matches[match_count++] = kws[i];
                        }
                    }
                    if (g_db) {
                        for (int t = 0; t < g_db->table_count; t++) {
                            bool prefix_match = true;
                            for (size_t c = 0; c < wlen; c++) {
                                if (uppercase_ascii(g_db->tables[t].name[c]) != uppercase_ascii(prefix[c])) {
                                    prefix_match = false;
                                    break;
                                }
                            }
                            if (prefix_match && match_count < 128) {
                                matches[match_count++] = g_db->tables[t].name;
                            }
                            for (int col = 0; col < g_db->tables[t].col_count; col++) {
                                const char* colname = g_db->tables[t].columns[col].name;
                                bool col_match = true;
                                for (size_t c = 0; c < wlen; c++) {
                                    if (uppercase_ascii(colname[c]) != uppercase_ascii(prefix[c])) {
                                        col_match = false;
                                        break;
                                    }
                                }
                                if (col_match && match_count < 128) {
                                    bool dup = false;
                                    for (int m = 0; m < match_count; m++) {
                                        if (!strcmp(matches[m], colname)) { dup = true; break; }
                                    }
                                    if (!dup) matches[match_count++] = colname;
                                }
                            }
                        }
                    }
                    if (match_count == 1) {
                        const char* match = matches[0];
                        size_t mlen = strlen(match);
                        for (size_t i = wlen; i < mlen; i++) {
                            if (len + 1 < max_len) {
                                buf[pos++] = match[i];
                                len++;
                                fputc(match[i], stdout);
                            }
                        }
                        if (len + 1 < max_len) {
                            buf[pos++] = ' ';
                            len++;
                            fputc(' ', stdout);
                        }
                        buf[len] = '\0';
                        fflush(stdout);
                    } else if (match_count > 1) {
                        size_t lcp_len = strlen(matches[0]);
                        for (int m = 1; m < match_count; m++) {
                            size_t k_idx = 0;
                            while (k_idx < lcp_len && matches[m][k_idx] &&
                                   (uppercase_ascii(matches[0][k_idx]) == uppercase_ascii(matches[m][k_idx]))) {
                                k_idx++;
                            }
                            lcp_len = k_idx;
                        }
                        if (lcp_len > wlen) {
                            for (size_t i = wlen; i < lcp_len; i++) {
                                if (len + 1 < max_len) {
                                    buf[pos++] = matches[0][i];
                                    len++;
                                    fputc(matches[0][i], stdout);
                                }
                            }
                            buf[len] = '\0';
                            fflush(stdout);
                        }
                        fputc('\n', stdout);
                        for (int m = 0; m < match_count; m++) {
                            shell_printf("  %-18s%s", matches[m], (m % 4 == 3 || m + 1 == match_count) ? "\n" : "");
                        }
                        if (prompt && *prompt) fputs(prompt, stdout);
                        fputs(buf, stdout);
                        for (size_t i = pos; i < len; i++) fputc('\b', stdout);
                        fflush(stdout);
                    }
                }
            }
            continue;
        }
        if ((unsigned char)k >= 32 && (unsigned char)k < 127 && len + 1 < max_len) {
            if (pos == len) {
                buf[pos++] = (char)k;
                len++;
                buf[len] = '\0';
                fputc(k, stdout);
                fflush(stdout);
            } else {
                memmove(buf + pos + 1, buf + pos, len - pos + 1);
                buf[pos++] = (char)k;
                len++;
                for (size_t i = pos - 1; i < len; i++) fputc(buf[i], stdout);
                for (size_t i = pos; i < len; i++) fputc('\b', stdout);
                fflush(stdout);
            }
        }
    }
}


static void print_quick_start_help(void) {
    shell_puts("================================================================================");
    shell_puts("                    PC/OpenData SQL — Quick Start");
    shell_puts("================================================================================\n"
          "  GETTING STARTED (5 commands)\n"
          "    CREATE DATABASE 'mydata.pcod';     Create and open a new database file\n"
          "    CREATE TABLE t (id NUMBER PRIMARY KEY, name VARCHAR2(50));\n"
          "                                      Create your first table\n"
          "    INSERT INTO t VALUES (1, 'hello');  Add a row\n"
          "    COMMIT;                             Save your work (important!)\n"
          "    SELECT * FROM t;                    See your data\n"
          "    exit;                               Quit\n\n"
          "  COMMON TASKS\n"
          "    GUIDE;                              Choose a task and see the next command\n"
          "    LOAD SAMPLE;                        Load practice data (SAMPLE_DATA table)\n"
          "    SHOW TABLES;                        List your tables\n"
          "    BROWSE table;                      Preview up to 20 rows\n"
          "    CONNECTION STATUS;                 Check service, session, and transport\n"
          "    DESCRIBE t;                         Show table structure\n"
          "    UPDATE t SET name='x' WHERE id=1;   Change a row\n"
          "    DELETE FROM t WHERE id=1;           Remove a row\n"
          "    ROLLBACK;                           Undo uncommitted changes\n"
          "    BACKUP;                             Save a backup copy\n\n"
          "  LEARN MORE\n"
          "    HELP SELECT;                        Query examples\n"
          "    HELP CREATE;                        Table and database creation\n"
          "    HELP DDL;                           Schema evolution and ALTER TABLE\n"
          "    HELP META;                          Meta-commands (\\dt, \\d, .mode)\n"
          "    HELP SET;                           Session options and formatting\n"
          "    HELP SAFETY;                        Dry-run impact preview and safe mode\n"
          "    HELP CLI;                           Command-line launch flags\n"
          "    HELP IMPORT;                        Guided spreadsheet import\n"
          "    HELP SECURITY;                      Safe account and role guidance\n"
          "    HELP BACKUP;                        Backup and restore workflow\n"
          "    HELP TRANSACTIONS;                  Commit, rollback, savepoints\n"
          "    HELP VECTORS;                       AI/semantic search\n"
          "    HELP SERVER;                        Network and TLS setup\n"
          "    HELP;                               Full command reference\n"
          "================================================================================");
}

static void print_getting_started_guide(void) {
    shell_puts("PC/OpenData — Choose a task\n"
          "  Practice with data:    LOAD SAMPLE; then BROWSE SAMPLE_DATA;\n"
          "  Import a spreadsheet:  connect to a database, then IMPORT WIZARD;\n"
          "  Create a table:        CREATE WIZARD; (no SQL needed)\n"
          "  Create a database:     CREATE DATABASE 'mydata.pcod';\n"
          "  Explore a table:       SHOW TABLES; then BROWSE table_name;\n"
          "  Inspect its columns:   DESCRIBE table_name;\n"
          "  Protect your data:     BACKUP; then check STATUS;\n"
          "  Set up user access:    HELP SECURITY;\n"
          "  Learn a command:       HELP SELECT; HELP IMPORT; HELP BACKUP;\n"
          "This guide is optional. Type GUIDE; whenever you want to see it again.");
}

typedef struct {
    const char* name;
    const char* aliases;
    const char* body;
} HelpTopic;

static const HelpTopic HELP_TOPICS[] = {
    {"SELECT", "QUERY,FILTER,FIND",
     "SELECT — Query data\n"
     "  SELECT * FROM table;                  All rows and columns\n"
     "  SELECT col1, col2 FROM table;         Specific columns\n"
     "  SELECT * FROM table WHERE id = 1;     Filter rows\n"
     "  SELECT * FROM table ORDER BY col;     Sort results\n"
     "  SELECT COUNT(*) FROM table;           Count rows\n"
     "  SELECT * FROM t1 JOIN t2 ON t1.id = t2.id;  Join tables"},
    {"CREATE", "TABLE,WIZARD,NEW",
     "CREATE — Make databases and tables\n"
     "  CREATE WIZARD;                        Interactive table builder (no SQL needed)\n"
     "  CREATE DATABASE 'file.pcod';          New database\n"
     "  CREATE TABLE t (id NUMBER PRIMARY KEY, name VARCHAR2(50));\n"
     "  CREATE TABLE t (id NUMBER PRIMARY KEY, dept_id NUMBER REFERENCES dept(id));\n"
     "  CREATE INDEX idx ON t(col);           Faster lookups\n"
     "  CREATE USER 'name' IDENTIFIED BY 'pass' ROLE ADMIN;"},
    {"TRANSACTIONS", "TRANSACTION,COMMIT,ROLLBACK,UNDO,SAVE",
     "TRANSACTIONS — Save or undo work\n"
     "  BEGIN;                                Start a transaction\n"
     "  COMMIT;                               Save all changes permanently\n"
     "  ROLLBACK;                             Discard uncommitted changes\n"
     "  SAVEPOINT name;                       Mark a point to undo to\n"
     "  ROLLBACK TO name;                     Undo back to that point\n"
     "  SET AUTOCOMMIT ON;                    Save after every statement"},
    {"SAFETY", "DRYRUN,TEST,CONFIRM,SAFETY,DESTRUCTIVE",
     "SAFETY & IMPACT PREVIEW — Protect and test changes before executing\n"
     "  SET CONFIRM ON | OFF;                 Prompt before DROP, TRUNCATE, or unconditional DELETE\n"
     "  DRYRUN DROP TABLE users;              Preview table deletion impact (rows, indexes, FKs, bytes)\n"
     "  TEST DELETE FROM orders;              Simulate row deletion without modifying database file\n"
     "  TEST DROP DATABASE 'mydata.pcod';     Inspect database file before deleting"},
    {"VECTORS", "VECTOR,AI,EMBEDDING,SEARCH,SIMILARITY",
     "VECTORS — AI / semantic search\n"
     "  CREATE TABLE docs (id NUMBER PRIMARY KEY, embedding VECTOR(3));\n"
     "  INSERT INTO docs VALUES (1, '[1.0, 0.0, 0.0]');\n"
     "  SELECT * FROM docs WHERE embedding <=> '[1.0, 0.0, 0.0]' < 0.5;\n"
     "  <=> means cosine distance (0 = identical, 2 = opposite)"},
    {"SERVER", "NETWORK,TLS,TCPS,REMOTE",
     "SERVER — Network and TLS\n"
     "  opendata --database file.pcod --server --port 7350\n"
     "  opendata --database file.pcod --tcps --port 7338 --cert ... --key ...\n"
     "  CREATE USER 'name' IDENTIFIED BY 'pass' ROLE READ_ONLY;\n"
     "  SHOW SESSIONS;                        See connected clients\n"
     "  RESTART;                              Zero-downtime hot-patch restart"},
    {"IMPORT", "CSV,SPREADSHEET,LOAD,EXCEL",
     "IMPORT — Load CSV data\n"
     "  IMPORT WIZARD;                       Interactive preview and type suggestions\n"
     "  IMPORT TABLE people FROM CSV 'people.csv' HEADER;\n"
     "                                      Scripted import into a prepared table\n"
     "  Header names are matched to columns; use DESCRIBE people to check names.\n"
     "  The wizard imports into a new table and commits the table and rows together.\n"
     "  Type suggestions come from the preview only; review them before confirming."},
    {"SECURITY", "USERS,ROLES,GRANT,REVOKE,PERMISSIONS,PASSWORD,ACCESS",
     "SECURITY — Accounts and access\n"
     "  CREATE USER 'analyst' IDENTIFIED BY '<secret>' ROLE READ_ONLY;\n"
     "  GRANT SELECT ON employees TO 'analyst';\n"
     "  GRANT SELECT, INSERT ON timesheets TO 'analyst';\n"
     "  REVOKE ON employees FROM 'analyst';\n"
     "  SHOW GRANTS;                              List per-table privileges\n"
     "  COMMIT;\n"
     "  READ_ONLY users get no table access over the network until you GRANT it.\n"
     "  ADMIN users always have full access. Keep passwords out of scripts and transcripts."},
    {"BACKUP", "RESTORE,RECOVERY,PROTECT,VERIFY",
     "BACKUP — Protect and restore data\n"
     "  BACKUP;                               Create a verified backup now\n"
     "  VERIFY BACKUP '<file>';               Prove a backup is restorable (offline check)\n"
     "  STATUS;                               Check backup health and generation\n"
     "  opendata --database restored.pcod --restore '<backup-file>'\n"
     "  Restore to a new file first, then verify it before replacing production data.\n"
     "  Configure a separate backup directory when creating/opening the database."},
    {"DIAGNOSTICS", "HEALTH,SUPPORT,DEBUG,STATUS",
     "DIAGNOSTICS — Check health and prepare support details\n"
     "  STATUS;                               Database, storage, and backup state\n"
     "  SHOW SESSIONS;                        Active client connections\n"
     "  CHECK INTEGRITY;                      Verify database consistency\n"
     "  SUPPORT REPORT TO 'support-summary.txt';\n"
     "  The support summary omits paths, configuration, accounts, SQL, and row data."},
    {"CONNECTION", "SESSION,TRANSPORT,ENCRYPTION",
     "CONNECTION STATUS — Current service and session\n"
     "  CONNECTION STATUS;\n"
     "  Shows whether the service/database are available, the current user,\n"
     "  whether the connection uses TLS, and storage/backup health.\n"
     "  Plain network mode is restricted to loopback and is not encrypted."},
    {"META", "DOT,SLASH,SHORTCUT,SHORTCUTS",
     "META-COMMANDS & CLI SHORTCUTS (PostgreSQL & SQLite style)\n"
     "  \\dt | .tables                         List all tables, column counts, and row counts\n"
     "  \\d [<t>] | .schema [<t>]              Show CREATE TABLE DDL for table or database\n"
     "  \\l                                    List .pcod database files in current directory\n"
     "  \\du                                   List database users and roles (alias for SHOW USERS)\n"
     "  \\i '<file>' | .read '<file>'          Execute SQL statements from a file\n"
     "  \\x                                    Toggle expanded vertical row display\n"
     "  .mode <format>                        Set format: box, markdown, vertical, tsv, json, csv, table\n"
     "  .mode pager on | off                  Toggle interactive query results pager\n"
     "  .pager [on | off]                     Show or toggle results pager\n"
     "  \\c | CANCEL                           Abort current multi-line query buffer\n"
     "  \\q | .exit | EXIT | QUIT              Exit the shell session"},
    {"SHELL", "LINE,EDITING,HISTORY,COMPLETION,PAGER,SHORTCUTS,SEARCH",
     "INTERACTIVE SHELL & NAVIGATION SHORTCUTS\n"
     "  Up / Down Arrow                       Navigate command history\n"
     "  Left / Right / Home / End             Move cursor within current line\n"
     "  Delete / Backspace                    Delete characters under/before cursor\n"
     "  Tab                                   Auto-complete keywords, tables, columns, and dot-commands\n"
     "  Tab (multi-match)                     Display formatted grid of matching candidates\n"
     "  Ctrl+R                                Interactive reverse history search (type to search)\n"
     "  Ctrl+C                                Cancel current line without executing\n"
     "  Ctrl+D                                Exit shell (on empty prompt)\n"
     "  Enter (on empty continuation line)    Execute buffered multi-line query immediately"},
    {"SET", "CONFIG,FORMAT,OPTIONS,SETTINGS",
     "SET & CONFIGURATION OPTIONS\n"
     "  SET AUTOCOMMIT ON | OFF;              Toggle automatic commit after mutations\n"
     "  SET CONFIRM ON | OFF;                 Prompt before DROP, TRUNCATE, or unconditional DELETE\n"
     "  SET SAFE_MODE ON | OFF;               Alias for SET CONFIRM\n"
     "  SET FORMAT BOX|MARKDOWN|VERTICAL|TSV|JSON|CSV|TABLE;\n"
     "                                        Set result display formatting\n"
     "  SET NULL '<string>';                  Set display text for NULL values\n"
     "  SET MAXROWS <n>;                      Cap query result rows (0 = unlimited)\n"
     "  SET SEMICOLON OPTIONAL ON | OFF;      Execute single-line statements on Enter\n"
     "  SET TIMING ON | OFF;                  Display query execution duration in microseconds"},
    {"DDL", "ALTER,SCHEMA,TABLE,CTAS,MODIFY,DROP",
     "DATA DEFINITION & SCHEMA EVOLUTION\n"
     "  CREATE TABLE [IF NOT EXISTS] t (id NUMBER PRIMARY KEY, name VARCHAR2(50));\n"
     "  CREATE TABLE t_copy AS SELECT * FROM t WHERE id < 100;\n"
     "  CREATE INDEX [IF NOT EXISTS] idx ON t(name);\n"
     "  ALTER TABLE t ADD [COLUMN] email VARCHAR2(100);\n"
     "  ALTER TABLE t DROP [COLUMN] email;\n"
     "  ALTER TABLE t MODIFY [COLUMN] name VARCHAR2(100);\n"
     "  ALTER TABLE t RENAME TO t_new;\n"
     "  ALTER TABLE t ADD CONSTRAINT fk_d FOREIGN KEY (dept_id) REFERENCES dept(id);\n"
     "  ALTER TABLE t DROP CONSTRAINT fk_d;\n"
     "  DROP TABLE [IF EXISTS] t;\n"
     "  DROP INDEX idx;\n"
     "  TRUNCATE TABLE t;"},
    {"CLI", "STARTUP,FLAGS,OPTIONS,ARGUMENTS",
     "COMMAND-LINE STARTUP FLAGS\n"
     "  opendata [<file.pcod>] [\"<sql>\"]       Open database and optionally run query\n"
     "  opendata -e \"<sql>\"                    Execute query and exit immediately\n"
     "  opendata -f <table|csv|json>          Output format for query results\n"
     "  opendata --dev                        Instant dev server on 127.0.0.1:7350 (dev.pcod)\n"
     "  opendata -s, --server                 Start TCP server on loopback\n"
     "  opendata --tcps --port 7338           Start TLS server\n"
     "  opendata --backup-dir <dir>           Directory for automated/manual backups\n"
     "  opendata --backup-interval <sec>      Automated backup interval in seconds\n"
     "  opendata --restore <backup-file>      Restore database snapshot from backup\n"
     "  opendata --readonly                   Open database in read-only mode\n"
     "  opendata --check-config               Validate configuration file syntax\n"
     "  opendata --ping                       Ping running server instance\n"
     "  opendata -v, --version                Display version\n"
     "  opendata -h, --help                   Display reference guide"}
};

static bool contains_case_insensitive(const char* haystack, const char* needle) {
    size_t needle_len = strlen(needle);
    if (!needle_len) return true;
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               uppercase_ascii(p[i]) == uppercase_ascii(needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

static bool help_name_matches(const HelpTopic* topic_entry, const char* word) {
    if (!strcmp(topic_entry->name, word)) return true;
    const char* alias = topic_entry->aliases;
    while (*alias) {
        const char* comma = strchr(alias, ',');
        size_t len = comma ? (size_t)(comma - alias) : strlen(alias);
        if (strlen(word) == len && !strncmp(alias, word, len)) return true;
        if (!comma) break;
        alias = comma + 1;
    }
    return false;
}

static void print_topic_help(const char* topic) {
    const size_t count = sizeof(HELP_TOPICS) / sizeof(HELP_TOPICS[0]);
    /* 1. Exact match on topic name. */
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(HELP_TOPICS[i].name, topic)) {
            shell_puts(HELP_TOPICS[i].body);
            return;
        }
    }
    /* 2. Match on alias. */
    for (size_t i = 0; i < count; i++) {
        if (help_name_matches(&HELP_TOPICS[i], topic)) {
            shell_puts(HELP_TOPICS[i].body);
            return;
        }
    }
    /* 2. Fuzzy match: a near-miss spelling shows the closest topic. */
    const char* best_name = NULL;
    const char* best_body = NULL;
    int best_dist = 3;
    for (size_t i = 0; i < count; i++) {
        int d = edit_distance_capped(topic, HELP_TOPICS[i].name, 2);
        if (d < best_dist) {
            best_dist = d;
            best_name = HELP_TOPICS[i].name;
            best_body = HELP_TOPICS[i].body;
        }
    }
    if (best_body) {
        shell_printf("(Showing help for %s — closest match to '%s')\n", best_name, topic);
        shell_puts(best_body);
        return;
    }
    /* 3. Keyword search: list topics whose body or aliases mention the word. */
    shell_printf("No help topic named '%s'.", topic);
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (contains_case_insensitive(HELP_TOPICS[i].body, topic) ||
            contains_case_insensitive(HELP_TOPICS[i].aliases, topic)) {
            shell_printf("%s HELP %s;", found ? "," : " Related topics:", HELP_TOPICS[i].name);
            found = true;
        }
    }
    if (!found) shell_puts(" Type HELP; for the full reference.");
    else shell_puts("");
}

void print_command_help(void) {
    print_quick_start_help();
    shell_puts("");
    shell_puts("================================================================================");
    shell_puts("                    PC/OpenData SQL Interactive Shell " SERVER_VERSION);
    shell_puts("================================================================================\n"
          "  COMMAND-LINE LAUNCH OPTIONS\n"
          "    opendata [<file.pcod>] [\"<sql>\"]   Connect database and optionally run query\n"
          "    opendata -e, --execute \"<sql>\"     Run SQL non-interactively and exit\n"
          "    opendata -f, --format <fmt>        Result format: table, csv, json\n"
          "    opendata --dev                     Local dev server on 127.0.0.1:7350 (dev.pcod)\n"
          "    opendata -s, --server              Start TCP server on 127.0.0.1\n"
          "    opendata --tcps                    Start TLS encrypted server\n"
          "    opendata -p, --port <port>         Server port (default: 7350 TCP, 7338 TLS)\n"
          "    opendata --bind <IPv4>             Server bind IP address (default: 127.0.0.1)\n"
          "    opendata --database <file>         Database snapshot path\n"
          "    opendata --backup-dir <dir>        Directory for background/instant backups\n"
          "    opendata --backup-interval <sec>   Automated backup interval (1-86400 seconds)\n"
          "    opendata --restore <backup>        Restore snapshot into target database\n"
          "    opendata --pitr <timestamp>        Point-in-time recovery to Unix timestamp\n"
#ifdef _WIN32
          "    opendata --install-service [name]  Register Windows background service\n"
          "    opendata --uninstall-service [name] Remove Windows background service\n"
          "    opendata --start-service [name]    Start installed Windows service\n"
          "    opendata --stop-service [name]     Stop running Windows service\n"
#endif
          "    opendata --ad-server <host>        Active Directory domain controller host/IP\n"
          "    opendata --ad-domain <domain>      Active Directory domain name (e.g. corp.local)\n"
          "    opendata --ad-admin-group <grp>    AD security group mapped to ADMIN role\n"
          "    opendata --ldap-server <host>      LDAP server hostname or IP address\n"
          "    opendata --ldap-port <port>        LDAP server port (default: 389, or 636 for SSL)\n"
          "    opendata --ldap-ssl                Enable LDAPS / SSL encryption\n"
          "    opendata --ldap-base-dn <dn>       LDAP search base DN (e.g. ou=users,dc=org)\n"
          "    opendata --ldap-admin-group <grp>  LDAP admin group name/DN for ADMIN role\n"
          "    opendata --krb-realm <realm>       Kerberos / GSSAPI authentication realm\n"
          "    opendata --krb-service <service>   Kerberos service principal name (default: opendata)\n"
          "    opendata --krb-keytab <path>       Kerberos keytab file path\n"
          "    opendata --statement-timeout <ms>  Maximum statement execution time limit in milliseconds\n"
          "    opendata --auto-vacuum             Enable automatic background table compaction\n"
          "    opendata --no-auto-vacuum          Disable automatic background table compaction\n"
          "    opendata --readonly                Open database in read-only mode\n"
          "    opendata --no-default-db           Do not auto-open default database\n"
          "    opendata --check-config            Validate configuration file syntax and exit\n"
          "    opendata --wallet <dir>            Directory with TLS certificates and config\n"
          "    opendata --inspect-wallet <dir>    Inspect and validate TLS wallet configuration\n"
#ifdef _WIN32
          "    opendata --cert store:<thumb>      Schannel TLS certificate (CurrentUser\\My)\n"
#else
          "    opendata --cert <PEM-chain>        TLS certificate chain PEM file\n"
          "    opendata --key <PEM-key>           TLS private key PEM file\n"
#endif
          "    opendata --cluster-node <id>       Raft cluster node ID\n"
          "    opendata --cluster-port <port>     Raft cluster consensus port\n"
          "    opendata --cluster-peers <list>    Comma-separated peer addresses (node@ip:port)\n"
          "    opendata --cluster-ca <path>       CA certificate for inter-node mutual TLS\n"
          "    opendata --sync-replication        Enable synchronous cluster replication\n"
          "    opendata --cluster-follower        Join Raft cluster as follower node\n"
          "    opendata --ping                    Health-check running server instance\n"
          "    opendata self-test [suite]         Run built-in enterprise self-test suite\n"
          "    opendata -v, --version             Display version information\n"
          "    opendata -h, --help                Display this reference guide\n");

    shell_puts("  CONNECTION & DATABASE LIFECYCLE\n"
          "    CREATE DATABASE '<path>' [BACKUP '<dir>'] [INTERVAL <sec>]\n"
          "                                  Initialize and connect to a new empty database\n"
          "    CONNECT '<path>' [BACKUP '<dir>'] [INTERVAL <sec>] [READONLY]\n"
          "                                  Open or create a database instance (aliases: USE, OPEN)\n"
          "    DISCONNECT                    Close active database snapshot and release locks\n"
          "    DROP DATABASE '<path>'        Permanently delete a database snapshot file\n"
          "    SHOW DATABASES                List .pcod database files in working directory\n"
          "    \\c | CANCEL                   Clear/abort current multi-line query buffer\n"
          "    STATUS                        Display database health, generation, and metrics\n"
          "    CONNECTION STATUS             Show connection transport, TLS state, and user\n"
          "    SUPPORT REPORT TO '<file>'    Write a privacy-preserving support summary\n"
          "    CLEAR | CLS                   Clear terminal screen\n"
          "    PING                          Check server responsiveness\n"
          "    GUIDE                         Interactive task guide and next commands\n"
          "    HELP [<topic>]                Display full reference or topic help\n"
          "    EXIT | QUIT                   Exit shell session (discards uncommitted work)\n");

    shell_puts("  META-COMMANDS & CLI SHORTCUTS (POSTGRESQL & SQLITE STYLE)\n"
          "    \\dt | .tables                 List all user tables, column counts, and row counts\n"
          "    \\d [<t>] | .schema [<t>]      Display CREATE TABLE DDL for table or all tables\n"
          "    \\l                            List available .pcod database files in current directory\n"
          "    \\du                           List database users and roles (alias for SHOW USERS)\n"
          "    \\i '<file>' | .read '<file>'  Execute SQL commands from an external script file\n"
          "    \\x                            Toggle expanded vertical result display on/off\n"
          "    .mode <fmt>                   Set result format: box, markdown, vertical, tsv, json, csv, table\n"
          "    \\q | .exit                    Exit shell session\n"
          "    @<script.sql>                 Alias for SOURCE '<script.sql>'\n");

    shell_puts("  IRIS INTERNAL TEXT EDITOR (GNU NANO-STYLED)\n"
          "    IRIS ['<file>']               Launch IRIS editor on SQL buffer or target file\n"
          "    EDIT ['<file>'] | ED ['<file>']\n"
          "                                  Aliases to invoke the IRIS text editor\n"
          "    Shortcuts: ^G Help  ^O WriteOut  ^R ReadFile  ^W WhereIs  ^K Cut  ^U UnCut\n"
          "               ^J Justify SQL  ^C CurPos  ^T Execute SQL  ^X Exit\n");

    shell_puts("  SCRIPTING, SPOOLING, CONFIG & SAFETY\n"
          "    SOURCE '<script.sql>' | @<script.sql>\n"
          "                                  Execute batch SQL statements from a file\n"
          "    SPOOL '<output.txt>' | SPOOL OFF\n"
          "                                  Mirror all shell and query output to a transcript\n"
          "    HISTORY                       Display numbered session command history\n"
          "    SET AUTOCOMMIT ON | OFF       Toggle automatic durable commit after mutations\n"
          "    SET CONFIRM ON | OFF          Toggle confirmation prompt for destructive statements\n"
          "    SET SAFE_MODE ON | OFF        Alias for SET CONFIRM\n"
          "    DRYRUN | TEST <statement>     Simulate statement and preview impact without modifying data\n"
          "    SET FORMAT BOX|MARKDOWN|VERTICAL|TSV|JSON|CSV|TABLE\n"
          "                                  Output formatting mode for query results\n"
          "    SET NULL '<text>'             Set custom display string for NULL values\n"
          "    SET MAXROWS <n>               Cap maximum rows returned by SELECT (0 = unlimited)\n"
          "    SET SEMICOLON OPTIONAL ON|OFF Run complete single-line statements on Enter\n"
          "    SET TIMING ON | OFF           Toggle microsecond query execution timer\n");

    shell_puts("  DATABASE USER ADMINISTRATION\n"
          "    GRANT SELECT|INSERT|UPDATE|DELETE|ALL ON <t> TO '<user>'\n"
          "                                  Grant per-table privileges to a READ_ONLY user\n"
          "    REVOKE ON <t> FROM '<user>'     Remove all of a user's grants on a table\n"
          "    SHOW GRANTS                   List per-table privilege grants\n"
          "    CREATE USER '<name>' IDENTIFIED BY '<password>' ROLE ADMIN|READ_ONLY\n"
          "                                  Create an administrator or read-only account\n"
          "    SHOW USERS                    List account names, roles, and enabled states\n"
          "    ALTER USER '<name>' PASSWORD '<password>'\n"
          "                                  Change an account password\n"
          "    ALTER USER '<name>' ROLE ADMIN|READ_ONLY\n"
          "                                  Change database-wide permissions\n"
          "    ALTER USER '<name>' DISABLE | ENABLE\n"
          "                                  Suspend or restore an account's access\n"
          "    DROP USER '<name>'            Remove an account\n"
          "    COMMIT                        Persist account changes; ROLLBACK discards them\n"
          "    Network account management requires ADMIN; trusted local access is privileged.\n"
          "    Passwords: 16-255 bytes. Names: 1-63 printable ASCII bytes, no spaces/quotes.\n"
          "    The last enabled administrator cannot be dropped, disabled, or demoted.\n"
          "    Existing connections are rechecked on their next request after COMMIT.\n"
          "    Keep passwords out of command-line arguments and spooled transcripts.\n");

    shell_puts("  SCHEMA INTROSPECTION, OBSERVABILITY & DIAGNOSTICS\n"
          "    DESCRIBE | DESC <t>           Display column names, types, keys, and references\n"
          "    SHOW TABLES                   List all registered tables and row counts\n"
          "    BROWSE <table>                Preview up to 20 rows from one table\n"
          "    SHOW SCHEMA [<t>]             Reverse-engineer exact CREATE TABLE DDL statements\n"
          "    SHOW CREATE TABLE <t>         Print canonical CREATE TABLE statement for table\n"
          "    SHOW INDEXES [FROM <t>]       List primary, unique, and secondary B+ tree indexes\n"
          "    SHOW CONSTRAINTS [FROM <t>]   List PK, UNIQUE, NOT NULL, and FK actions\n"
          "    SHOW STATS [FOR <t>]          Display CBO statistics, sample rows, and bounds\n"
          "    SHOW ENGINE STATUS            Display storage engine memory, buffer pool & metrics\n"
          "    SHOW METRICS                  Display database engine performance counters\n"
          "    SHOW SESSIONS | SHOW PROCESSLIST\n"
          "                                  List active client sessions and current queries\n"
          "    KILL [SESSION] <id>           Terminate an active client session by ID\n"
          "    RESTART                       Zero-downtime hot-patch restart of a running\n"
          "                                  server (network: ADMIN role required)\n"
          "    DUMP DATABASE [TO '<file>']   Export full SQL DDL + INSERT DML script\n"
          "    DIFF SCHEMA '<other_db>'      Compare active schema against another database file\n"
          "    BENCHMARK <n> <query>         Run query <n> times and report latency/QPS metrics\n"
          "    WATCH [INTERVAL <s>] [COUNT <n>] <query>\n"
          "                                  Repeatedly execute and render a live query\n"
          "    ANALYZE [TABLE <t>]           Collect cardinality and column distribution stats\n"
          "    SHOW CLUSTER                  Display Raft consensus role, peers, and WAL state\n"
          "    SHOW USER                     Display active session identity\n"
          "    CHECK INTEGRITY               Verify relational invariants and snapshot health\n"
          "    SELF-TEST [suite]             Run built-in 13-suite enterprise self-test module\n"
          "    VERIFY BACKUP '<file>'        Offline restorability check of a backup snapshot\n"
          "    VERIFY AUDIT                  Verify tamper-evident SHA-256 audit log chain\n"
          "    VACUUM | COMPACT              Compact table storage and rewrite clean snapshot\n"
          "    EXPLAIN [QUERY PLAN] <query>  Output query execution plan and CBO cost estimate\n"
          "    BACKUP                        Trigger immediate verified off-machine backup\n");

    shell_puts("  TRANSACTION CONTROL, DDL, DML, VECTOR SEARCH & DATA INTERCHANGE\n"
          "    BEGIN | COMMIT | ROLLBACK     Explicit atomic transaction control\n"
          "    SAVEPOINT <s>                 Create named rollback savepoint\n"
          "    ROLLBACK TO [SAVEPOINT] <s>   Roll back state to designated savepoint\n"
          "    RELEASE [SAVEPOINT] <s>       Release savepoint marker\n"
          "    CREATE TABLE [IF NOT EXISTS] <t> (<col> NUMBER|VARCHAR2(n)|DATE|VECTOR(dim) ...)\n"
          "    CREATE TABLE <t> AS SELECT ...\n"
          "                                  Create new table pre-populated with query results\n"
          "    CREATE INDEX [IF NOT EXISTS] <name> ON <t> [USING HNSW] (<col>)\n"
          "                                  Create secondary B+ tree or HNSW vector index\n"
          "    ALTER TABLE <t> ADD [COLUMN] <col> <type>\n"
          "                                  Add column to existing table\n"
          "    ALTER TABLE <t> DROP [COLUMN] <col>\n"
          "                                  Drop column from existing table\n"
          "    ALTER TABLE <t> MODIFY [COLUMN] <col> <type>\n"
          "                                  Change column data type or expand length\n"
          "    ALTER TABLE <t> RENAME TO <new_name>\n"
          "                                  Rename table\n"
          "    ALTER TABLE <t> ADD|DROP CONSTRAINT <name> PRIMARY KEY|UNIQUE|FOREIGN KEY ...\n"
          "    DROP TABLE [IF EXISTS] <t>    Drop table and all associated indexes\n"
          "    DROP INDEX <name>             Drop secondary B+ tree index\n"
          "    TRUNCATE TABLE <t>            Remove all rows while preserving table schema\n"
          "    INSERT INTO <t> [(<cols>)] VALUES (...)\n"
          "    INSERT INTO <t> [(<cols>)] SELECT ...\n"
          "    INSERT INTO <t> VALUES (...) ON CONFLICT [(<c>)] DO NOTHING | DO UPDATE SET ...\n"
          "    UPDATE <t> SET <col>=<val> [WHERE <condition>]\n"
          "    DELETE FROM <t> [WHERE <condition>]\n"
          "    WINDOW & JSON FUNCTIONS:      ROW_NUMBER(), RANK(), DENSE_RANK() OVER (PARTITION BY ... ORDER BY ...),\n"
          "                                  JSON_EXTRACT(col, 'path'), JSON_VALUE(col, 'path'), col->'path', col->>'path'\n"
          "    VECTOR OPERATORS & HNSW:      <=> (Cosine Distance), <-> (L2 Distance), USING HNSW\n"
          "                      COSINE_DISTANCE(col, '[...]'), L2_DISTANCE(col, '[...]')\n"
          "    IMPORT WIZARD                 Guided CSV preview, mapping, and import\n"
          "    CREATE WIZARD                 Interactive table builder (no SQL needed)\n"
          "    LOAD SAMPLE                   Load the SAMPLE_DATA practice table\n"
          "    IMPORT TABLE <t> FROM CSV|JSON|MSSQL|ORACLE '<file>' [HEADER]\n"
          "    EXPORT TABLE <t> TO CSV|JSON|MSSQL|ORACLE '<file>'\n"
          "    IMPORT MSSQL|ORACLE [INTO <t>] FROM '<file>'\n"
          "    EXPORT MSSQL|ORACLE [FROM <t>] TO '<file>'\n"
          "================================================================================");
}

static bool parse_path_arg(Parser* p, char* out_path, size_t cap) {
    out_path[0] = '\0';
    if (p->kind == TK_STRING) {
        if (strlen(p->text) >= cap) return false;
        snprintf(out_path, cap, "%s", p->text);
        scan_next_token(p);
        return out_path[0] != '\0';
    }
    if (p->kind == TK_ID || p->kind == TK_SLASH || p->kind == TK_DOT) {
        size_t len = 0;
        while (!p->failed && p->kind != TK_END && p->kind != TK_SEMI &&
               !token_is_keyword(p, "BACKUP") && !token_is_keyword(p, "INTERVAL") &&
               !token_is_keyword(p, "READONLY") && !token_is_keyword(p, "RESTORE")) {
            size_t tlen = strlen(p->text);
            if (len + tlen + 1 < cap) {
                memcpy(out_path + len, p->text, tlen);
                len += tlen;
                out_path[len] = '\0';
            } else {
                return false;
            }
            scan_next_token(p);
        }
        return !p->failed && out_path[0] != '\0';
    }
    return false;
}

static FILE* create_private_report_file(const char* path) {
#ifdef _WIN32
    HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_TEXT);
    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return NULL;
#endif
#ifdef _WIN32
    FILE* file = _fdopen(fd, "w");
#else
    FILE* file = fdopen(fd, "w");
#endif
    if (!file) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }
    return file;
}

static bool execute_support_report(Parser* p) {
    char path[PATH_CAP];
    if (!consume_keyword(p, "REPORT") || !consume_keyword(p, "TO") ||
        !parse_path_arg(p, path, sizeof(path)) || !consume_statement_end(p))
        return report_database_error("expected SUPPORT REPORT TO '<new-file-path>'");

    FILE* file = create_private_report_file(path);
    if (!file)
        return report_database_error("cannot create support report '%s'; it must be a new writable file", path);
    time_t collected_at = time(NULL);
    bool ok = fprintf(file,
        "PC/OpenData support summary\n"
        "Version: %s\n"
        "Collected UTC epoch: %lld\n"
        "Database connected: %s\n"
        "Generation: %llu\n"
        "Pending changes: %s\n"
        "Read-only mode: %s\n"
        "Server mode: %s\n"
        "Storage health: %s\n"
        "Backup health: %s\n"
        "Backup configured: %s\n"
        "Errors reported this process: %d\n"
        "\nSensitive data omitted: database paths, configuration contents, account names,\n"
        "passwords, SQL statements, query history, and row data.\n",
        SERVER_VERSION, (long long)collected_at, g_db ? "yes" : "no",
        (unsigned long long)g_generation, g_pending ? "yes" : "no",
        g_readonly ? "yes" : "no", g_server_mode ? "yes" : "no",
        g_io_uncertain ? "uncertain" : "healthy",
        atomic_load(&g_backup_failed) ? "failed" : "healthy",
        g_backup_dir[0] ? "yes" : "no", atomic_load(&g_errors)) >= 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) return report_database_error("failed to write support report '%s'", path);
    shell_printf("Privacy-preserving support report written to %s.\n", path);
    return true;
}

static bool emit_connection_status(void) {
    char generation[32];
    snprintf(generation, sizeof(generation), "%llu", (unsigned long long)g_generation);
    const char* session = g_network_user[0] ? g_network_user :
                          (g_interactive_shell ? "local interactive shell" : "local process");
    const char* transport = g_network_user[0] ?
        (g_network_tls_active ? "encrypted TLS" : "unencrypted loopback TCP") : "local process";
    const char* service = g_server_mode ? "running" : "not serving network clients";
    const char* database = g_db ? "open" : "not connected";
    const char* storage = g_io_uncertain ? "uncertain; restart required" : "healthy";
    const char* backup = atomic_load(&g_backup_failed) ? "failed" :
                         (g_backup_dir[0] ? "healthy" : "not configured");
    const char* names[] = {"Session", "Transport security", "Service", "Database",
                           "Generation", "Storage health", "Backup health"};
    const char* values[] = {session, transport, service, database, generation, storage, backup};

    if (g_result_capture) {
        QueryResultCapture* capture = g_result_capture;
        const int row_count = (int)(sizeof(names) / sizeof(names[0]));
        if (capture->capacity < row_count) {
            Value (*rows)[MAX_COLUMNS] = realloc(capture->rows,
                (size_t)row_count * sizeof(*capture->rows));
            if (!rows) return report_database_error("unable to allocate connection status output");
            capture->rows = rows;
            capture->capacity = row_count;
        }
        capture->col_count = 2;
        snprintf(capture->col_names[0], sizeof(capture->col_names[0]), "PROPERTY");
        snprintf(capture->col_names[1], sizeof(capture->col_names[1]), "VALUE");
        capture->row_count = row_count;
        for (int row = 0; row < row_count; row++) {
            memset(capture->rows[row], 0, sizeof(capture->rows[row]));
            snprintf(capture->rows[row][0].text, sizeof(capture->rows[row][0].text), "%s", names[row]);
            snprintf(capture->rows[row][1].text, sizeof(capture->rows[row][1].text), "%s", values[row]);
        }
    } else {
        shell_puts("PROPERTY\tVALUE");
        for (size_t row = 0; row < sizeof(names) / sizeof(names[0]); row++)
            shell_printf("%s\t%s\n", names[row], values[row]);
    }
    return true;
}

/* Returns 1 on input, 0 on EOF/cancel, -1 on error. */
static int shell_prompt_line(const char* prompt, char* output, size_t capacity) {
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(output, (int)capacity, stdin))
        return ferror(stdin) ? -1 : 0;
    size_t length = strlen(output);
    bool complete = length > 0 && output[length - 1] == '\n';
    while (length > 0 && (output[length - 1] == '\r' || output[length - 1] == '\n'))
        output[--length] = '\0';
    if (!complete && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        report_database_error("response exceeds the supported length");
        return -1;
    }
    return 1;
}

static bool parse_wizard_column_type(const char* input, char* output, size_t capacity) {
    if (!strcmp(input, "NUMBER") || !strcmp(input, "DATE")) {
        snprintf(output, capacity, "%s", input);
        return true;
    }
    if (!strncmp(input, "VARCHAR2", 8)) {
        long size = 255;
        if (input[8] == '(') {
            char* end = NULL;
            size = strtol(input + 9, &end, 10);
            if (!end || *end != ')' || end[1] != '\0' || size < 1 || size > 255) return false;
        } else if (input[8] != '\0') return false;
        snprintf(output, capacity, "VARCHAR2(%ld)", size);
        return true;
    }
    if (!strncmp(input, "VECTOR(", 7)) {
        char* end = NULL;
        long dim = strtol(input + 7, &end, 10);
        if (!end || *end != ')' || end[1] != '\0' || dim < 1 || dim > 64) return false;
        snprintf(output, capacity, "VECTOR(%ld)", dim);
        return true;
    }
    return false;
}

static bool run_create_table_wizard(Database* db) {
    if (!g_interactive_shell)
        return report_database_error("CREATE WIZARD requires an interactive terminal; write CREATE TABLE directly in scripts");
    if (!db) return report_database_error("connect to a database before using CREATE WIZARD");
    if (g_readonly) return report_database_error("cannot create tables in read-only mode");
    if (db->table_count >= MAX_TABLES) return report_database_error("table limit reached");

    char table_name[MAX_NAME_LEN];
    int r = shell_prompt_line("New table name (blank to cancel): ", table_name, sizeof(table_name));
    if (r < 0) return false;
    if (r == 0 || !table_name[0]) {
        puts("Cancelled; no changes made.");
        return true;
    }
    for (char* ch = table_name; *ch; ch++) *ch = uppercase_ascii(*ch);
    if (!is_valid_identifier(table_name) || !strcmp(table_name, "DUAL"))
        return report_database_error("table name must start with a letter and use only letters, digits, _, $, #");
    if (find_table_index(db, table_name) >= 0)
        return report_database_error("table %s already exists; choose another name", table_name);

    char col_names[MAX_COLUMNS][MAX_NAME_LEN];
    char col_types[MAX_COLUMNS][40];
    int col_count = 0;
    puts("Add columns one at a time. Types: NUMBER, DATE, VARCHAR2(n), VECTOR(dim).");
    while (col_count < MAX_COLUMNS) {
        char prompt[80];
        snprintf(prompt, sizeof(prompt), "Column %d name (blank when done): ", col_count + 1);
        char name[MAX_NAME_LEN];
        r = shell_prompt_line(prompt, name, sizeof(name));
        if (r < 0) return false;
        if (r == 0 || !name[0]) {
            if (col_count == 0) {
                puts("Cancelled; a table needs at least one column.");
                return true;
            }
            break;
        }
        for (char* ch = name; *ch; ch++) *ch = uppercase_ascii(*ch);
        if (!is_valid_identifier(name))
            return report_database_error("column name '%s' is not a valid identifier", name);
        for (int i = 0; i < col_count; i++)
            if (!strcmp(col_names[i], name))
                return report_database_error("duplicate column name '%s'", name);
        snprintf(prompt, sizeof(prompt),
                 "Type for %s [NUMBER/DATE/VARCHAR2(n)/VECTOR(dim)] (default VARCHAR2(255)): ", name);
        char type[64];
        r = shell_prompt_line(prompt, type, sizeof(type));
        if (r < 0) return false;
        if (r == 0) {
            puts("Cancelled; no changes made.");
            return true;
        }
        if (!type[0]) snprintf(type, sizeof(type), "VARCHAR2(255)");
        for (char* ch = type; *ch; ch++) *ch = uppercase_ascii(*ch);
        if (!parse_wizard_column_type(type, col_types[col_count], sizeof(col_types[col_count])))
            return report_database_error("type must be NUMBER, DATE, VARCHAR2(1..255), or VECTOR(1..64)");
        snprintf(col_names[col_count], sizeof(col_names[col_count]), "%s", name);
        col_count++;
    }

    char pk[MAX_NAME_LEN] = {0};
    r = shell_prompt_line("Primary key column (blank for none): ", pk, sizeof(pk));
    if (r < 0) return false;
    if (r == 0) {
        puts("Cancelled; no changes made.");
        return true;
    }
    if (pk[0]) {
        for (char* ch = pk; *ch; ch++) *ch = uppercase_ascii(*ch);
        bool found = false;
        for (int i = 0; i < col_count; i++)
            if (!strcmp(col_names[i], pk)) found = true;
        if (!found)
            return report_database_error("primary key must be one of the columns you defined");
    }

    char sql[MAX_QUERY_LEN];
    int used = snprintf(sql, sizeof(sql), "CREATE TABLE %s (", table_name);
    if (used < 0 || (size_t)used >= sizeof(sql))
        return report_database_error("generated statement exceeds the supported length");
    for (int c = 0; c < col_count; c++) {
        int written = snprintf(sql + used, sizeof(sql) - (size_t)used, "%s%s %s%s",
                               c ? ", " : "", col_names[c], col_types[c],
                               pk[0] && !strcmp(col_names[c], pk) ? " PRIMARY KEY" : "");
        if (written < 0 || (size_t)written >= sizeof(sql) - (size_t)used)
            return report_database_error("generated statement exceeds the supported length");
        used += written;
    }
    int written = snprintf(sql + used, sizeof(sql) - (size_t)used, ");");
    if (written < 0 || (size_t)written >= sizeof(sql) - (size_t)used)
        return report_database_error("generated statement exceeds the supported length");

    printf("About to run:\n  %s\n", sql);
    char confirm[32];
    r = shell_prompt_line("Create this table? [y/N]: ", confirm, sizeof(confirm));
    if (r < 0) return false;
    if (r == 0 || (strcmp(confirm, "y") && strcmp(confirm, "Y") &&
                   strcmp(confirm, "yes") && strcmp(confirm, "YES"))) {
        puts("Cancelled; no changes made.");
        return true;
    }
    bool quit = false;
    bool ok = execute_console_statement(sql, &quit);
    if (ok) {
        if (g_autocommit) shell_printf("Table %s created and saved.\n", table_name);
        else shell_printf("Table %s created. Type COMMIT; to save it permanently.\n", table_name);
        shell_printf("Next: add rows with INSERT INTO %s ... or preview with BROWSE %s;\n",
                     table_name, table_name);
    }
    return ok;
}

static bool load_sample_data(void) {
    if (!g_db)
        return report_database_error("connect to a database first (the default mydata.pcod opens automatically)");
    if (g_readonly) return report_database_error("cannot load sample data in read-only mode");
    if (find_table_index(g_db, "SAMPLE_DATA") >= 0) {
        shell_puts("Sample data is already loaded. Try: BROWSE SAMPLE_DATA;");
        return true;
    }
    static const char* const steps[] = {
        "CREATE TABLE SAMPLE_DATA (ID NUMBER PRIMARY KEY, NAME VARCHAR2(50), CATEGORY VARCHAR2(30), SCORE NUMBER);",
        "INSERT INTO SAMPLE_DATA VALUES (1, 'Ada Lovelace', 'Engineering', 98);",
        "INSERT INTO SAMPLE_DATA VALUES (2, 'Grace Hopper', 'Engineering', 95);",
        "INSERT INTO SAMPLE_DATA VALUES (3, 'Alan Turing', 'Research', 99);",
        "INSERT INTO SAMPLE_DATA VALUES (4, 'Katherine Johnson', 'Mathematics', 97);",
        "INSERT INTO SAMPLE_DATA VALUES (5, 'Edsger Dijkstra', 'Research', 92);"
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        bool quit = false;
        if (!execute_console_statement(steps[i], &quit)) return false;
    }
    shell_puts("Sample table SAMPLE_DATA loaded (5 rows).");
    shell_puts("Try: BROWSE SAMPLE_DATA;  or  SELECT NAME, SCORE FROM SAMPLE_DATA WHERE SCORE > 95;");
    if (!g_autocommit) shell_puts("Type COMMIT; to save the sample data permanently.");
    return true;
}

static bool execute_connect_statement(Parser* p) {
    if (g_network_user[0]) {
        return report_database_error("CONNECT is disabled for network sessions");
    }
    char db_path[PATH_CAP] = {0};
    char backup_dir[PATH_CAP] = {0};
    char restore_path[PATH_CAP] = {0};
    unsigned interval = 0;
    bool readonly = false;

    if (!parse_path_arg(p, db_path, sizeof(db_path))) {
        return report_database_error("expected database file path for CONNECT");
    }

    while (p->kind != TK_END && p->kind != TK_SEMI) {
        if (consume_keyword(p, "BACKUP")) {
            if (p->kind == TK_STRING || p->kind == TK_ID) {
                snprintf(backup_dir, sizeof(backup_dir), "%s", p->text);
                scan_next_token(p);
            } else {
                return report_database_error("expected backup directory path after BACKUP");
            }
        } else if (consume_keyword(p, "INTERVAL")) {
            if (p->kind != TK_NUMBER) return report_database_error("expected seconds for INTERVAL");
            interval = (unsigned)strtoul(p->text, NULL, 10);
            scan_next_token(p);
        } else if (consume_keyword(p, "RESTORE")) {
            if (p->kind == TK_STRING || p->kind == TK_ID) {
                snprintf(restore_path, sizeof(restore_path), "%s", p->text);
                scan_next_token(p);
            } else {
                return report_database_error("expected backup file path after RESTORE");
            }
        } else if (consume_keyword(p, "READONLY")) {
            readonly = true;
        } else {
            return report_database_error("unexpected token in CONNECT statement: %s", p->text);
        }
    }
    consume_token(p, TK_SEMI);

    if (g_db) {
        opendata_close();
    }

    bool ok = false;
    if (readonly) {
        ok = opendata_open_readonly(db_path);
    } else {
        ok = opendata_open(db_path, backup_dir[0] ? backup_dir : NULL, interval, restore_path[0] ? restore_path : NULL);
    }

    if (ok) {
        if (readonly) {
            shell_printf("Connected to database '%s' (READONLY mode, generation %llu).\n", g_path, (unsigned long long)g_generation);
        } else {
            shell_printf("Connected to database '%s' (Generation %llu, %d pending changes).\n", g_path, (unsigned long long)g_generation, g_pending ? 1 : 0);
            if (g_backup_interval > 0 && g_backup_dir[0] != '\0') {
                shell_printf("Periodic backups active every %u seconds -> %s.\n", g_backup_interval, g_backup_dir);
            }
        }
    }
    return ok;
}

static bool execute_create_database_statement(Parser* p) {
    if (g_network_user[0]) {
        return report_database_error("CREATE DATABASE is disabled for network sessions");
    }
    char db_path[PATH_CAP] = {0};
    char backup_dir[PATH_CAP] = {0};
    unsigned interval = 0;

    if (!parse_path_arg(p, db_path, sizeof(db_path))) {
        return report_database_error("expected database file path for CREATE DATABASE");
    }

    while (p->kind != TK_END && p->kind != TK_SEMI) {
        if (consume_keyword(p, "BACKUP")) {
            if (p->kind == TK_STRING || p->kind == TK_ID) {
                snprintf(backup_dir, sizeof(backup_dir), "%s", p->text);
                scan_next_token(p);
            } else {
                return report_database_error("expected backup directory path after BACKUP");
            }
        } else if (consume_keyword(p, "INTERVAL")) {
            if (p->kind != TK_NUMBER) return report_database_error("expected seconds for INTERVAL");
            interval = (unsigned)strtoul(p->text, NULL, 10);
            scan_next_token(p);
        } else {
            return report_database_error("unexpected token in CREATE DATABASE statement: %s", p->text);
        }
    }
    consume_token(p, TK_SEMI);

    char resolved[PATH_CAP];
    if (!resolve_absolute_path(db_path, resolved)) {
        return report_database_error("invalid database path: %s", db_path);
    }

    if (snapshot_path_exists(resolved)) {
        return report_database_error("database '%s' already exists. Use CONNECT '%s'; to open it.", db_path, db_path);
    }

    if (g_db) {
        opendata_close();
    }

    bool ok = opendata_open(db_path, backup_dir[0] ? backup_dir : NULL, interval, NULL);
    if (ok) {
        shell_printf("Database '%s' created successfully.\n", resolved);
        shell_puts("Connected (Generation 0).");
        if (g_backup_interval > 0 && g_backup_dir[0] != '\0') {
            shell_printf("Periodic backups active every %u seconds -> %s.\n", g_backup_interval, g_backup_dir);
        }
    }
    return ok;
}

static bool execute_drop_database_statement(Parser* p) {
    if (g_readonly) return report_database_error("cannot DROP DATABASE in read-only mode");
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
        return report_database_error("permission denied: DROP DATABASE requires the ADMIN role");
    }
    char db_path[PATH_CAP] = {0};
    if (!parse_path_arg(p, db_path, sizeof(db_path)) || !consume_statement_end(p)) {
        return report_database_error("expected database file path for DROP DATABASE");
    }
    char resolved[PATH_CAP];
    if (!resolve_absolute_path(db_path, resolved)) {
        return report_database_error("invalid database path: %s", db_path);
    }
    if (!snapshot_path_exists(resolved)) {
        return report_database_error("database file does not exist: %s", resolved);
    }
    ConfirmationResult confirmation = confirm_destructive_action(
        "DROP DATABASE", resolved, "This permanently deletes the database file and its contents from disk.", "DROP");
    if (confirmation == CONFIRM_DRYRUN) {
        handle_confirmation_with_dryrun(&confirmation, "DROP DATABASE", resolved, "All tables, snapshots, and data files will be deleted.");
    }
    if (!allow_or_report_confirmation(confirmation)) return confirmation == CONFIRM_CANCEL;
    /* Only delete files that are actually PC/OpenData database snapshots. */
    {
        Image probe;
        if (!read_snapshot_file(resolved, &probe)) {
            return report_database_error("refusing to drop unreadable file: %s", resolved);
        }
        bool is_snapshot = probe.size >= 8 &&
            (!memcmp(probe.data, "PCOD0001", 8) ||
             !memcmp(probe.data, "MSQLMDF1", 8) ||
             !memcmp(probe.data, "ORCLDBF1", 8));
        free(probe.data);
        if (!is_snapshot) {
            return report_database_error("refusing to drop file that is not a database snapshot: %s", resolved);
        }
    }
    if (g_db && !strcmp(g_path, resolved)) {
        opendata_close();
    }
    if (remove(resolved) != 0) {
        return report_database_error("unable to remove database file: %s", resolved);
    }
    char lock_path[PATH_CAP];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", resolved) < (int)sizeof(lock_path)) {
        remove(lock_path);
    }
    shell_printf("Database '%s' dropped.\n", resolved);
    return true;
}

static bool execute_disconnect_statement(Parser* p) {
    if (g_network_user[0]) {
        return report_database_error("DISCONNECT is disabled for network sessions");
    }
    if (!consume_statement_end(p)) return false;
    if (!g_db) {
        shell_puts("No active database connection.");
        return true;
    }
    char old_path[PATH_CAP];
    snprintf(old_path, sizeof(old_path), "%s", g_path);
    opendata_close();
    shell_printf("Database '%s' closed. File lock released.\n", old_path);
    return true;
}

static bool execute_source_file_impl(const char* filepath, bool* quit) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        return report_database_error("cannot open script file: %s", filepath);
    }
    shell_printf("Executing script '%s'...\n", filepath);
    char line[4096];
    char* stmt = (char*)malloc(MAX_QUERY_LEN);
    if (!stmt) {
        fclose(f);
        return report_database_error("cannot allocate script statement buffer");
    }
    size_t used = 0;
    stmt[0] = '\0';
    int executed = 0;
    bool all_ok = true;

    while (fgets(line, sizeof(line), f) && !*quit) {
        const char* p = line;
        while (is_sql_whitespace(*p)) p++;
        if (!*p || (*p == '-' && p[1] == '-')) continue;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (used > 0 && used + 1 < MAX_QUERY_LEN) {
            stmt[used++] = ' ';
        }
        if (used + len + 1 >= MAX_QUERY_LEN) {
            free(stmt);
            fclose(f);
            return report_database_error("script statement input limit exceeded");
        }
        memcpy(stmt + used, line, len + 1);
        used += len;

        bool has_semi = false;
        bool in_quote = false;
        for (size_t i = 0; i < used; i++) {
            if (stmt[i] == '\'') in_quote = !in_quote;
            else if (!in_quote && stmt[i] == ';') has_semi = true;
        }
        if (has_semi) {
            if (!execute_console_statement(stmt, quit)) {
                all_ok = false;
            }
            executed++;
            used = 0;
            stmt[0] = '\0';
        }
    }
    if (used > 0 && !*quit) {
        if (!execute_console_statement(stmt, quit)) {
            all_ok = false;
        }
        executed++;
    }
    free(stmt);
    fclose(f);
    shell_printf("Script '%s' completed (%d statement%s executed).\n",
            filepath, executed, executed == 1 ? "" : "s");
    return all_ok;
}

static bool execute_source_file(const char* filepath, bool* quit) {
    if (g_network_user[0]) {
        return report_database_error("script execution is disabled for network sessions");
    }
    static unsigned depth = 0;
    if (depth >= 16) return report_database_error("SOURCE nesting limit exceeded");
    depth++;
    bool was_interactive = g_interactive_shell;
    g_interactive_shell = false;
    bool ok = execute_source_file_impl(filepath, quit);
    g_interactive_shell = was_interactive;
    depth--;
    return ok;
}

static bool execute_spool_statement(Parser* p) {
    if (g_network_user[0]) {
        return report_database_error("SPOOL is disabled for network sessions");
    }
    if (consume_keyword(p, "OFF")) {
        if (!consume_statement_end(p)) return false;
        if (!g_spool_file) {
            shell_puts("Spooling is not currently active.");
            return true;
        }
        char old_spool[PATH_CAP];
        snprintf(old_spool, sizeof(old_spool), "%s", g_spool_path);
        fclose(g_spool_file);
        g_spool_file = NULL;
        g_spool_path[0] = '\0';
        printf("Spooling stopped ('%s').\n", old_spool);
        return true;
    }
    char path[PATH_CAP] = {0};
    if (!parse_path_arg(p, path, sizeof(path)) || !consume_statement_end(p)) {
        if (p->kind == TK_END) {
            if (g_spool_file) printf("Currently spooling to '%s'.\n", g_spool_path);
            else puts("Spooling is OFF.");
            return true;
        }
        return report_database_error("expected file path or OFF after SPOOL");
    }
    if (g_spool_file) {
        fclose(g_spool_file);
        g_spool_file = NULL;
    }
    g_spool_file = fopen(path, "w");
    if (!g_spool_file) {
        g_spool_path[0] = '\0';
        return report_database_error("cannot open spool file for writing: %s", path);
    }
    snprintf(g_spool_path, sizeof(g_spool_path), "%s", path);
    shell_printf("Spooling output to '%s'.\n", g_spool_path);
    return true;
}

static void print_table_ddl(const Table* t) {
    shell_printf("CREATE TABLE %s (\n", t->name);
    for (int c = 0; c < t->col_count; c++) {
        const ColumnDef* col = &t->columns[c];
        char type_buf[32];
        if (col->type == TYPE_VARCHAR2) snprintf(type_buf, sizeof(type_buf), "VARCHAR2(%d)", col->size);
        else if (col->type == TYPE_VECTOR) snprintf(type_buf, sizeof(type_buf), "VECTOR(%d)", col->size);
        else snprintf(type_buf, sizeof(type_buf), "%s", col->type == TYPE_NUMBER ? "NUMBER" : "DATE");

        shell_printf("    %s %s", col->name, type_buf);
        if (t->pk_col_count <= 1 && col->primary) shell_printf(" PRIMARY KEY");
        else {
            if (col->unique) shell_printf(" UNIQUE");
            if (!col->nullable) shell_printf(" NOT NULL");
        }
        if (col->ref_table[0]) {
            shell_printf(" REFERENCES %s(%s)", col->ref_table, col->ref_column);
            if (col->fk_on_delete == FK_ACTION_CASCADE) shell_printf(" ON DELETE CASCADE");
            else if (col->fk_on_delete == FK_ACTION_SET_NULL) shell_printf(" ON DELETE SET NULL");
            if (col->fk_on_update == FK_ACTION_CASCADE) shell_printf(" ON UPDATE CASCADE");
            else if (col->fk_on_update == FK_ACTION_SET_NULL) shell_printf(" ON UPDATE SET NULL");
        }
        if (c + 1 < t->col_count || t->pk_col_count > 1 || t->uq_count > 0) shell_puts(",");
        else shell_puts("");
    }
    if (t->pk_col_count > 1) {
        shell_printf("    PRIMARY KEY (");
        for (int k = 0; k < t->pk_col_count; k++) {
            shell_printf("%s%s", t->columns[t->pk_cols[k]].name, k + 1 < t->pk_col_count ? ", " : "");
        }
        shell_printf(")%s\n", t->uq_count > 0 ? "," : "");
    }
    for (int u = 0; u < t->uq_count; u++) {
        if (t->uq_col_counts[u] > 1) {
            shell_printf("    UNIQUE (");
            for (int k = 0; k < t->uq_col_counts[u]; k++) {
                shell_printf("%s%s", t->columns[t->uq_cols[u][k]].name, k + 1 < t->uq_col_counts[u] ? ", " : "");
            }
            shell_printf(")%s\n", u + 1 < t->uq_count ? "," : "");
        }
    }
    shell_printf(")");
    if (t->engine_type == ENGINE_COLUMNAR) {
        shell_printf(" ENGINE = COLUMNAR");
    } else if (t->engine_type == ENGINE_DISK) {
        shell_printf(" ENGINE = DISK");
    }
    if (t->part_type == PART_HASH && t->part_col_idx >= 0 && t->part_col_idx < t->col_count) {
        shell_printf(" PARTITION BY HASH (%s) PARTITIONS %d", t->columns[t->part_col_idx].name, t->part_count);
    } else if (t->part_type == PART_RANGE && t->part_col_idx >= 0 && t->part_col_idx < t->col_count) {
        shell_printf(" PARTITION BY RANGE (%s) (", t->columns[t->part_col_idx].name);
        for (int p = 0; p < t->part_count; p++) {
            if (p > 0) shell_printf(", ");
            if (t->partitions[p].is_maxvalue) {
                shell_printf("PARTITION %s VALUES LESS THAN (MAXVALUE)", t->partitions[p].name);
            } else if (t->columns[t->part_col_idx].type == TYPE_NUMBER) {
                shell_printf("PARTITION %s VALUES LESS THAN (%s)", t->partitions[p].name, t->partitions[p].upper_bound);
            } else {
                shell_printf("PARTITION %s VALUES LESS THAN ('%s')", t->partitions[p].name, t->partitions[p].upper_bound);
            }
        }
        shell_printf(")");
    }
    shell_puts(";");
}

bool is_supported_db_filename(const char* name) {
    size_t len = strlen(name);
    static const char* exts[] = { ".pcod", ".mdf", ".ndf", ".bak", ".dbf", ".ora", ".dmp" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t elen = strlen(exts[i]);
        if (len > elen) {
            const char* tail = name + (len - elen);
            bool match = true;
            for (size_t k = 0; k < elen; k++) {
                if (uppercase_ascii(tail[k]) != uppercase_ascii(exts[i][k])) { match = false; break; }
            }
            if (match) return true;
        }
    }
    return false;
}

static void print_engine_status(void) {
    if (!g_db) {
        shell_puts("Engine Status: Offline (no database connected).");
        return;
    }
    size_t row_mem_bytes = sizeof(Database);
    int total_rows = 0;
    int total_capacity = 0;
    int total_idx_entries = 0;
    int total_dead_rows = 0;
    uint64_t total_vacuumed = 0;
    int partitioned_tables = 0;
    for (int t = 0; t < g_db->table_count; t++) {
        total_rows += g_db->tables[t].row_count;
        total_capacity += g_db->tables[t].capacity;
        total_dead_rows += g_db->tables[t].dead_row_count;
        total_vacuumed += g_db->tables[t].total_vacuumed_rows;
        if (g_db->tables[t].part_type != PART_NONE) partitioned_tables++;
        row_mem_bytes += (size_t)g_db->tables[t].capacity * sizeof(Row);
    }
    for (int i = 0; i < g_db->index_count; i++) {
        total_idx_entries += g_db->indexes[i].entry_count;
        row_mem_bytes += (size_t)g_db->indexes[i].capacity * sizeof(IndexEntry);
    }
    shell_puts("METRIC\tVALUE");
    shell_printf("ENGINE_VERSION\t%s\n", SERVER_VERSION);
    shell_printf("DATABASE_PATH\t%s\n", g_path[0] ? g_path : "(memory)");
    shell_printf("GENERATION\t%llu\n", (unsigned long long)g_generation);
    shell_printf("USER_TABLES\t%d / %d\n", g_db->table_count, MAX_TABLES);
    shell_printf("PARTITIONED_TABLES\t%d\n", partitioned_tables);
    shell_printf("TOTAL_ROWS\t%d / %d (allocated capacity: %d)\n", total_rows, MAX_TOTAL_ROWS, total_capacity);
    shell_printf("MVCC_DEAD_ROW_VERSIONS\t%d (reclaimed total: %llu)\n", total_dead_rows, (unsigned long long)total_vacuumed);
    shell_printf("VALUE_LIMIT\t%d bytes per value\n", MAX_VAL_LEN);
    shell_printf("SECONDARY_INDEXES\t%d (%d indexed keys)\n", g_db->index_count, total_idx_entries);
    shell_printf("MEMORY_ALLOCATED_BYTES\t%zu\n", row_mem_bytes);
    shell_printf("BUFFER_POOL_FRAMES\t%d (%d bytes/page)\n", BUFFER_POOL_FRAMES, PAGE_SIZE);
    shell_printf("TRANSACTION_STATE\t%s\n", g_pending ? "PENDING_CHANGES" : "COMMITTED_CLEAN");
    shell_printf("IO_HEALTH\t%s\n", g_io_uncertain ? "UNCERTAIN" : "HEALTHY");
}

static void show_databases_in_dir(void) {
    shell_puts("DATABASE_FILE\tSIZE_BYTES\tSTATUS");
    int count = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_supported_db_filename(fd.cFileName)) {
                unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                char abs_p[PATH_CAP];
                bool is_active = false;
                if (g_db && resolve_absolute_path(fd.cFileName, abs_p) && !strcmp(abs_p, g_path)) {
                    is_active = true;
                }
                char sz_str[32]; format_human_size(sz, sz_str, sizeof(sz_str)); shell_printf("%s\t%s\t%s\n", fd.cFileName, sz_str, is_active ? "CONNECTED" : "AVAILABLE");
                count++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* dir = opendir(".");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (is_supported_db_filename(ent->d_name)) {
                struct stat st;
                unsigned long long sz = 0;
                if (stat(ent->d_name, &st) == 0) sz = (unsigned long long)st.st_size;
                char abs_p[PATH_CAP];
                bool is_active = false;
                if (g_db && resolve_absolute_path(ent->d_name, abs_p) && !strcmp(abs_p, g_path)) {
                    is_active = true;
                }
                shell_printf("%s\t%llu\t%s\n", ent->d_name, sz, is_active ? "CONNECTED" : "AVAILABLE");
                count++;
            }
        }
        closedir(dir);
    }
#endif
    if (g_db && g_path[0]) {
        bool already_listed = (count > 0);
        if (!already_listed) {
            FILE* f = fopen(g_path, "rb");
            unsigned long long sz = 0;
            if (f) {
                fseek(f, 0, SEEK_END);
                long pos = ftell(f);
                if (pos > 0) sz = (unsigned long long)pos;
                fclose(f);
            }
            shell_printf("%s\t%llu\tCONNECTED\n", g_path, sz);
            count++;
        }
    }
    shell_printf("\n%d database%s found.\n", count, count == 1 ? "" : "s");
}

static bool execute_check_integrity(void) {
    if (!g_db) return report_database_error("not connected to any database");
    bool rel_ok = validate_relational_integrity(g_db);
    int total_rows = 0;
    int total_cols = 0;
    int total_constraints = 0;
    for (int t = 0; t < g_db->table_count; t++) {
        const Table* tbl = &g_db->tables[t];
        total_rows += tbl->row_count;
        total_cols += tbl->col_count;
        for (int c = 0; c < tbl->col_count; c++) {
            if (tbl->columns[c].primary) total_constraints++;
            if (tbl->columns[c].unique) total_constraints++;
            if (!tbl->columns[c].nullable) total_constraints++;
            if (tbl->columns[c].ref_table[0]) total_constraints++;
        }
    }
    shell_puts("INTEGRITY_CHECK\tSTATUS\tDETAILS");
    shell_printf("RELATIONAL_INVARIANTS\t%s\t%d tables, %d columns, %d rows, %d constraints verified\n",
            rel_ok ? "OK" : "FAILED", g_db->table_count, total_cols, total_rows, total_constraints);
    shell_printf("STORAGE_ENGINE\t%s\tGeneration %llu (%s)\n",
            g_io_uncertain ? "UNCERTAIN" : "OK",
            (unsigned long long)g_generation,
            g_pending ? "uncommitted changes pending" : "clean snapshot");
    return rel_ok && !g_io_uncertain;
}

static bool verify_backup_file(const char* path) {
    Image image;
    if (!read_snapshot_file(path, &image))
        return report_database_error("cannot read backup file: %s", path);
    uint64_t identity = 0, generation = 0;
    Database* check = decode_database_snapshot(&image, &identity, &generation);
    free(image.data);
    if (!check)
        return report_database_error("backup file is corrupt or not a PC/OpenData snapshot: %s", path);
    int tables = check->table_count;
    int rows = 0;
    for (int t = 0; t < tables; t++) rows += check->tables[t].row_count;
    bool rel_ok = validate_relational_integrity(check);
    destroy_database(check);
    if (!rel_ok)
        return report_database_error("backup file failed relational integrity checks: %s", path);
    shell_printf("Backup verified: %s\n  Generation %llu, %d tables, %d rows. Safe to restore with:\n"
                 "  opendata --database restored.pcod --restore '%s'\n",
                 path, (unsigned long long)generation, tables, rows, path);
    return true;
}

bool execute_vacuum_command(const char* table_name, bool full, bool analyze) {
    if (!g_db) return report_database_error("not connected to any database");
    if (g_readonly) return report_database_error("cannot VACUUM in read-only mode");
    if (g_raft_role == RAFT_FOLLOWER) return report_database_error("follower nodes accept read-only queries");

    int t_start = 0, t_end = g_db->table_count;
    if (table_name && table_name[0]) {
        int idx = find_table_index(g_db, table_name);
        if (idx < 0) return report_database_error("table does not exist: %s", table_name);
        t_start = idx;
        t_end = idx + 1;
    }

    int compacted_tables = 0;
    size_t total_freed_bytes = 0;
    int total_reclaimed_versions = 0;

    for (int t = t_start; t < t_end; t++) {
        size_t freed = 0;
        int reclaimed = 0;
        if (!vacuum_table_storage(g_db, t, full, &freed, &reclaimed)) {
            return false;
        }
        if (freed > 0 || reclaimed > 0 || full) compacted_tables++;
        total_freed_bytes += freed;
        total_reclaimed_versions += reclaimed;
        if (analyze) {
            execute_analyze_table(g_db->tables[t].name);
        }
    }

    Image image = {0};
    if (!encode_database_snapshot(g_db, g_identity, g_generation, &image)) {
        free(image.data);
        return report_database_error("VACUUM snapshot encoding failed");
    }
    size_t snap_bytes = image.size;
    if (!g_pending && g_path[0]) {
        lock_snapshot_io();
        bool ok = publish_durable_snapshot(g_path, &image, true, &g_io_uncertain);
        unlock_snapshot_io();
        free(image.data);
        if (!ok) return report_database_error("VACUUM failed to rewrite snapshot");
    } else {
        free(image.data);
    }
    shell_printf("VACUUM complete. Compacted %d table%s, reclaimed %d dead MVCC row version%s, freed %zu bytes (Snapshot size: %zu bytes, Generation %llu).\n",
            compacted_tables, compacted_tables == 1 ? "" : "s",
            total_reclaimed_versions, total_reclaimed_versions == 1 ? "" : "s",
            total_freed_bytes, snap_bytes, (unsigned long long)g_generation);
    return true;
}

static bool explain_statement(Parser* p) {
    consume_keyword(p, "QUERY");
    consume_keyword(p, "PLAN");
    char subcmd[MAX_NAME_LEN];
    if (!parse_identifier(p, subcmd)) return report_database_error("expected statement after EXPLAIN");
    shell_puts("QUERY PLAN");
    if (!strcmp(subcmd, "SELECT")) {
        bool has_distinct = consume_keyword(p, "DISTINCT");
        while (!p->failed && p->kind != TK_END && !token_is_keyword(p, "FROM")) scan_next_token(p);
        if (!consume_keyword(p, "FROM")) return false;
        char tbl[MAX_NAME_LEN];
        if (!parse_identifier(p, tbl)) return false;
        int t_idx = find_table_index(g_db, tbl);
        Table* t_obj = t_idx >= 0 ? &g_db->tables[t_idx] : NULL;

        char right[MAX_NAME_LEN] = {0};
        bool has_join = false;
        bool is_left = false;
        if (p->kind == TK_ID && !token_is_keyword(p, "WHERE") && !token_is_keyword(p, "LEFT") &&
            !token_is_keyword(p, "INNER") && !token_is_keyword(p, "JOIN") && !token_is_keyword(p, "GROUP") &&
            !token_is_keyword(p, "ORDER") && !token_is_keyword(p, "LIMIT")) {
            scan_next_token(p); /* optional table alias */
        }
        is_left = consume_keyword(p, "LEFT");
        if (is_left) consume_keyword(p, "OUTER");
        else consume_keyword(p, "INNER");
        if (is_left || consume_keyword(p, "JOIN")) {
            if (parse_identifier(p, right)) {
                has_join = true;
            }
        }

        while (!p->failed && p->kind != TK_END && !token_is_keyword(p, "WHERE") && !token_is_keyword(p, "GROUP") &&
               !token_is_keyword(p, "ORDER") && !token_is_keyword(p, "LIMIT")) scan_next_token(p);

        Filter* filter = calloc(1, sizeof(Filter));
        bool has_where = false;
        if (t_obj && filter && token_is_keyword(p, "WHERE")) {
            Scope sc = {0};
            sc.tables[0] = t_obj;
            sc.count = 1;
            Parser wp = *p;
            if (parse_where_clause(&wp, &sc, filter) && filter->node_count > 0) {
                has_where = true;
            }
        }
        if (consume_keyword(p, "WHERE")) {
            has_where = true;
        }

        bool cbo_use_idx = false;
        int sec_idx = -1;
        int est_rows = t_obj ? t_obj->row_count : 0;
        double est_cost = t_obj ? cbo_estimate_query_cost(g_db, t_obj, 0, has_where ? filter : NULL, &cbo_use_idx, &sec_idx, &est_rows) : 1.0;

        char part_info[96] = {0};
        if (t_obj && t_obj->part_type != PART_NONE && t_obj->part_count > 0) {
            uint32_t mask = prune_partitions_for_filter(t_obj, 0, has_where ? filter : NULL);
            int active_p = 0;
            for (int pi = 0; pi < t_obj->part_count; pi++) {
                if (mask & (1u << (unsigned)pi)) active_p++;
            }
            snprintf(part_info, sizeof(part_info), " [Partition Pruning: %d/%d partitions]", active_p, t_obj->part_count);
        }

        if (t_obj && cbo_use_idx && sec_idx >= 0) {
            shell_printf("1. INDEX RANGE SCAN %s USING %s [CBO Est Rows: %d, Cost: %.2f]%s\n",
                         tbl, g_db->indexes[sec_idx].name, est_rows, est_cost, part_info);
        } else if (t_obj && t_obj->engine_type == ENGINE_COLUMNAR) {
            shell_printf("1. COLUMNAR VECTOR SCAN %s [CBO Est Rows: %d, Cost: %.2f]%s\n",
                         tbl, est_rows, est_cost, part_info);
        } else if (t_obj && t_obj->stats.is_analyzed) {
            shell_printf("1. SCAN %s [CBO Est Rows: %d, Cost: %.2f]%s\n",
                         tbl, est_rows, est_cost, part_info);
        } else {
            shell_printf("1. SCAN %s%s\n", tbl, part_info);
        }
        free(filter);

        int step = 2;
        if (has_join) {
            shell_printf("%d. %s JOIN %s (using JoinIndex hash acceleration)\n", step++, is_left ? "LEFT" : "HASH", right);
        }
        if (has_where) {
            shell_printf("%d. FILTER (WHERE condition)\n", step++);
        }
        while (!p->failed && p->kind != TK_END && !token_is_keyword(p, "GROUP") && !token_is_keyword(p, "ORDER") && !token_is_keyword(p, "LIMIT")) scan_next_token(p);
        if (consume_keyword(p, "GROUP")) {
            consume_keyword(p, "BY");
            shell_printf("%d. GROUP BY & AGGREGATE\n", step++);
        }
        if (has_distinct) {
            shell_printf("%d. DISTINCT (in-memory deduplication)\n", step++);
        }
        while (!p->failed && p->kind != TK_END && !token_is_keyword(p, "ORDER") && !token_is_keyword(p, "LIMIT")) scan_next_token(p);
        if (consume_keyword(p, "ORDER")) {
            consume_keyword(p, "BY");
            shell_printf("%d. ORDER BY (in-memory quicksort)\n", step++);
        }
        while (!p->failed && p->kind != TK_END && !token_is_keyword(p, "LIMIT") && !token_is_keyword(p, "OFFSET")) scan_next_token(p);
        if (consume_keyword(p, "LIMIT") || consume_keyword(p, "OFFSET")) {
            shell_printf("%d. LIMIT / OFFSET (slice window)\n", step++);
        }
        return !p->failed;
    }
    if (!strcmp(subcmd, "INSERT")) {
        shell_puts("1. PARSE VALUES\n2. VALIDATE CONSTRAINTS\n3. APPEND TO TABLE STORAGE");
        return true;
    }
    if (!strcmp(subcmd, "UPDATE")) {
        shell_puts("1. SCAN TABLE\n2. FILTER WHERE\n3. MODIFY ROWS IN-PLACE (CoW copy on write)\n4. VALIDATE CONSTRAINTS");
        return true;
    }
    if (!strcmp(subcmd, "DELETE")) {
        shell_puts("1. SCAN TABLE\n2. FILTER WHERE\n3. PACK ROWS\n4. VALIDATE FOREIGN KEYS");
        return true;
    }
    shell_printf("1. EXECUTE %s\n", subcmd);
    return true;
}

static bool execute_console_statement_impl(const char* text, bool* quit) {
    while (is_sql_whitespace(*text)) text++;
    if (!*text) return true;

    Parser history_parser = {0};
    history_parser.cursor = text;
    scan_next_token(&history_parser);
    bool sensitive_user_command = false;
    while (history_parser.kind != TK_END && history_parser.kind != TK_BAD &&
           !history_parser.failed) {
        if (token_is_keyword(&history_parser, "CREATE") ||
            token_is_keyword(&history_parser, "ALTER") ||
            token_is_keyword(&history_parser, "DROP")) {
            Parser lookahead = history_parser;
            scan_next_token(&lookahead);
            if (token_is_keyword(&lookahead, "USER")) {
                sensitive_user_command = true;
                break;
            }
        }
        scan_next_token(&history_parser);
    }
    if (!sensitive_user_command) record_history_entry(text);

    if (text[0] == '\\' || text[0] == '.') {
        const char* cmd = text;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (!strcmp(cmd, "\\dt") || !strcmp(cmd, ".tables")) {
            shell_puts("TABLE_NAME\tCOLUMNS\tROWS");
            shell_puts("DUAL\t1\t1");
            if (g_db) {
                for (int t = 0; t < g_db->table_count; t++)
                    shell_printf("%s\t%d\t%d\n", g_db->tables[t].name, g_db->tables[t].col_count, g_db->tables[t].row_count);
            }
            return true;
        }
        if (!strncmp(cmd, "\\d", 2) || !strncmp(cmd, ".schema", 7)) {
            const char* arg = cmd + (cmd[1] == 'd' ? 2 : 7);
            while (*arg == ' ' || *arg == '\t') arg++;
            char tbl_name[MAX_NAME_LEN] = {0};
            size_t ai = 0;
            while (*arg && !is_sql_whitespace(*arg) && *arg != ';' && ai + 1 < sizeof(tbl_name)) {
                tbl_name[ai++] = *arg++;
            }
            tbl_name[ai] = '\0';
            if (tbl_name[0]) {
                if (!g_db) return report_database_error("not connected to any database");
                int idx = find_table_index(g_db, tbl_name);
                if (idx < 0) {
                    report_table_not_found(g_db, tbl_name);
                    return false;
                }
                print_table_ddl(&g_db->tables[idx]);
            } else {
                if (!g_db || g_db->table_count == 0) shell_puts("-- No user tables defined.");
                else {
                    for (int t = 0; t < g_db->table_count; t++) {
                        print_table_ddl(&g_db->tables[t]);
                        if (t + 1 < g_db->table_count) shell_puts("");
                    }
                }
            }
            return true;
        }
        if (!strcmp(cmd, "\\l")) {
            show_databases_in_dir();
            return true;
        }
        if (!strcmp(cmd, "\\du")) {
            return execute_console_statement("SHOW USERS;", quit);
        }
        if (!strncmp(cmd, "\\i", 2) || !strncmp(cmd, ".read", 5)) {
            const char* path = cmd + (cmd[1] == 'i' ? 2 : 5);
            while (*path == ' ' || *path == '\t' || *path == '\'' || *path == '"') path++;
            char clean_path[PATH_CAP] = {0};
            size_t pi = 0;
            while (*path && *path != ';' && *path != '\'' && *path != '"' && pi + 1 < sizeof(clean_path)) {
                clean_path[pi++] = *path++;
            }
            while (pi > 0 && is_sql_whitespace(clean_path[pi - 1])) clean_path[--pi] = '\0';
            if (!clean_path[0]) return report_database_error("expected file path after script command");
            return execute_source_file(clean_path, quit);
        }
        if (!strcmp(cmd, "\\q") || !strcmp(cmd, ".exit")) {
            if (g_spool_file) { fclose(g_spool_file); g_spool_file = NULL; }
            *quit = true;
            return true;
        }
        if (!strcmp(cmd, "\\x")) {
            static OutputFormat s_prev_format = FORMAT_TABLE;
            if (g_output_format == FORMAT_VERTICAL) {
                g_output_format = s_prev_format;
                shell_puts("Expanded display is off.");
            } else {
                s_prev_format = g_output_format;
                g_output_format = FORMAT_VERTICAL;
                shell_puts("Expanded display is on.");
            }
            return true;
        }
        if (!strcmp(cmd, ".mode")) {
            const char* cur = "table";
            switch (g_output_format) {
                case FORMAT_BOX: cur = "box"; break;
                case FORMAT_MARKDOWN: cur = "markdown"; break;
                case FORMAT_VERTICAL: cur = "vertical"; break;
                case FORMAT_TSV: cur = "tsv"; break;
                case FORMAT_JSON: cur = "json"; break;
                case FORMAT_CSV: cur = "csv"; break;
                default: cur = "table"; break;
            }
            shell_printf("current mode: %s (available: table, box, markdown, vertical, tsv, json, csv, list | pager: %s)\n", cur, g_pager_enabled ? "on" : "off");
            return true;
        }
        if (!strncmp(cmd, ".mode pager on", 14) || !strcmp(cmd, ".pager on")) { g_pager_enabled = true; shell_puts("pager: on"); return true; }
        if (!strncmp(cmd, ".mode pager off", 15) || !strcmp(cmd, ".pager off")) { g_pager_enabled = false; shell_puts("pager: off"); return true; }
        if (!strcmp(cmd, ".pager")) { shell_printf("pager: %s\n", g_pager_enabled ? "on" : "off"); return true; }
        if (!strncmp(cmd, ".mode list", 10) || !strncmp(cmd, ".mode tabs", 10)) { g_output_format = FORMAT_TSV; shell_puts("format: tsv"); return true; }
        if (!strncmp(cmd, ".mode box", 9)) { g_output_format = FORMAT_BOX; shell_puts("format: box"); return true; }
        if (!strncmp(cmd, ".mode markdown", 14) || !strncmp(cmd, ".mode md", 8)) { g_output_format = FORMAT_MARKDOWN; shell_puts("format: markdown"); return true; }
        if (!strncmp(cmd, ".mode vertical", 14)) { g_output_format = FORMAT_VERTICAL; shell_puts("format: vertical"); return true; }
        if (!strncmp(cmd, ".mode tsv", 9)) { g_output_format = FORMAT_TSV; shell_puts("format: tsv"); return true; }
        if (!strncmp(cmd, ".mode json", 10)) { g_output_format = FORMAT_JSON; shell_puts("format: json"); return true; }
        if (!strncmp(cmd, ".mode csv", 9)) { g_output_format = FORMAT_CSV; shell_puts("format: csv"); return true; }
        if (!strncmp(cmd, ".mode table", 11)) { g_output_format = FORMAT_TABLE; shell_puts("format: table"); return true; }
        if (!strncmp(cmd, ".selftest", 9) || !strncmp(cmd, ".self-test", 10)) {
            const char* arg = cmd + (cmd[5] == '-' ? 10 : 9);
            while (*arg == ' ' || *arg == '\t') arg++;
            char filter[64] = {0};
            size_t fi = 0;
            while (*arg && !is_sql_whitespace(*arg) && *arg != ';' && fi + 1 < sizeof(filter)) filter[fi++] = *arg++;
            return execute_self_test(filter[0] ? filter : NULL);
        }
    }
    if (*text == '@') {
        Parser sp = {0};
        sp.cursor = text + 1;
        scan_next_token(&sp);
        char script_path[PATH_CAP] = {0};
        if (!parse_path_arg(&sp, script_path, sizeof(script_path)) || !consume_statement_end(&sp)) {
            return report_database_error("expected script path after '@'");
        }
        return execute_source_file(script_path, quit);
    }

    /* exit! / quit! force-quit even with uncommitted changes. Handle before
       tokenizing because '!' is not a valid SQL token. */
    {
        const char* scan = text;
        while (is_sql_whitespace(*scan)) scan++;
        if (!strncmp(scan, "exit!", 5) || !strncmp(scan, "quit!", 5) ||
            !strncmp(scan, "EXIT!", 5) || !strncmp(scan, "QUIT!", 5)) {
            if (g_spool_file) {
                fclose(g_spool_file);
                g_spool_file = NULL;
            }
            *quit = true;
            return true;
        }
    }

    Parser p = {0}; p.cursor = text; scan_next_token(&p);
    if (p.kind == TK_END) return true;
    char command[MAX_NAME_LEN];
    if (consume_token(&p, TK_HELP)) memcpy(command, "HELP", sizeof("HELP"));
    else if (!parse_identifier(&p, command)) return report_database_error("invalid command or token");

    if (!strcmp(command, "SELECT") || !strcmp(command, "INSERT") || !strcmp(command, "UPDATE") ||
        !strcmp(command, "DELETE") || !strcmp(command, "CREATE") || !strcmp(command, "ALTER") ||
        !strcmp(command, "WITH")) {
        snprintf(g_last_sql, sizeof(g_last_sql), "%s", text);
    }

    double start = monotonic_seconds();
    bool ok = true; int before_errors = g_errors;
    if (!strcmp(command, "CONNECTION")) {
        ok = consume_keyword(&p, "STATUS") && consume_statement_end(&p);
        if (ok) ok = emit_connection_status();
    } else if (!strcmp(command, "CONNECT") || !strcmp(command, "USE") || !strcmp(command, "OPEN")) {
        ok = execute_connect_statement(&p);
    } else if (!strcmp(command, "DISCONNECT")) {
        ok = execute_disconnect_statement(&p);
    } else if (!strcmp(command, "EXIT") || !strcmp(command, "QUIT")) {
        if (!consume_statement_end(&p)) ok = false;
        else if (g_pending && g_db) {
            shell_puts("You have uncommitted changes. Type COMMIT; to save them, ROLLBACK; to discard, or exit!; to quit without saving.");
        } else {
            if (g_spool_file) {
                fclose(g_spool_file);
                g_spool_file = NULL;
            }
            *quit = true;
        }
    } else if (!strcmp(command, "HELP")) {
        char topic[MAX_NAME_LEN] = {0};
        if (p.kind == TK_ID) {
            snprintf(topic, sizeof(topic), "%s", p.text);
            scan_next_token(&p);
        }
        if (!consume_statement_end(&p)) ok = false;
        else if (topic[0]) print_topic_help(topic);
        else print_command_help();
    } else if (!strcmp(command, "GUIDE")) {
        ok = consume_statement_end(&p);
        if (ok) print_getting_started_guide();
    } else if (!strcmp(command, "CLEAR") || !strcmp(command, "CLS")) {
        if (!consume_statement_end(&p)) ok = false; else fputs("\033[2J\033[H", stdout);
    } else if (!strcmp(command, "PING")) {
        ok = consume_statement_end(&p);
        if (ok) shell_puts("PONG");
    } else if (!strcmp(command, "SELFTEST") || !strcmp(command, "SELF_TEST") ||
               (!strcmp(command, "SELF") && (consume_token(&p, TK_MINUS), consume_keyword(&p, "TEST")))) {
        char suite_filter[MAX_NAME_LEN] = {0};
        if (p.kind == TK_ID || p.kind == TK_STRING) {
            snprintf(suite_filter, sizeof(suite_filter), "%s", p.text);
            scan_next_token(&p);
        }
        ok = consume_statement_end(&p) && execute_self_test(suite_filter[0] ? suite_filter : NULL);
    } else if (!strcmp(command, "SOURCE")) {
        char script_path[PATH_CAP] = {0};
        if (!parse_path_arg(&p, script_path, sizeof(script_path)) || !consume_statement_end(&p)) {
            return report_database_error("expected script file path after SOURCE");
        }
        ok = execute_source_file(script_path, quit);
    } else if (!strcmp(command, "SPOOL")) {
        ok = execute_spool_statement(&p);
    } else if (!strcmp(command, "HISTORY")) {
        ok = consume_statement_end(&p);
        if (ok) {
            for (int i = 0; i < g_history_count; i++) {
                shell_printf("%4d  %s\n", i + 1, g_history[i]);
            }
        }
    } else if (!strcmp(command, "IRIS") || !strcmp(command, "EDIT") || !strcmp(command, "ED")) {
        if (g_network_user[0]) {
            return report_database_error("interactive editor is disabled for network sessions");
        }
        char edit_file[PATH_CAP] = {0};
        if (p.kind != TK_END && p.kind != TK_SEMI) {
            if (!parse_path_arg(&p, edit_file, sizeof(edit_file))) {
                return report_database_error("invalid file path for %s", command);
            }
        }
        if (!consume_statement_end(&p)) return report_database_error("unexpected token after %s", command);
        bool exec_after = false;
        if (iris_run_interactive(edit_file[0] ? edit_file : NULL,
                                 g_last_sql,
                                 sizeof(g_last_sql),
                                 &exec_after)) {
            if (exec_after && g_last_sql[0]) {
                shell_printf("Executing IRIS buffer: %s\n", g_last_sql);
                ok = execute_console_statement(g_last_sql, quit);
            }
        }
    } else if (!strcmp(command, "SET")) {
        if (consume_keyword(&p, "TIMING")) {
            if (consume_keyword(&p, "ON")) { ok = consume_statement_end(&p); if (ok) { g_timing = true; shell_puts("Timing is ON."); } }
            else if (consume_keyword(&p, "OFF")) { ok = consume_statement_end(&p); if (ok) { g_timing = false; shell_puts("Timing is OFF."); } }
            else ok = false;
        } else if (consume_keyword(&p, "FORMAT")) {
            if (consume_keyword(&p, "JSON")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_JSON; }
            else if (consume_keyword(&p, "CSV")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_CSV; }
            else if (consume_keyword(&p, "TABLE")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_TABLE; }
            else if (consume_keyword(&p, "BOX")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_BOX; }
            else if (consume_keyword(&p, "MARKDOWN") || consume_keyword(&p, "MD")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_MARKDOWN; }
            else if (consume_keyword(&p, "VERTICAL")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_VERTICAL; }
            else if (consume_keyword(&p, "TSV") || consume_keyword(&p, "LIST") || consume_keyword(&p, "TABS")) { ok = consume_statement_end(&p); if (ok) g_output_format = FORMAT_TSV; }
            else ok = false;
        } else if (consume_keyword(&p, "AUTOCOMMIT")) {
            if (consume_keyword(&p, "ON")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_autocommit = true;
                    shell_puts("Autocommit is ON.");
                    if (g_db && g_pending) ok = commit_transaction();
                }
            } else if (consume_keyword(&p, "OFF")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_autocommit = false;
                    shell_puts("Autocommit is OFF.");
                }
            } else ok = false;
        } else if (consume_keyword(&p, "NULL")) {
            if (p.kind == TK_STRING || p.kind == TK_ID) {
                char null_buf[32];
                snprintf(null_buf, sizeof(null_buf), "%s", p.text);
                scan_next_token(&p);
                ok = consume_statement_end(&p);
                if (ok) {
                    snprintf(g_null_str, sizeof(g_null_str), "%s", null_buf);
                    shell_printf("NULL display string set to \"%s\".\n", g_null_str);
                }
            } else ok = false;
        } else if (consume_keyword(&p, "MAXROWS")) {
            if (p.kind == TK_NUMBER) {
                long mr = strtol(p.text, NULL, 10);
                scan_next_token(&p);
                ok = (mr >= 0) && consume_statement_end(&p);
                if (ok) {
                    g_max_rows = (int)mr;
                    shell_printf("MAXROWS set to %d.\n", g_max_rows);
                }
            } else ok = false;
        } else if (consume_keyword(&p, "SEMICOLON")) {
            if (!consume_keyword(&p, "OPTIONAL")) ok = false;
            else if (consume_keyword(&p, "ON")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_semicolon_optional = true;
                    shell_puts("Semicolons are now optional for complete single-line statements.");
                }
            } else if (consume_keyword(&p, "OFF")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_semicolon_optional = false;
                    shell_puts("Semicolons are required.");
                }
            } else ok = false;
        } else if (consume_keyword(&p, "CONFIRM_DESTRUCTIVE") || consume_keyword(&p, "CONFIRM") || consume_keyword(&p, "SAFE_MODE")) {
            if (consume_keyword(&p, "ON")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_confirm_destructive = true;
                    shell_puts("Destructive command confirmation is ON.");
                }
            } else if (consume_keyword(&p, "OFF")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_confirm_destructive = false;
                    shell_puts("Destructive command confirmation is OFF (prompts disabled).");
                }
            } else ok = false;
        } else if (consume_keyword(&p, "STATEMENT_TIMEOUT")) {
            consume_token(&p, TK_EQ);
            if (p.kind == TK_NUMBER) {
                long to_val = strtol(p.text, NULL, 10);
                scan_next_token(&p);
                ok = (to_val >= 0) && consume_statement_end(&p);
                if (ok) {
                    g_statement_timeout_ms = (uint32_t)to_val;
                    shell_printf("STATEMENT_TIMEOUT set to %u ms.\n", g_statement_timeout_ms);
                }
            } else ok = false;
        } else if (consume_keyword(&p, "BUFFER_POOL") || consume_keyword(&p, "BUFFER_POOL_SIZE")) {
            consume_token(&p, TK_EQ);
            if (p.kind == TK_NUMBER) {
                long mb_val = strtol(p.text, NULL, 10);
                scan_next_token(&p);
                ok = (mb_val >= 1 && mb_val <= 1048576) && consume_statement_end(&p);
                if (ok) {
                    g_buffer_pool_size_mb = (uint32_t)mb_val;
                    shell_printf("BUFFER_POOL_SIZE set to %u MB.\n", g_buffer_pool_size_mb);
                }
            } else ok = false;
        } else if (consume_keyword(&p, "AUTO_VACUUM") || consume_keyword(&p, "AUTOVACUUM")) {
            if (consume_keyword(&p, "ON")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_autovacuum_enabled = true;
                    shell_puts("Auto-vacuum is ON.");
                }
            } else if (consume_keyword(&p, "OFF")) {
                ok = consume_statement_end(&p);
                if (ok) {
                    g_autovacuum_enabled = false;
                    shell_puts("Auto-vacuum is OFF.");
                }
            } else ok = false;
        } else ok = false;
    } else if (!strcmp(command, "SUPPORT")) {
        ok = execute_support_report(&p);
    } else if (!strcmp(command, "STATUS")) {
        ok = consume_statement_end(&p);
        if (!g_db) {
            shell_puts("Status: Not connected to any database.");
            shell_puts("Use CONNECT '<path>' [BACKUP '<backup_dir>']; to open a database.");
        } else if (ok) {
            if (g_network_user[0]) {
                shell_printf("Session: network user %s\nTransport security: %s\n",
                             g_network_user, g_network_tls_active ? "encrypted TLS" : "unencrypted loopback TCP");
            } else {
                shell_printf("Session: %s\n",
                             g_server_mode ? "local server console" : "local shell or embedded API");
            }
            shell_printf("Database: %s\nGeneration: %llu\nPending: %s\nAutocommit: %s\nDestructive confirm: %s\nStorage health: %s\nBackup interval: %u seconds\nBackup directory: %s\nBackup health: %s\nLast backed-up generation: %llu\nLast completed backup (Unix seconds): %llu\n",
                    g_path, (unsigned long long)g_generation, g_pending ? "yes" : "no",
                    g_autocommit ? "ON" : "OFF",
                    g_confirm_destructive ? "ON" : "OFF",
                    g_io_uncertain ? "UNCERTAIN - restart required" : "healthy",
                    g_backup_interval, g_backup_dir, atomic_load(&g_backup_failed) ? "FAILED" : "healthy",
                    atomic_load(&g_backup_generation), atomic_load(&g_backup_time));
        }
    } else if (!strcmp(command, "BROWSE")) {
        char table_name[MAX_NAME_LEN];
        ok = parse_identifier(&p, table_name) && consume_statement_end(&p);
        if (ok) {
            if (!g_db) return report_database_error("connect to a database before browsing tables");
            int table_idx = find_table_index(g_db, table_name);
            if (table_idx < 0)
                return report_database_error("table does not exist: %s; use SHOW TABLES to list available tables",
                                             table_name);
            int total_rows = g_db->tables[table_idx].row_count;
            int previous_max_rows = g_max_rows;
            int page_size = g_max_rows > 0 && g_max_rows < 20 ? g_max_rows : 20;
            int offset = 0;
            for (;;) {
                g_max_rows = page_size;
                char query[MAX_NAME_LEN + 64];
                snprintf(query, sizeof(query), "SELECT * FROM %s LIMIT %d OFFSET %d;",
                         table_name, page_size, offset);
                bool nested_quit = false;
                ok = execute_console_statement(query, &nested_quit);
                g_max_rows = previous_max_rows;
                if (!ok) break;
                offset += page_size;
                if (offset >= total_rows) {
                    if (total_rows > page_size)
                        shell_printf("End of %s (%d rows total).\n", table_name, total_rows);
                    else
                        shell_printf("Preview limited to %d rows; use SELECT for filtering or more rows.\n",
                                     page_size);
                    break;
                }
                if (!g_interactive_shell) {
                    shell_printf("Showing first %d of %d rows; use SELECT ... LIMIT %d OFFSET %d for more.\n",
                                 page_size, total_rows, page_size, offset);
                    break;
                }
                shell_printf("-- %d of %d rows shown. Press Enter for the next page, or q to stop: ",
                             offset, total_rows);
                char answer[16];
                int r = shell_prompt_line("", answer, sizeof(answer));
                if (r < 0) { ok = false; break; }
                if (r == 0 || (answer[0] && (answer[0] == 'q' || answer[0] == 'Q'))) {
                    shell_puts("Paging stopped.");
                    break;
                }
            }
        }
    } else if (!strcmp(command, "CANCEL")) {
        shell_puts("Query buffer cleared.");
        return true;
    } else if (!strcmp(command, "SHOW") && token_is_keyword(&p, "USERS")) {
        ok = execute_user_catalog_statement(&p, command);
    } else if (!strcmp(command, "SHOW") && token_is_keyword(&p, "GRANTS")) {
        ok = consume_keyword(&p, "GRANTS") && consume_statement_end(&p) && execute_show_grants();
    } else if (!strcmp(command, "GRANT")) {
        ok = execute_grant_statement(&p, true);
    } else if (!strcmp(command, "REVOKE")) {
        ok = execute_grant_statement(&p, false);
    } else if (!strcmp(command, "SHOW") && (consume_keyword(&p, "SESSIONS") || consume_keyword(&p, "PROCESSLIST"))) {
        ok = consume_statement_end(&p);
        if (ok) print_active_sessions();
    } else if (!strcmp(command, "KILL")) {
        if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
            return report_database_error("permission denied: KILL SESSION requires the ADMIN role");
        }
        consume_keyword(&p, "SESSION");
        if (p.kind != TK_NUMBER) {
            ok = report_database_error("expected session ID number after KILL");
        } else {
            int sid = (int)strtol(p.text, NULL, 10);
            scan_next_token(&p);
            ok = consume_statement_end(&p) && kill_client_session(sid);
        }
    } else if (!strcmp(command, "RESTART")) {
        if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
            return report_database_error("permission denied: RESTART requires the ADMIN role");
        }
        ok = consume_statement_end(&p) && request_server_restart();
        if (ok) shell_puts("Hot-patch restart initiated; the successor process is taking over.");
    } else if (!strcmp(command, "SHOW") && consume_keyword(&p, "DATABASES")) {
        ok = consume_statement_end(&p);
        if (ok) show_databases_in_dir();
    } else if (!strcmp(command, "SHOW") && consume_keyword(&p, "CLUSTER")) {
        ok = consume_statement_end(&p);
        if (ok) print_cluster_status();
    } else if (!strcmp(command, "SHOW") && consume_keyword(&p, "METRICS")) {
        ok = consume_statement_end(&p);
        if (ok) {
            AuthRateLimitStats arl_stats = auth_rate_limit_stats();
            shell_puts("METRIC\tVALUE");
            shell_puts("opendata_up\t1");
            shell_printf("opendata_active_sessions\t%d\n", get_active_session_count());
            shell_printf("opendata_database_tables\t%d\n", g_committed ? g_committed->table_count : 0);
            shell_printf("opendata_database_generation\t%llu\n", (unsigned long long)g_generation);
            shell_printf("opendata_raft_role\t%s (%d)\n",
                         g_raft_role == RAFT_LEADER ? "LEADER" : (g_raft_role == RAFT_CANDIDATE ? "CANDIDATE" : "FOLLOWER"),
                         (int)g_raft_role);
            shell_printf("opendata_raft_term\t%u\n", raft_get_current_term());
            shell_printf("opendata_auth_admitted_total\t%llu\n", (unsigned long long)arl_stats.admitted);
            shell_printf("opendata_auth_failures_total\t%llu\n", (unsigned long long)arl_stats.credential_failures);
            shell_printf("opendata_auth_throttled_total\t%llu\n",
                         (unsigned long long)(arl_stats.source_throttled + arl_stats.pair_throttled + arl_stats.global_throttled));
        }
    } else if (!strcmp(command, "VERIFY") && (consume_keyword(&p, "AUDIT") || consume_keyword(&p, "AUDIT_LOG"))) {
        ok = consume_statement_end(&p) && execute_verify_audit_log();
    } else if (!strcmp(command, "DROP") && consume_keyword(&p, "DATABASE")) {
        ok = execute_drop_database_statement(&p);
    } else if (!g_db) {
        if (!strcmp(command, "CREATE") && consume_keyword(&p, "DATABASE")) {
            ok = execute_create_database_statement(&p);
        } else {
            return report_database_error("not connected to any database.\n"
                "Hint: type CREATE DATABASE 'mydata.pcod'; to create one, or restart with: opendata mydata.pcod");
        }
    } else if (!strcmp(command, "COMMIT")) {
        ok = consume_statement_end(&p) && commit_transaction();
    } else if (!strcmp(command, "ROLLBACK")) {
        if (consume_keyword(&p, "TO")) {
            consume_keyword(&p, "SAVEPOINT");
            char sp_name[MAX_NAME_LEN];
            ok = parse_identifier(&p, sp_name) && consume_statement_end(&p) && execute_rollback_savepoint(sp_name);
        } else {
            ok = consume_statement_end(&p) && rollback_transaction();
        }
    } else if (!strcmp(command, "SAVEPOINT")) {
        char sp_name[MAX_NAME_LEN];
        ok = parse_identifier(&p, sp_name) && consume_statement_end(&p) && execute_savepoint(sp_name);
    } else if (!strcmp(command, "RELEASE")) {
        consume_keyword(&p, "SAVEPOINT");
        char sp_name[MAX_NAME_LEN];
        ok = parse_identifier(&p, sp_name) && consume_statement_end(&p) && execute_release_savepoint(sp_name);
    } else if (!strcmp(command, "BEGIN")) {
        if (!consume_statement_end(&p) || g_pending) ok = false;
        else { g_pending = true; shell_puts("Transaction started."); }
    } else if (!strcmp(command, "BACKUP")) {
        if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
            ok = report_database_error("permission denied: BACKUP requires the ADMIN role");
        } else {
            ok = consume_statement_end(&p);
            if (ok) { ok = backup_committed_snapshot(); if (ok) shell_puts("Backup complete (committed data only)."); }
        }
    } else if (!strcmp(command, "CHECK")) {
        if (consume_keyword(&p, "INTEGRITY") && consume_statement_end(&p)) {
            ok = execute_check_integrity();
        } else ok = false;
    } else if (!strcmp(command, "VERIFY")) {
        if (!consume_keyword(&p, "BACKUP")) {
            ok = report_database_error("expected VERIFY BACKUP '<file>'");
        } else if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
            ok = report_database_error("permission denied: VERIFY BACKUP requires the ADMIN role");
        } else {
            char verify_path[PATH_CAP] = {0};
            if (!parse_path_arg(&p, verify_path, sizeof(verify_path)) || !consume_statement_end(&p)) {
                ok = report_database_error("expected backup file path after VERIFY BACKUP");
            } else {
                ok = verify_backup_file(verify_path);
            }
        }
    } else if (!strcmp(command, "PRAGMA")) {
        if (consume_keyword(&p, "INTEGRITY_CHECK") && consume_statement_end(&p)) {
            ok = execute_check_integrity();
        } else ok = false;
    } else if (!strcmp(command, "VACUUM") || !strcmp(command, "COMPACT")) {
        bool v_full = false;
        bool v_analyze = false;
        for (int vi = 0; vi < 2; vi++) {
            if (consume_keyword(&p, "FULL")) v_full = true;
            else if (consume_keyword(&p, "ANALYZE")) v_analyze = true;
        }
        char v_tbl[MAX_NAME_LEN] = {0};
        if (p.kind == TK_ID) parse_identifier(&p, v_tbl);
        if (consume_statement_end(&p)) {
            ok = execute_vacuum_command(v_tbl[0] ? v_tbl : NULL, v_full, v_analyze);
        } else ok = false;
    } else if (!strcmp(command, "EXPORT")) ok = execute_export_statement(&p);
    else if (!strcmp(command, "IMPORT")) ok = execute_import_statement(&p);
    else if (!strcmp(command, "DUMP")) {
        if (!consume_keyword(&p, "DATABASE")) {
            ok = report_database_error("expected DATABASE after DUMP");
        } else {
            char dump_path[PATH_CAP] = {0};
            if (consume_keyword(&p, "TO")) {
                if (p.kind != TK_STRING) {
                    return report_database_error("expected file path string after DUMP DATABASE TO");
                }
                snprintf(dump_path, sizeof(dump_path), "%s", p.text);
                scan_next_token(&p);
            }
            if (dump_path[0] && g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
                ok = report_database_error("permission denied: DUMP DATABASE TO requires the ADMIN role");
            } else if (!consume_statement_end(&p)) ok = false;
            else ok = dump_database_sql(g_db, dump_path[0] ? dump_path : NULL);
        }
    } else if (!strcmp(command, "DIFF")) {
        if (!consume_keyword(&p, "SCHEMA") || p.kind != TK_STRING) {
            ok = report_database_error("expected SCHEMA '<other_db>' after DIFF");
        } else if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
            ok = report_database_error("permission denied: DIFF SCHEMA requires the ADMIN role");
        } else {
            char other_path[PATH_CAP];
            snprintf(other_path, sizeof(other_path), "%s", p.text);
            scan_next_token(&p);
            if (!consume_statement_end(&p)) ok = false;
            else ok = diff_database_schema(g_db, other_path);
        }
    } else if (!strcmp(command, "BENCHMARK")) {
        if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN) {
            ok = report_database_error("permission denied: BENCHMARK requires the ADMIN role");
        } else if (p.kind != TK_NUMBER) {
            ok = report_database_error("expected iteration count after BENCHMARK");
        } else {
            int iters = (int)strtol(p.text, NULL, 10);
            if (iters < 1) iters = 1;
            if (iters > 100000) iters = 100000;
            const char* sub_sql = p.cursor;
            while (is_sql_whitespace(*sub_sql)) sub_sql++;
            if (!*sub_sql) {
                ok = report_database_error("expected SQL statement after BENCHMARK <iterations>");
            } else {
                double min_ms = 1e18, max_ms = 0.0, sum_ms = 0.0;
                QueryResultCapture bench_cap = {0};
                bench_cap.silent = true;
                QueryResultCapture* prev_cap = g_result_capture;
                g_result_capture = &bench_cap;
                for (int i = 0; i < iters && ok; i++) {
                    bool sub_quit = false;
                    double t0 = monotonic_seconds();
                    ok = execute_console_statement(sub_sql, &sub_quit);
                    double dt = (monotonic_seconds() - t0) * 1000.0;
                    if (dt < min_ms) min_ms = dt;
                    if (dt > max_ms) max_ms = dt;
                    sum_ms += dt;
                    free(bench_cap.rows);
                    bench_cap.rows = NULL;
                    bench_cap.row_count = 0;
                    bench_cap.capacity = 0;
                }
                g_result_capture = prev_cap;
                if (ok) {
                    double avg_ms = sum_ms / (double)iters;
                    double qps = (sum_ms > 0.0001) ? ((double)iters * 1000.0 / sum_ms) : ((double)iters * 1000000.0);
                    shell_printf("BENCHMARK (%d iterations): Total=%.3f ms | Avg=%.3f ms | Min=%.3f ms | Max=%.3f ms | Throughput=%.1f QPS\n",
                                 iters, sum_ms, avg_ms, min_ms, max_ms, qps);
                }
            }
        }
    } else if (!strcmp(command, "WATCH")) {
        if (g_network_user[0]) {
            return report_database_error("WATCH is disabled for network sessions");
        }
        int interval_s = 0;
        int watch_count = 1;
        for (;;) {
            if (consume_keyword(&p, "INTERVAL")) {
                if (p.kind != TK_NUMBER) return report_database_error("expected seconds after INTERVAL");
                interval_s = (int)strtol(p.text, NULL, 10);
                if (interval_s < 0) interval_s = 0;
                if (interval_s > 3600) interval_s = 3600;
                scan_next_token(&p);
            } else if (consume_keyword(&p, "COUNT")) {
                if (p.kind != TK_NUMBER) return report_database_error("expected iteration count after COUNT");
                watch_count = (int)strtol(p.text, NULL, 10);
                if (watch_count < 1) watch_count = 1;
                if (watch_count > 100) watch_count = 100;
                scan_next_token(&p);
            } else {
                break;
            }
        }
        char* watch_sql = malloc(MAX_QUERY_LEN);
        if (!watch_sql) return report_database_error("out of memory for WATCH statement");
        int length = snprintf(watch_sql, MAX_QUERY_LEN, "%s %s", p.text, p.cursor);
        if (length < 0 || length >= MAX_QUERY_LEN) {
            free(watch_sql);
            return report_database_error("WATCH statement input limit exceeded");
        }
        for (int i = 0; i < watch_count && ok; i++) {
            shell_printf("=== WATCH Iteration %d/%d ===\n", i + 1, watch_count);
            bool sub_quit = false;
            ok = execute_console_statement(watch_sql, &sub_quit);
            if (i + 1 < watch_count && interval_s > 0) {
#ifdef _WIN32
                Sleep((DWORD)(interval_s * 1000));
#else
                sleep((unsigned)interval_s);
#endif
            }
        }
        free(watch_sql);
    } else if (!strcmp(command, "SHOW")) {
        if (consume_keyword(&p, "ENGINE") && consume_keyword(&p, "STATUS") && consume_statement_end(&p)) {
            print_engine_status();
        } else if (consume_keyword(&p, "CREATE") && consume_keyword(&p, "TABLE")) {
            char tbl_name[MAX_NAME_LEN] = {0};
            if (!parse_identifier(&p, tbl_name) || !consume_statement_end(&p)) {
                ok = false;
            } else {
                int idx = find_table_index(g_db, tbl_name);
                if (idx < 0) return report_database_error("table does not exist: %s", tbl_name);
                print_table_ddl(&g_db->tables[idx]);
            }
        } else if (consume_keyword(&p, "TABLES") && consume_statement_end(&p)) {
            shell_puts("TABLE_NAME\tCOLUMNS\tROWS");
            shell_puts("DUAL\t1\t1");
            for (int t = 0; t < g_db->table_count; t++)
                shell_printf("%s\t%d\t%d\n", g_db->tables[t].name, g_db->tables[t].col_count, g_db->tables[t].row_count);
            if (g_db->table_count == 0)
                shell_puts("-- No tables yet. Create one: CREATE TABLE t (id NUMBER PRIMARY KEY, name VARCHAR2(50));");
        } else if (consume_keyword(&p, "USER") && consume_statement_end(&p)) {
            shell_puts("USER is \"SYSDBA\" (display label, not authentication).");
        } else if (consume_keyword(&p, "SCHEMA")) {
            char tbl_name[MAX_NAME_LEN] = {0};
            if (p.kind == TK_ID) parse_identifier(&p, tbl_name);
            if (!consume_statement_end(&p)) {
                ok = false;
            } else if (tbl_name[0]) {
                int idx = find_table_index(g_db, tbl_name);
                if (idx < 0) return report_database_error("table does not exist: %s", tbl_name);
                print_table_ddl(&g_db->tables[idx]);
            } else {
                if (g_db->table_count == 0) shell_puts("-- No user tables defined.");
                for (int t = 0; t < g_db->table_count; t++) {
                    print_table_ddl(&g_db->tables[t]);
                    if (t + 1 < g_db->table_count) shell_puts("");
                }
            }
        } else if (consume_keyword(&p, "INDEXES")) {
            char tbl_name[MAX_NAME_LEN] = {0};
            consume_keyword(&p, "FROM");
            if (p.kind == TK_ID) parse_identifier(&p, tbl_name);
            if (!consume_statement_end(&p)) {
                ok = false;
            } else {
                if (tbl_name[0] && find_table_index(g_db, tbl_name) < 0) {
                    return report_database_error("table does not exist: %s", tbl_name);
                }
                shell_puts("INDEX_NAME\tTABLE_NAME\tCOLUMN_NAME\tINDEX_TYPE\tUNIQUENESS");
                for (int t = 0; t < g_db->table_count; t++) {
                    const Table* tbl = &g_db->tables[t];
                    if (tbl_name[0] && strcmp(tbl->name, tbl_name) != 0) continue;
                    for (int c = 0; c < tbl->col_count; c++) {
                        const ColumnDef* col = &tbl->columns[c];
                        if (col->primary) {
                            shell_printf("PK_%s_%s\t%s\t%s\tHASH_PK\tUNIQUE\n", tbl->name, col->name, tbl->name, col->name);
                        } else if (col->unique) {
                            shell_printf("UQ_%s_%s\t%s\t%s\tHASH_UQ\tUNIQUE\n", tbl->name, col->name, tbl->name, col->name);
                        }
                        if (col->ref_table[0]) {
                            shell_printf("FK_%s_%s\t%s\t%s\tJOIN_HASH\tNONUNIQUE\n", tbl->name, col->name, tbl->name, col->name);
                        }
                    }
                }
                for (int i = 0; i < g_db->index_count; i++) {
                    const SecondaryIndex* s_idx = &g_db->indexes[i];
                    if (tbl_name[0] && strcmp(s_idx->table_name, tbl_name) != 0) continue;
                    int t = find_table_index(g_db, s_idx->table_name);
                    const char* col_name = (t >= 0 && s_idx->col_idx >= 0 && s_idx->col_idx < g_db->tables[t].col_count) ? g_db->tables[t].columns[s_idx->col_idx].name : "";
                    shell_printf("%s\t%s\t%s\tBTREE_ORDERED\t%s\n",
                                 s_idx->name, s_idx->table_name, col_name, s_idx->is_unique ? "UNIQUE" : "NONUNIQUE");
                }
            }
        } else if (consume_keyword(&p, "STATS")) {
            char tbl_name[MAX_NAME_LEN] = {0};
            if (consume_keyword(&p, "FOR")) {
                if (p.kind == TK_ID) parse_identifier(&p, tbl_name);
            }
            if (!consume_statement_end(&p)) {
                ok = false;
            } else {
                print_table_stats(tbl_name[0] ? tbl_name : NULL);
                ok = true;
            }
        } else if (consume_keyword(&p, "CONSTRAINTS")) {
            char tbl_name[MAX_NAME_LEN] = {0};
            consume_keyword(&p, "FROM");
            if (p.kind == TK_ID) parse_identifier(&p, tbl_name);
            if (!consume_statement_end(&p)) {
                ok = false;
            } else {
                if (tbl_name[0] && find_table_index(g_db, tbl_name) < 0) {
                    return report_database_error("table does not exist: %s", tbl_name);
                }
                shell_puts("CONSTRAINT_NAME\tTABLE_NAME\tCOLUMN_NAME\tTYPE\tREFERENCE");
                for (int t = 0; t < g_db->table_count; t++) {
                    const Table* tbl = &g_db->tables[t];
                    if (tbl_name[0] && strcmp(tbl->name, tbl_name) != 0) continue;
                    for (int c = 0; c < tbl->col_count; c++) {
                        const ColumnDef* col = &tbl->columns[c];
                        if (col->primary) {
                            shell_printf("PK_%s_%s\t%s\t%s\tPRIMARY KEY\t\n", tbl->name, col->name, tbl->name, col->name);
                        }
                        if (col->unique && !col->primary) {
                            shell_printf("UQ_%s_%s\t%s\t%s\tUNIQUE\t\n", tbl->name, col->name, tbl->name, col->name);
                        }
                        if (!col->nullable && !col->primary) {
                            shell_printf("NN_%s_%s\t%s\t%s\tNOT NULL\t\n", tbl->name, col->name, tbl->name, col->name);
                        }
                        if (col->ref_table[0]) {
                            char cascade_info[64] = {0};
                            if (col->fk_on_delete == FK_ACTION_CASCADE) snprintf(cascade_info, sizeof(cascade_info), " [ON DELETE CASCADE]");
                            else if (col->fk_on_delete == FK_ACTION_SET_NULL) snprintf(cascade_info, sizeof(cascade_info), " [ON DELETE SET NULL]");
                            if (col->fk_on_update == FK_ACTION_CASCADE) {
                                size_t len = strlen(cascade_info);
                                snprintf(cascade_info + len, sizeof(cascade_info) - len, " [ON UPDATE CASCADE]");
                            } else if (col->fk_on_update == FK_ACTION_SET_NULL) {
                                size_t len = strlen(cascade_info);
                                snprintf(cascade_info + len, sizeof(cascade_info) - len, " [ON UPDATE SET NULL]");
                            }
                            shell_printf("FK_%s_%s\t%s\t%s\tFOREIGN KEY\t%s(%s)%s\n",
                                    tbl->name, col->name, tbl->name, col->name, col->ref_table, col->ref_column, cascade_info);
                        }
                    }
                }
            }
        } else {
            char sub[MAX_NAME_LEN] = {0};
            if (p.kind == TK_ID) snprintf(sub, sizeof(sub), "%s", p.text);
            const char* suggestion = suggest_show_word(sub);
            if (suggestion)
                return report_database_error("unknown SHOW target '%s'. Did you mean SHOW %s?",
                                             sub, suggestion);
            return report_database_error("unknown SHOW target. Try SHOW TABLES, SHOW DATABASES, SHOW USERS, SHOW SESSIONS, or SHOW SCHEMA.");
        }
    } else if (!strcmp(command, "DESC") || !strcmp(command, "DESCRIBE")) {
        char name[MAX_NAME_LEN];
        ok = parse_identifier(&p, name) && consume_statement_end(&p);
        if (ok && !strcmp(name, "DUAL")) shell_puts("NAME\tTYPE\tNULLABLE\tKEY\tREFERENCES\nDUMMY\tVARCHAR2(1)\tNO\t\t");
        else if (ok) {
            int index = find_table_index(g_db, name);
            if (index < 0) return report_database_error("table does not exist: %s", name);
            const Table* table = &g_db->tables[index];
            shell_puts("NAME\tTYPE\tNULLABLE\tKEY\tREFERENCES");
            for (int c = 0; c < table->col_count; c++) {
                const ColumnDef* col = &table->columns[c];
                char type[32];
                if (col->type == TYPE_VARCHAR2) snprintf(type, sizeof(type), "VARCHAR2(%d)", col->size);
                else if (col->type == TYPE_VECTOR) snprintf(type, sizeof(type), "VECTOR(%d)", col->size);
                else snprintf(type, sizeof(type), "%s", col->type == TYPE_NUMBER ? "NUMBER" : "DATE");
                shell_printf("%s\t%s\t%s\t%s\t%s%s%s\n", col->name, type,
                        col->nullable ? "YES" : "NO", col->primary ? "PRIMARY" : col->unique ? "UNIQUE" : "",
                        col->ref_table, col->ref_table[0] ? "." : "", col->ref_column);
            }
        }
    } else if (!strcmp(command, "EXPLAIN")) {
        ok = explain_statement(&p);
    } else if (!strcmp(command, "DRYRUN") || !strcmp(command, "DRY_RUN") || !strcmp(command, "TEST")) {
        char subcmd[MAX_NAME_LEN] = {0};
        if (!parse_identifier(&p, subcmd)) {
            return report_database_error("expected statement after %s (e.g. %s DROP TABLE tablename;)", command, command);
        }
        char target[MAX_NAME_LEN] = {0};
        char action[MAX_NAME_LEN] = {0};
        char extra[256] = {0};
        if (!strcmp(subcmd, "DROP")) {
            if (consume_keyword(&p, "TABLE")) {
                consume_keyword(&p, "IF");
                consume_keyword(&p, "EXISTS");
                parse_identifier(&p, target);
                snprintf(action, sizeof(action), "DROP TABLE");
            } else if (consume_keyword(&p, "DATABASE")) {
                parse_path_arg(&p, target, sizeof(target));
                snprintf(action, sizeof(action), "DROP DATABASE");
            } else if (consume_keyword(&p, "USER")) {
                if (p.kind == TK_STRING || p.kind == TK_ID) {
                    snprintf(target, sizeof(target), "%s", p.text);
                    scan_next_token(&p);
                }
                snprintf(action, sizeof(action), "DROP USER");
            } else if (consume_keyword(&p, "INDEX")) {
                parse_identifier(&p, target);
                snprintf(action, sizeof(action), "DROP INDEX");
            }
        } else if (!strcmp(subcmd, "TRUNCATE")) {
            consume_keyword(&p, "TABLE");
            parse_identifier(&p, target);
            snprintf(action, sizeof(action), "TRUNCATE TABLE");
        } else if (!strcmp(subcmd, "DELETE")) {
            consume_keyword(&p, "FROM");
            parse_identifier(&p, target);
            snprintf(action, sizeof(action), "DELETE FROM");
            bool has_where = false;
            while (p.kind != TK_END && p.kind != TK_SEMI && !p.failed) {
                if (token_is_keyword(&p, "WHERE")) { has_where = true; break; }
                scan_next_token(&p);
            }
            if (!has_where) snprintf(extra, sizeof(extra), "UNCONDITIONAL (no WHERE clause)");
            else snprintf(extra, sizeof(extra), "Filtered with WHERE condition");
        } else if (!strcmp(subcmd, "UPDATE")) {
            parse_identifier(&p, target);
            snprintf(action, sizeof(action), "UPDATE");
            bool has_where = false;
            while (p.kind != TK_END && p.kind != TK_SEMI && !p.failed) {
                if (token_is_keyword(&p, "WHERE")) { has_where = true; break; }
                scan_next_token(&p);
            }
            if (!has_where) snprintf(extra, sizeof(extra), "UNCONDITIONAL (no WHERE clause)");
            else snprintf(extra, sizeof(extra), "Filtered with WHERE condition");
        } else if (!strcmp(subcmd, "ALTER")) {
            if (consume_keyword(&p, "TABLE")) {
                parse_identifier(&p, target);
                snprintf(action, sizeof(action), "ALTER TABLE");
                snprintf(extra, sizeof(extra), "Table schema modification");
            }
        } else {
            snprintf(action, sizeof(action), "%s", subcmd);
            parse_identifier(&p, target);
        }
        show_destructive_dry_run(action[0] ? action : subcmd, target, extra);
        ok = true;
    } else if (!strcmp(command, "SELECT")) ok = execute_select_query(&p);
    else if (!strcmp(command, "WITH")) ok = execute_with_query(&p);
    else if (!strcmp(command, "ANALYZE")) {
        char tbl_name[MAX_NAME_LEN] = {0};
        consume_keyword(&p, "TABLE");
        if (p.kind == TK_ID) parse_identifier(&p, tbl_name);
        if (!consume_statement_end(&p)) {
            ok = false;
        } else if (tbl_name[0]) {
            ok = execute_analyze_table(tbl_name);
        } else {
            ok = execute_analyze_all();
        }
    } else if (!strcmp(command, "CREATE")) {
        if (consume_keyword(&p, "DATABASE")) ok = execute_create_database_statement(&p);
        else if (consume_keyword(&p, "WIZARD")) {
            ok = consume_statement_end(&p) && run_create_table_wizard(g_db);
        }
        else if (token_is_keyword(&p, "USER")) ok = execute_user_catalog_statement(&p, command);
        else ok = execute_atomic_mutation(&p, command);
    } else if (!strcmp(command, "LOAD")) {
        ok = consume_keyword(&p, "SAMPLE") && consume_statement_end(&p) && load_sample_data();
    } else if ((!strcmp(command, "DROP") || !strcmp(command, "ALTER")) &&
               token_is_keyword(&p, "USER")) {
        if (!strcmp(command, "DROP")) {
            Parser probe = p;
            consume_keyword(&probe, "USER");
            char user_name[MAX_NAME_LEN] = {0};
            if (probe.kind == TK_STRING || probe.kind == TK_ID) {
                snprintf(user_name, sizeof(user_name), "%s", probe.text);
                scan_next_token(&probe);
            }
            ConfirmationResult confirmation = confirm_destructive_action(
                "DROP USER", user_name, "This permanently deletes the user account and revokes credentials.", "DROP");
            if (confirmation == CONFIRM_DRYRUN) {
                handle_confirmation_with_dryrun(&confirmation, "DROP USER", user_name, "Active sessions and roles for this user will be invalidated.");
            }
            if (!allow_or_report_confirmation(confirmation)) {
                if (confirmation == CONFIRM_CANCEL) return true;
                return false;
            }
        }
        ok = execute_user_catalog_statement(&p, command);
    } else if (!strcmp(command, "DROP") || !strcmp(command, "INSERT") ||
             !strcmp(command, "UPDATE") || !strcmp(command, "DELETE") || !strcmp(command, "ALTER") ||
             !strcmp(command, "TRUNCATE")) {
        Parser probe = p;
        const char* explanation = NULL;
        const char* expected = NULL;
        const char* action_desc = command;
        char table_name[MAX_NAME_LEN] = {0};
        char extra_detail[256] = {0};
        if (!strcmp(command, "DROP") && consume_keyword(&probe, "TABLE")) {
            consume_keyword(&probe, "IF");
            consume_keyword(&probe, "EXISTS");
            if (parse_identifier(&probe, table_name)) {
                explanation = "This permanently removes the table and all of its rows.";
                expected = "DROP";
                action_desc = "DROP TABLE";
            }
        } else if (!strcmp(command, "TRUNCATE")) {
            consume_keyword(&probe, "TABLE");
            if (parse_identifier(&probe, table_name)) {
                explanation = "This removes every row from the table while preserving schema.";
                expected = "TRUNCATE";
                action_desc = "TRUNCATE TABLE";
            }
        } else if (!strcmp(command, "DELETE") && consume_keyword(&probe, "FROM") &&
                   parse_identifier(&probe, table_name)) {
            if (probe.kind == TK_SEMI || probe.kind == TK_END) {
                explanation = "Unconditional DELETE: every row in this table will be deleted!";
                expected = "DELETE";
                action_desc = "DELETE FROM";
                snprintf(extra_detail, sizeof(extra_detail), "UNCONDITIONAL (no WHERE clause)");
            }
        } else if (!strcmp(command, "UPDATE") && parse_identifier(&probe, table_name)) {
            bool has_where = false;
            while (probe.kind != TK_END && probe.kind != TK_SEMI && !probe.failed) {
                if (token_is_keyword(&probe, "WHERE")) { has_where = true; break; }
                scan_next_token(&probe);
            }
            if (!has_where) {
                explanation = "Unconditional UPDATE: every row in this table will be modified!";
                expected = "UPDATE";
                action_desc = "UPDATE";
                snprintf(extra_detail, sizeof(extra_detail), "UNCONDITIONAL (no WHERE clause)");
            }
        }
        if (expected) {
            ConfirmationResult confirmation = confirm_destructive_action(action_desc, table_name, explanation, expected);
            if (confirmation == CONFIRM_DRYRUN) {
                handle_confirmation_with_dryrun(&confirmation, action_desc, table_name, extra_detail);
            }
            if (!allow_or_report_confirmation(confirmation)) {
                if (confirmation == CONFIRM_CANCEL) return true;
                return false;
            }
        }
        ok = execute_atomic_mutation(&p, command);
    }
    else ok = false;
    if (!ok && g_errors == before_errors) {
        if (!strcmp(command, "SELECT"))
            report_database_error("invalid or unsupported SELECT statement.\n"
                "Hint: try SELECT * FROM tablename; or check table names with SHOW TABLES;");
        else if (!strcmp(command, "CREATE"))
            report_database_error("invalid or unsupported CREATE statement.\n"
                "Hint: try CREATE TABLE name (id NUMBER PRIMARY KEY, col VARCHAR2(50)); or HELP CREATE;");
        else if (!strcmp(command, "INSERT"))
            report_database_error("invalid or unsupported INSERT statement.\n"
                "Hint: try INSERT INTO tablename VALUES (1, 'text'); or check the table with DESCRIBE tablename;");
        else {
            const char* suggestion = suggest_command_word(command);
            if (suggestion)
                report_database_error("invalid or unsupported %s statement. Did you mean %s? Type HELP; for examples.",
                                      command, suggestion);
            else
                report_database_error("invalid or unsupported %s statement. Type HELP; for examples.", command);
        }
    }
    if (g_timing) shell_printf("Elapsed: %.3f ms\n", (monotonic_seconds() - start) * 1000.0);
    return ok;
}

bool execute_console_statement(const char* text, bool* quit) {
    static unsigned depth = 0;
    if (depth >= 16) return report_database_error("command nesting limit exceeded");
    depth++;
    bool ok = execute_console_statement_impl(text, quit);
    depth--;
    /* Structured audit trail for data- and security-changing statements.
       Passwords and row values are never recorded — only the command class
       and the target object name. */
    if (g_audit_log_enabled && depth == 0) {
        Parser audit_p;
        memset(&audit_p, 0, sizeof(audit_p));
        audit_p.cursor = text;
        scan_next_token(&audit_p);
        if (audit_p.kind == TK_ID) {
            char audit_cmd[MAX_NAME_LEN];
            snprintf(audit_cmd, sizeof(audit_cmd), "%s", audit_p.text);
            bool auditable = !strcmp(audit_cmd, "INSERT") || !strcmp(audit_cmd, "UPDATE") ||
                !strcmp(audit_cmd, "DELETE") || !strcmp(audit_cmd, "TRUNCATE") ||
                !strcmp(audit_cmd, "CREATE") || !strcmp(audit_cmd, "DROP") ||
                !strcmp(audit_cmd, "ALTER") || !strcmp(audit_cmd, "GRANT") ||
                !strcmp(audit_cmd, "REVOKE") || !strcmp(audit_cmd, "IMPORT");
            if (auditable) {
                char target[MAX_NAME_LEN] = {0};
                /* Skip to the first plausible object name. */
                for (int hops = 0; hops < 4 && audit_p.kind == TK_ID; hops++) {
                    if (token_is_keyword(&audit_p, "TABLE") || token_is_keyword(&audit_p, "INTO") ||
                        token_is_keyword(&audit_p, "FROM") || token_is_keyword(&audit_p, "ON") ||
                        token_is_keyword(&audit_p, "USER") || token_is_keyword(&audit_p, "INDEX") ||
                        token_is_keyword(&audit_p, "DATABASE") || token_is_keyword(&audit_p, "TO") ||
                        token_is_keyword(&audit_p, "UPDATE")) {
                        scan_next_token(&audit_p);
                        if (audit_p.kind == TK_ID) {
                            snprintf(target, sizeof(target), "%s", audit_p.text);
                            break;
                        }
                        if (audit_p.kind == TK_STRING) break; /* never record string literals */
                        continue;
                    }
                    if (hops == 0 && strcmp(audit_cmd, audit_p.text))
                        snprintf(target, sizeof(target), "%s", audit_p.text);
                    break;
                }
                audit_log_statement(audit_cmd, target, ok);
            }
        }
    }
    return ok;
}
