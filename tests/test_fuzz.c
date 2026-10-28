#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include "opendata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Simple pseudorandom number generator for reproducible fuzz runs */
static uint32_t g_fuzz_seed = 133742;
static uint32_t fuzz_rand(void) {
    g_fuzz_seed = g_fuzz_seed * 1664525u + 1013904223u;
    return g_fuzz_seed;
}

static void test_fuzz_random_sql(int iterations) {
    printf("  [Fuzz] 1. Random byte mutation & grammar stress (%d iterations)...\n", iterations);
    static const char* const keywords[] = {
        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
        "DELETE", "CREATE", "TABLE", "DROP", "ALTER", "ADD", "RENAME", "GROUP",
        "BY", "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "JOIN", "ON", "INNER",
        "LEFT", "BEGIN", "COMMIT", "ROLLBACK", "BACKUP", "STATUS", "EXPLAIN",
        "ROW_NUMBER()", "RANK()", "OVER()", "EXISTS", "NOT", "AND", "OR",
        "PRIMARY", "KEY", "REFERENCES", "NUMBER", "VARCHAR2", "DATE"
    };
    int kw_count = (int)(sizeof(keywords) / sizeof(keywords[0]));

    for (int i = 0; i < iterations; i++) {
        char buf[512];
        size_t pos = 0;
        int parts = (int)(fuzz_rand() % 8) + 1;
        for (int p = 0; p < parts && pos + 64 < sizeof(buf); p++) {
            uint32_t choice = fuzz_rand() % 5;
            if (choice == 0) {
                /* Keyword */
                const char* kw = keywords[fuzz_rand() % (uint32_t)kw_count];
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s ", kw);
            } else if (choice == 1) {
                /* Punctuation / brackets */
                static const char punct[] = "(),;.*<>=!+-/%'\"";
                char ch = punct[fuzz_rand() % (sizeof(punct) - 1)];
                buf[pos++] = ch;
                buf[pos++] = ' ';
            } else if (choice == 2) {
                /* Random word or number */
                if (fuzz_rand() % 2 == 0) {
                    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%u ", fuzz_rand() % 100000);
                } else {
                    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "col_%u ", fuzz_rand() % 50);
                }
            } else if (choice == 3) {
                /* Garbage byte sequence */
                int glen = (int)(fuzz_rand() % 16) + 1;
                for (int g = 0; g < glen && pos + 1 < sizeof(buf); g++) {
                    char gc = (char)(1 + (fuzz_rand() % 126));
                    buf[pos++] = gc;
                }
                buf[pos++] = ' ';
            } else {
                /* Unclosed quote */
                buf[pos++] = '\'';
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "unclosed_%u ", fuzz_rand() % 100);
            }
        }
        buf[pos] = '\0';
        if (getenv("OPENDATA_FUZZ_TRACE")) {
            fprintf(stderr, "FUZZ %d:", i);
            for (size_t j = 0; j < pos; j++) fprintf(stderr, "%02x", (unsigned char)buf[j]);
            fputc('\n', stderr);
            fflush(stderr);
        }
        /* Must not crash or memory-corrupt */
        (void)opendata_exec(buf);
    }
}

static void test_fuzz_deep_nesting(void) {
    printf("  [Fuzz] 2. Deep expression & subquery nesting...\n");
    char nested[4096];
    size_t pos = 0;

    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, "SELECT ");
    for (int i = 0; i < 200 && pos + 4 < sizeof(nested); i++) {
        nested[pos++] = '(';
    }
    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, "1");
    for (int i = 0; i < 200 && pos + 4 < sizeof(nested); i++) {
        nested[pos++] = ')';
    }
    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, " FROM DUAL;");
    (void)opendata_exec(nested);

    /* Deeply nested WHERE EXISTS */
    pos = 0;
    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, "SELECT * FROM DUAL WHERE ");
    for (int i = 0; i < 20 && pos + 40 < sizeof(nested); i++) {
        pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, "EXISTS (SELECT * FROM DUAL WHERE ");
    }
    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, "1=1");
    for (int i = 0; i < 20 && pos + 2 < sizeof(nested); i++) {
        nested[pos++] = ')';
    }
    nested[pos] = '\0';
    (void)opendata_exec(nested);
}

