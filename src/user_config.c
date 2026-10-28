#include "database_engine.h"

#ifdef _WIN32
#include <shlobj.h>
#else
#include <pwd.h>
#include <sys/types.h>
#endif

char g_config_path[PATH_CAP];
char g_error_report_path[PATH_CAP];
bool g_config_loaded;
char g_default_database[PATH_CAP];
bool g_show_tutorial = true;
bool g_autocommit_configured;
bool g_semicolon_optional_configured;
bool g_audit_log_enabled;
char g_audit_log_path[PATH_CAP];

static const char* DEFAULT_CONFIG =
    "# PC/OpenData SQL Server — User Configuration\n"
    "# This file was created automatically on first launch.\n"
    "# Edit values to change defaults. Delete this file to reset to defaults.\n"
    "\n"
    "[shell]\n"
    "# Automatically COMMIT after every INSERT/UPDATE/DELETE/CREATE/DROP/ALTER\n"
    "# Default: on for interactive terminal sessions; off for scripts/piped input\n"
    "# Uncomment to force a specific behavior in all modes:\n"
    "; autocommit = on\n"
    "\n"
    "# Run a complete single-line statement as soon as you press Enter, without\n"
    "# needing a semicolon. Multi-line statements still work: end intermediate\n"
    "# lines with a comma or open parenthesis, or finish with a semicolon.\n"
    "# Default: on for interactive terminal sessions; off for scripts/piped input\n"
    "# Uncomment to force a specific behavior in all modes:\n"
    "; semicolon_optional = on\n"
    "\n"
    "# Output format for query results: table, csv, or json\n"
    "# Default: table\n"
    "format = table\n"
    "\n"
    "# Maximum rows returned by SELECT (0 = unlimited)\n"
    "# Default: 0\n"
    "maxrows = 0\n"
    "\n"
    "# Show query execution time after each statement\n"
    "# Default: off\n"
    "timing = off\n"
    "\n"
    "# Display string for NULL values\n"
    "# Default: NULL\n"
    "null_string = NULL\n"
    "\n"
    "[startup]\n"
    "# Default database file to open when none is specified on the command line\n"
    "# The file is created automatically if it does not exist.\n"
    "# Default: mydata.pcod\n"
    "default_database = mydata.pcod\n"
    "\n"
    "# Show the quick-start tutorial banner when no database is connected\n"
    "# Default: on\n"
    "show_tutorial = on\n"
    "\n"
    "[audit]\n"
    "# Write a structured audit record for every statement that changes data or\n"
    "# the security catalog. Records include timestamp, actor, command, and\n"
    "# outcome — never passwords or row values.\n"
    "# Default: off\n"
    "enabled = off\n"
    "\n"
    "# Audit log file path. Default: .opendata_audit in your home directory\n"
    "; path = C:\\Logs\\opendata_audit.log\n";

bool get_home_directory(char* output, size_t capacity) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';
#ifdef _WIN32
    const char* profile = getenv("USERPROFILE");
    if (profile && *profile) {
        return snprintf(output, capacity, "%s", profile) < (int)capacity;
    }
    const char* drive = getenv("HOMEDRIVE");
    const char* path = getenv("HOMEPATH");
    if (drive && *drive && path && *path) {
        return snprintf(output, capacity, "%s%s", drive, path) < (int)capacity;
    }
    /* Fallback: current directory */
    return GetCurrentDirectoryA((DWORD)capacity, output) > 0;
#else
    const char* home = getenv("HOME");
    if (home && *home) {
        return snprintf(output, capacity, "%s", home) < (int)capacity;
    }
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir) {
        return snprintf(output, capacity, "%s", pw->pw_dir) < (int)capacity;
    }
    /* Fallback: current directory */
    return getcwd(output, capacity) != NULL;
#endif
}

static void build_config_paths(void) {
    char home[PATH_CAP];
    if (!get_home_directory(home, sizeof(home))) {
        g_config_path[0] = '\0';
        g_error_report_path[0] = '\0';
        return;
    }
#ifdef _WIN32
    snprintf(g_config_path, sizeof(g_config_path), "%s\\opendata.ini", home);
    snprintf(g_error_report_path, sizeof(g_error_report_path), "%s\\.opendata_error_report", home);
#else
    snprintf(g_config_path, sizeof(g_config_path), "%s/opendata.ini", home);
    snprintf(g_error_report_path, sizeof(g_error_report_path), "%s/.opendata_error_report", home);
#endif
}

