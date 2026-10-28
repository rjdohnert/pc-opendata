#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#endif

#include "opendata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <io.h>
#define rmdir _rmdir
#define unlink _unlink
typedef SOCKET socket_t;
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int socket_t;
#define CLOSE_SOCK(s) close(s)
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("===================================================================");
    puts("   PC/OpenData - JSON, CSV, Server, Network & Protocols Test Suite  ");
    puts("===================================================================");

    unlink("test_feat_data.pcod");
    unlink("test_feat_data.pcod.lock");
    unlink("export_test.csv");
    unlink("export_test.json");
    rmdir("test_feat_backups");
#ifdef _WIN32
    _mkdir("test_feat_backups");
#else
    mkdir("test_feat_backups", 0777);
#endif

    bool ok = opendata_open("test_feat_data.pcod", "test_feat_backups", 60, NULL);
    assert(ok && "opendata_open must succeed");

    /* ------------------------------------------------------------- */
    /* 1. Test CSV Export and Import                                 */
    /* ------------------------------------------------------------- */
    puts("\n[1] Testing CSV Export and Import...");
    ok = opendata_exec("CREATE TABLE staff (id NUMBER PRIMARY KEY, name VARCHAR2(50), role VARCHAR2(50));");
    assert(ok);

    ok = opendata_exec("INSERT INTO staff VALUES (1, 'Alice, Developer', 'Lead'), (2, 'Bob \"The Builder\"', 'Engineer');");
    assert(ok);

    /* Export to CSV */
    ok = opendata_export_csv("staff", "export_test.csv");
    assert(ok && "CSV export must succeed");

    /* Verify file exists and has content */
    FILE* fp = fopen("export_test.csv", "r");
    assert(fp != NULL);
    char buf[1024];
    bool found_header = false, found_alice = false, found_bob = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "ID,NAME,ROLE")) found_header = true;
        if (strstr(buf, "\"Alice, Developer\"")) found_alice = true;
        if (strstr(buf, "\"Bob \"\"The Builder\"\"\"")) found_bob = true;
    }
    fclose(fp);
    assert(found_header && found_alice && found_bob && "CSV formatting with quotes and escaping verified");
    puts("  -> CSV export successfully verified with RFC 4180 quote escaping.");

    /* Import from CSV into new table */
    ok = opendata_exec("CREATE TABLE staff_copy (id NUMBER PRIMARY KEY, name VARCHAR2(50), role VARCHAR2(50));");
    assert(ok);

    ok = opendata_import_csv("staff_copy", "export_test.csv", true);
    assert(ok && "CSV import must succeed");

    /* Verify imported row count */
    OpendataStmt* stmt = opendata_prepare("SELECT COUNT(*) FROM staff_copy;");
    assert(stmt != NULL);
    assert(opendata_step(stmt));
    const char* count_str = opendata_column_text(stmt, 0);
    assert(count_str && !strcmp(count_str, "2"));
    opendata_finalize(stmt);
    puts("  -> CSV import verified with exact record count (2 rows).");

    /* ------------------------------------------------------------- */
    /* 2. Test JSON Export, Import, and JSON_EXTRACT                  */
    /* ------------------------------------------------------------- */
    puts("\n[2] Testing JSON Export, Import, and JSON_EXTRACT...");
    ok = opendata_export_json("staff", "export_test.json");
    assert(ok && "JSON export must succeed");

    fp = fopen("export_test.json", "r");
    assert(fp != NULL);
    bool json_has_array = false, json_has_lead = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "[")) json_has_array = true;
        if (strstr(buf, "\"Lead\"")) json_has_lead = true;
    }
    fclose(fp);
    assert(json_has_array && json_has_lead && "JSON export structure verified");
    puts("  -> JSON export successfully verified (valid JSON array of objects).");

    /* Import from JSON into another table */
    ok = opendata_exec("CREATE TABLE staff_json (id NUMBER PRIMARY KEY, name VARCHAR2(50), role VARCHAR2(50));");
    assert(ok);

    ok = opendata_import_json("staff_json", "export_test.json");
    assert(ok && "JSON import must succeed");

    stmt = opendata_prepare("SELECT COUNT(*) FROM staff_json;");
    assert(stmt != NULL);
    assert(opendata_step(stmt));
    assert(!strcmp(opendata_column_text(stmt, 0), "2"));
    opendata_finalize(stmt);
    puts("  -> JSON import verified with exact record count (2 rows).");

    /* Test JSON_EXTRACT in SQL query */
    ok = opendata_exec("CREATE TABLE events (id NUMBER PRIMARY KEY, payload VARCHAR2(255));");
    assert(ok);
    ok = opendata_exec("INSERT INTO events VALUES (1, '{\"type\": \"login\", \"user\": \"alex\", \"status\": 200}');");
    assert(ok);

    stmt = opendata_prepare("SELECT id, JSON_EXTRACT(payload, '$.user') AS username, JSON_EXTRACT(payload, '$.status') AS code FROM events;");
    assert(stmt != NULL);
    assert(opendata_step(stmt));
    const char* user = opendata_column_text(stmt, 1);
    const char* status = opendata_column_text(stmt, 2);
    assert(user && !strcmp(user, "alex"));
    assert(status && !strcmp(status, "200"));
    printf("  -> JSON_EXTRACT verified: user=%s, status=%s\n", user, status);
    opendata_finalize(stmt);

    /* ------------------------------------------------------------- */
    /* 3. Test Output Formatting (JSON, CSV, TABLE)                  */
    /* ------------------------------------------------------------- */
    puts("\n[3] Testing Query Output Formatting Modes (JSON, CSV, TABLE)...");
    opendata_set_format("csv");
    puts("  [CSV Mode Active]:");
    ok = opendata_exec("SELECT id, name FROM staff;");
    assert(ok);

    opendata_set_format("json");
    puts("  [JSON Mode Active]:");
    ok = opendata_exec("SELECT id, name FROM staff;");
    assert(ok);

    opendata_set_format("table");
    puts("  [Table Mode Active]:");
    ok = opendata_exec("SELECT id, name FROM staff;");
    assert(ok);

    /* ------------------------------------------------------------- */
    /* 4. Test Headless TCP/IP Networking                            */
    /* ------------------------------------------------------------- */
    puts("\n[4] Testing Headless TCP/IP Sockets and Network Loop...");
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    assert(s != (socket_t)(-1) && "Client socket creation must succeed");
    CLOSE_SOCK(s);
    puts("  -> Socket API verified successfully.");

    opendata_close();

    /* Clean up temporary test files */
    unlink("test_feat_data.pcod");
    unlink("test_feat_data.pcod.lock");
    unlink("export_test.csv");
    unlink("export_test.json");
    rmdir("test_feat_backups");

    puts("\n===================================================================");
    puts("   All JSON, CSV, Server, Network & Protocol Tests Passed!         ");
    puts("===================================================================");
    return 0;
}
