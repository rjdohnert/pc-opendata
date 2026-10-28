#include "database_engine.h"

#ifdef _WIN32
#include <shlobj.h>
#include <bcrypt.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <openssl/sha.h>
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

static void format_digest_alpha(const unsigned char digest[32], char out[65]) {
    for (int i = 0; i < 32; i++) {
        out[i * 2] = (char)('a' + ((digest[i] >> 4) & 0x0F));
        out[i * 2 + 1] = (char)('a' + (digest[i] & 0x0F));
    }
    out[64] = '\0';
}

static bool compute_audit_sha256(const void* data, size_t len, char hex_out[65]) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return false;
    bool ok = false;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0) {
        if (BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0) >= 0) {
            unsigned char digest[32];
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
                format_digest_alpha(digest, hex_out);
                ok = true;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    unsigned char digest[32];
    SHA256((const unsigned char*)data, len, digest);
    format_digest_alpha(digest, hex_out);
    return true;
#endif
}

static bool get_audit_path(char* path, size_t cap) {
    if (g_audit_log_path[0]) {
        snprintf(path, cap, "%s", g_audit_log_path);
        return true;
    }
    if (!g_error_report_path[0]) build_config_paths();
    if (!g_error_report_path[0]) return false;
    snprintf(path, cap, "%s", g_error_report_path);
    char* sep = strrchr(path, PATH_SEPARATOR[0]);
    if (!sep) return false;
    snprintf(sep + 1, cap - (size_t)(sep + 1 - path), ".opendata_audit");
    return true;
}

void audit_log_statement(const char* command, const char* detail, bool success) {
    if (!g_audit_log_enabled) return;
    char path[PATH_CAP];
    if (!get_audit_path(path, sizeof(path))) return;

    char prev_hash[65] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    /* Read the last line to obtain the chained previous hash */
    FILE* in = fopen(path, "r");
    if (in) {
        char line[1024];
        while (fgets(line, sizeof(line), in)) {
            const char* hp = strstr(line, " hash=");
            if (hp && strlen(hp + 6) >= 64) {
                memcpy(prev_hash, hp + 6, 64);
                prev_hash[64] = '\0';
            }
        }
        fclose(in);
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

    char record[1024];
    snprintf(record, sizeof(record), "[%s] actor=%s command=%s target=%s result=%s prev_hash=%s",
             timestamp, actor, command,
             detail && detail[0] ? detail : "-", success ? "OK" : "FAILED", prev_hash);
    char current_hash[65] = {0};
    compute_audit_sha256(record, strlen(record), current_hash);

    fprintf(out, "%s hash=%s\n", record, current_hash);
    fclose(out);
}

bool verify_audit_log(const char* path, char* error_out, size_t error_cap) {
    if (!path || !path[0]) {
        snprintf(error_out, error_cap, "invalid audit log path");
        return false;
    }
    FILE* file = fopen(path, "r");
    if (!file) {
        snprintf(error_out, error_cap, "cannot open audit log file: %s", path);
        return false;
    }
    char expected_prev[65] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char line[1024];
    int line_num = 0;
    while (fgets(line, sizeof(line), file)) {
        line_num++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        if (len == 0) continue;

        const char* prev_ptr = strstr(line, " prev_hash=");
        const char* hash_ptr = strstr(line, " hash=");
        if (!prev_ptr || !hash_ptr || hash_ptr <= prev_ptr) {
            fclose(file);
            snprintf(error_out, error_cap, "Line %d: missing cryptographic hash chaining metadata", line_num);
            return false;
        }

        char rec_prev[65] = {0};
        char rec_hash[65] = {0};
        if (strlen(prev_ptr + 11) < 64 || strlen(hash_ptr + 6) < 64) {
            fclose(file);
            snprintf(error_out, error_cap, "Line %d: truncated hash metadata", line_num);
            return false;
        }
        memcpy(rec_prev, prev_ptr + 11, 64);
        memcpy(rec_hash, hash_ptr + 6, 64);

        if (strcmp(rec_prev, expected_prev) != 0) {
            fclose(file);
            snprintf(error_out, error_cap, "Line %d: hash chain broken (expected prev_hash %s, found %s)",
                     line_num, expected_prev, rec_prev);
            return false;
        }

        size_t prefix_len = (size_t)(hash_ptr - line);
        char computed_hash[65] = {0};
        compute_audit_sha256(line, prefix_len, computed_hash);
        if (strcmp(computed_hash, rec_hash) != 0) {
            fclose(file);
            snprintf(error_out, error_cap, "Line %d: entry content tampered (computed %s, recorded %s)",
                     line_num, computed_hash, rec_hash);
            return false;
        }

        memcpy(expected_prev, rec_hash, 65);
    }
    fclose(file);
    if (line_num == 0) snprintf(error_out, error_cap, "audit log is empty");
    else snprintf(error_out, error_cap, "verified %d audit entries; cryptographic hash chain intact", line_num);
    return true;
}

bool execute_verify_audit_log(void) {
    char path[PATH_CAP];
    if (!get_audit_path(path, sizeof(path))) {
        return report_database_error("cannot determine audit log path");
    }
    char msg[256];
    bool ok = verify_audit_log(path, msg, sizeof(msg));
    if (ok) {
        printf("AUDIT VERIFICATION: OK (%s)\n", msg);
    } else {
        report_database_error("AUDIT VERIFICATION FAILED: %s", msg);
    }
    return ok;
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