bool ensure_user_config_exists(void) {
    build_config_paths();
    if (!g_config_path[0]) return false;
    FILE* probe = fopen(g_config_path, "r");
    if (probe) {
        fclose(probe);
        return true;
    }
    FILE* out = fopen(g_config_path, "w");
    if (!out) return false;
    fputs(DEFAULT_CONFIG, out);
    fclose(out);
    return true;
}

static void trim_whitespace(char* s) {
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static bool parse_bool(const char* value, bool* out) {
    if (!strcmp(value, "on") || !strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "1")) {
        *out = true;
        return true;
    }
    if (!strcmp(value, "off") || !strcmp(value, "false") || !strcmp(value, "no") || !strcmp(value, "0")) {
        *out = false;
        return true;
    }
    return false;
}

bool load_user_config(void) {
    if (g_config_loaded) return true;
    if (!g_config_path[0]) build_config_paths();
    if (!g_config_path[0]) return false;
    FILE* in = fopen(g_config_path, "r");
    if (!in) return false;
    char line[1024];
    char section[64] = {0};
    while (fgets(line, sizeof(line), in)) {
        trim_whitespace(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", line + 1);
            }
            continue;
        }
        char* equals = strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        char* key = line;
        char* value = equals + 1;
        trim_whitespace(key);
        trim_whitespace(value);
        if (!strcmp(section, "shell")) {
            if (!strcmp(key, "autocommit")) {
                bool v;
                if (parse_bool(value, &v)) { g_autocommit = v; g_autocommit_configured = true; }
            } else if (!strcmp(key, "format")) {
                if (!strcmp(value, "csv")) g_output_format = FORMAT_CSV;
                else if (!strcmp(value, "json")) g_output_format = FORMAT_JSON;
                else g_output_format = FORMAT_TABLE;
            } else if (!strcmp(key, "maxrows")) {
                long v = strtol(value, NULL, 10);
                if (v >= 0 && v <= 1000000) g_max_rows = (int)v;
            } else if (!strcmp(key, "timing")) {
                bool v;
                if (parse_bool(value, &v)) g_timing = v;
            } else if (!strcmp(key, "null_string")) {
                if (strlen(value) < sizeof(g_null_str))
                    snprintf(g_null_str, sizeof(g_null_str), "%s", value);
            } else if (!strcmp(key, "semicolon_optional")) {
                bool v;
                if (parse_bool(value, &v)) { g_semicolon_optional = v; g_semicolon_optional_configured = true; }
            }
        } else if (!strcmp(section, "startup")) {
            if (!strcmp(key, "default_database")) {
                if (strlen(value) < sizeof(g_default_database))
                    snprintf(g_default_database, sizeof(g_default_database), "%s", value);
            } else if (!strcmp(key, "show_tutorial")) {
                bool v;
                if (parse_bool(value, &v)) g_show_tutorial = v;
            }
        } else if (!strcmp(section, "audit")) {
            if (!strcmp(key, "enabled")) {
                bool v;
                if (parse_bool(value, &v)) g_audit_log_enabled = v;
            } else if (!strcmp(key, "path")) {
                if (strlen(value) < sizeof(g_audit_log_path))
                    snprintf(g_audit_log_path, sizeof(g_audit_log_path), "%s", value);
            }
        }
    }
    fclose(in);
    g_config_loaded = true;
    return true;
}

/* Validate the config file strictly: unknown sections/keys and malformed
   values are reported. Used by --check-config. */
