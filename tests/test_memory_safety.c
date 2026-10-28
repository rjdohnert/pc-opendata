#include "database_engine.h"
#include <assert.h>

static void query(const char* sql, QueryResultCapture* capture) {
    bool quit = false;
    g_result_capture = capture;
    assert(execute_console_statement(sql, &quit));
    g_result_capture = NULL;
}

int main(void) {
    assert(opendata_open("memory-safety.pcod", NULL, 0, NULL));
    assert(opendata_exec("CREATE TABLE numbers (n NUMBER, v VARCHAR2(255));"));
    for (int i = 0; i < 10; i++) {
        char sql[100];
        snprintf(sql, sizeof(sql), "INSERT INTO numbers VALUES (%d, 'aaaaab');", i);
        assert(opendata_exec(sql));
    }
    QueryResultCapture cap = {0};
    cap.silent = true;
    query("SELECT n FROM numbers ORDER BY n LIMIT 2 OFFSET 3;", &cap);
    assert(cap.row_count == 2 && cap.capacity >= 2);
    assert(!strcmp(cap.rows[0][0].text, "3") && !strcmp(cap.rows[1][0].text, "4"));
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;
    query("SELECT n FROM numbers ORDER BY n LIMIT 4 OFFSET 9;", &cap);
    assert(cap.row_count == 1 && !strcmp(cap.rows[0][0].text, "9"));
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;
    query("SELECT a.n FROM numbers a WHERE EXISTS (SELECT b.n FROM numbers b WHERE a.n=b.n);", &cap);
    assert(cap.row_count == 10);
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;
    query("SELECT n FROM numbers WHERE v LIKE '%a%a%a%a%a%b';", &cap);
    assert(cap.row_count == 10);
    query("SELECT n FROM numbers WHERE v LIKE '%a%a%a%a%a%c';", &cap);
    assert(cap.row_count == 0);
    free(cap.rows);
    char deep[10000];
    size_t off = (size_t)snprintf(deep, sizeof(deep), "SELECT n FROM numbers WHERE ");
    for (int i = 0; i < 4000; i++) deep[off++] = '(';
    memcpy(deep + off, "n=1", 3); off += 3;
    for (int i = 0; i < 4000; i++) deep[off++] = ')';
    deep[off++] = ';'; deep[off] = 0;
    assert(!opendata_exec(deep));
    assert(!opendata_exec("@U\x08#p"));
    assert(!opendata_exec("SOURCE U\x08#p"));
    assert(!opendata_exec("EXPLAIN SELECT n FROM numbers WHERE n=\x08"));
    FILE* recursive = fopen("recursive.sql", "w");
    assert(recursive);
    assert(fputs("SOURCE 'recursive.sql';\n", recursive) >= 0);
    assert(fclose(recursive) == 0);
    assert(!opendata_exec("SOURCE 'recursive.sql';"));
    assert(remove("recursive.sql") == 0);
    opendata_close();
    puts("Memory safety regressions passed.");
    return 0;
}