static void test_fuzz_numeric_overflows(void) {
    printf("  [Fuzz] 3. Extreme numeric overflows & malformed literals...\n");
    static const char* const overflows[] = {
        "SELECT 999999999999999999999999999999999999999999999999999999999 FROM DUAL;",
        "SELECT -99999999999999999999999999999999999999999999999999999999 FROM DUAL;",
        "SELECT 1.2.3.4.5 FROM DUAL;",
        "SELECT 0000000000000000000000000000000000000000000000001 FROM DUAL;",
        "SELECT . FROM DUAL;",
        "SELECT ..123 FROM DUAL;",
        "SELECT 1e999999999999999999999 FROM DUAL;",
        "SELECT 1/0 FROM DUAL;",
        "SELECT '2026-99-99' FROM DUAL;",
        "SELECT '1900-02-29' FROM DUAL;", /* Non leap-year check */
        "SELECT '9999-99-99' FROM DUAL;"
    };
    for (size_t i = 0; i < sizeof(overflows) / sizeof(overflows[0]); i++) {
        (void)opendata_exec(overflows[i]);
    }
}

static void test_fuzz_json_csv_inputs(void) {
    printf("  [Fuzz] 4. Malformed JSON and CSV fuzzing...\n");
    char out[128];
    static const char* const malformed_json[] = {
        "", "{", "}", "[", "]", "{\"a\":}", "{\"a\":", "{\"a\":[1,2,}",
        "{\"a\":\"unclosed", "{\"a\": \xFF\xFE\xFD}", "{\"a\": { \"b\": { \"c\": { \"d\": 1 } } }",
        "{\"k\": 1e9999999999}", "{\"k\": -}", "{\"k\": null}", "{\"k\": true}"
    };
    for (size_t i = 0; i < sizeof(malformed_json) / sizeof(malformed_json[0]); i++) {
        (void)opendata_extract_json(malformed_json[i], "a", out, sizeof(out));
        (void)opendata_extract_json(malformed_json[i], "k", out, sizeof(out));
    }

    /* Test CSV parser with malformed files */
    FILE* f = fopen("test_db_dir/fuzz_broken.csv", "w");
    if (f) {
        fprintf(f, "id,name\n");
        fprintf(f, "1,\"unclosed string\n2,normal\n");
        fprintf(f, "3,too,many,columns,here\n");
        fprintf(f, "4\n"); /* too few columns */
        fprintf(f, ",,\n");
        fclose(f);
        (void)opendata_exec("IMPORT TABLE fuzz_target FROM 'test_db_dir/fuzz_broken.csv' CSV;");
        unlink("test_db_dir/fuzz_broken.csv");
    }
}

static void test_fuzz_state_machine_chaos(int iterations) {
    printf("  [Fuzz] 5. Interleaved state machine chaos (%d iterations)...\n", iterations);
    static const char* const ops[] = {
        "BEGIN;",
        "COMMIT;",
        "ROLLBACK;",
        "CREATE TABLE chaos_t (id NUMBER PRIMARY KEY, v VARCHAR2(20));",
        "INSERT INTO chaos_t VALUES (1, 'val');",
        "INSERT INTO chaos_t VALUES (1, 'dup');", /* PK conflict */
        "UPDATE chaos_t SET v='updated' WHERE id=1;",
        "DELETE FROM chaos_t WHERE id=1;",
        "DROP TABLE chaos_t;",
        "SELECT * FROM chaos_t;",
        "STATUS;",
        "BACKUP;"
    };
    int op_count = (int)(sizeof(ops) / sizeof(ops[0]));

    for (int i = 0; i < iterations; i++) {
        int idx = (int)(fuzz_rand() % (uint32_t)op_count);
        (void)opendata_exec(ops[idx]);
    }
    /* Clean rollback at end to restore clean state */
    (void)opendata_exec("ROLLBACK;");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Starting Automated Fuzz & SQL Grammar Stress Test ===\n");

#ifdef _WIN32
    CreateDirectoryA("test_db_dir", NULL);
    CreateDirectoryA("test_db_dir\\backups", NULL);
    const char* db = "test_db_dir\\fuzz_test.pcod";
    const char* backups = "test_db_dir\\backups";
#else
    mkdir("test_db_dir", 0755);
    mkdir("test_db_dir/backups", 0755);
    const char* db = "test_db_dir/fuzz_test.pcod";
    const char* backups = "test_db_dir/backups";
#endif

    unlink(db);
    bool ok = opendata_open(db, backups, 60, NULL);
    assert(ok && "opendata_open must succeed for fuzz harness");

    test_fuzz_random_sql(1000);
    test_fuzz_deep_nesting();
    test_fuzz_numeric_overflows();
    test_fuzz_json_csv_inputs();
    test_fuzz_state_machine_chaos(500);

    opendata_close();
    printf("=== Automated Fuzz & Stress Test PASSED with 0 Crashes ===\n");
    return 0;
}