bool validate_user_config(void) {
    if (!g_config_path[0]) build_config_paths();
    if (!g_config_path[0]) {
        report_database_error("cannot determine the configuration file path");
        return false;
    }
    FILE* in = fopen(g_config_path, "r");
    if (!in) {
        printf("Config file not found: %s (it will be created with defaults on first launch).\n",
               g_config_path);
        return true;
    }
    static const char* const sections[] = {"shell", "startup", "audit"};
    static const char* const shell_keys[] = {
        "autocommit", "format", "maxrows", "timing", "null_string", "semicolon_optional"
    };
    static const char* const startup_keys[] = {"default_database", "show_tutorial"};
    static const char* const audit_keys[] = {"enabled", "path"};
    int problems = 0;
    int line_no = 0;
    char line[1024];
    char section[64] = {0};
    while (fgets(line, sizeof(line), in)) {
        line_no++;
        trim_whitespace(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (!end || end[1] != '\0') {
                printf("line %d: malformed section header\n", line_no);
                problems++;
                continue;
            }
            *end = '\0';
            snprintf(section, sizeof(section), "%s", line + 1);
            bool known = false;
            for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
                if (!strcmp(section, sections[i])) known = true;
            if (!known) {
                printf("line %d: unknown section [%s]\n", line_no, section);
                problems++;
            }
            continue;
        }
        char* equals = strchr(line, '=');
        if (!equals) {
            printf("line %d: expected key = value\n", line_no);
            problems++;
            continue;
        }
        *equals = '\0';
        char* key = line;
        char* value = equals + 1;
        trim_whitespace(key);
        trim_whitespace(value);
        const char* const* keys = NULL;
        size_t key_count = 0;
        if (!strcmp(section, "shell")) { keys = shell_keys; key_count = sizeof(shell_keys) / sizeof(shell_keys[0]); }
        else if (!strcmp(section, "startup")) { keys = startup_keys; key_count = sizeof(startup_keys) / sizeof(startup_keys[0]); }
        else if (!strcmp(section, "audit")) { keys = audit_keys; key_count = sizeof(audit_keys) / sizeof(audit_keys[0]); }
        bool known = false;
        for (size_t i = 0; keys && i < key_count; i++)
            if (!strcmp(key, keys[i])) known = true;
        if (!known) {
            printf("line %d: unknown key '%s' in section [%s]\n", line_no, key,
                   section[0] ? section : "(none)");
            problems++;
            continue;
        }
        bool value_ok = true;
        if (!strcmp(key, "autocommit") || !strcmp(key, "timing") ||
            !strcmp(key, "semicolon_optional") || !strcmp(key, "show_tutorial") ||
            !strcmp(key, "enabled")) {
            bool v;
            value_ok = parse_bool(value, &v);
        } else if (!strcmp(key, "format")) {
            value_ok = !strcmp(value, "table") || !strcmp(value, "csv") || !strcmp(value, "json");
        } else if (!strcmp(key, "maxrows")) {
            char* end = NULL;
            long v = strtol(value, &end, 10);
            value_ok = end && *end == '\0' && v >= 0 && v <= 1000000;
        } else if (!strcmp(key, "null_string")) {
            value_ok = strlen(value) < 32;
        } else if (!strcmp(key, "default_database") || !strcmp(key, "path")) {
            value_ok = value[0] != '\0' && strlen(value) < PATH_CAP;
        }
        if (!value_ok) {
            printf("line %d: invalid value for '%s': '%s'\n", line_no, key, value);
            problems++;
        }
    }
    fclose(in);
    if (problems) {
        printf("%s: %d problem%s found. Fix or delete the file to reset to defaults.\n",
               g_config_path, problems, problems == 1 ? "" : "s");
        return false;
    }
    printf("%s: OK\n", g_config_path);
    return true;
}

void audit_log_statement(const char* command, const char* detail, bool success) {
    if (!g_audit_log_enabled) return;
    char path[PATH_CAP];
    if (g_audit_log_path[0]) {
        snprintf(path, sizeof(path), "%s", g_audit_log_path);
    } else {
        if (!g_error_report_path[0]) build_config_paths();
        if (!g_error_report_path[0]) return;
        /* Derive the default from the error-report path's directory. */
        snprintf(path, sizeof(path), "%s", g_error_report_path);
        char* sep = strrchr(path, PATH_SEPARATOR[0]);
        if (!sep) return;
        snprintf(sep + 1, sizeof(path) - (size_t)(sep + 1 - path), ".opendata_audit");
    }
    FILE* out = fopen(path, "a");
    if (!out) return;
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
    const char* actor = g_network_user[0] ? g_network_user :
                        (g_interactive_shell ? "local-shell" : "local-process");
    fprintf(out, "[%s] actor=%s command=%s target=%s result=%s\n",
            timestamp, actor, command,
            detail && detail[0] ? detail : "-", success ? "OK" : "FAILED");
    fclose(out);
}

void log_error_report(const char* format, ...) {
    if (!g_error_report_path[0]) build_config_paths();
    if (!g_error_report_path[0]) return;
    FILE* out = fopen(g_error_report_path, "a");
    if (!out) return;
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(out, "[%s] ERROR: ", timestamp);
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    fclose(out);
}

void log_error_report_event(const char* event) {
    if (!g_error_report_path[0]) build_config_paths();
    if (!g_error_report_path[0]) return;
    FILE* out = fopen(g_error_report_path, "a");
    if (!out) return;
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(out, "[%s] %s\n", timestamp, event);
    fclose(out);
}
