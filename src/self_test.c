#include "database_engine.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#include <share.h>
#include <sys/stat.h>
typedef SOCKET selftest_socket_t;
#define SELFTEST_INVALID_SOCKET INVALID_SOCKET
#define SELFTEST_CLOSE_SOCK(s) closesocket(s)
#define selftest_rmdir _rmdir
#define selftest_unlink _unlink
#define selftest_mkdir(p) _mkdir(p)
static void selftest_sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#if defined(OPENDATA_TLS)
#include <openssl/ssl.h>
#endif
typedef int selftest_socket_t;
#define SELFTEST_INVALID_SOCKET (-1)
#define SELFTEST_CLOSE_SOCK(s) close(s)
#define selftest_rmdir rmdir
#define selftest_unlink unlink
#define selftest_mkdir(p) mkdir(p, 0755)
static void selftest_sleep_ms(unsigned ms) {
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
#endif

static int g_st_assertions = 0;
static int g_st_failures = 0;

static void selftest_mute_io(void) {
    g_silent_mode = true;
}

static void selftest_unmute_io(void) {
    g_silent_mode = false;
}

#define ST_CHECK(cond) do { \
    g_st_assertions++; \
    if (!(cond)) { \
        selftest_unmute_io(); \
        fprintf(stderr, "\n  [FAIL] %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        g_st_failures++; \
        return false; \
    } \
} while (0)

static int selftest_open_file(const char* path) {
#ifdef _WIN32
    int fd = -1;
    if (_sopen_s(&fd, path, _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) return -1;
    return fd;
#else
    return open(path, O_CREAT | O_RDWR, 0644);
#endif
}

static void selftest_close_file(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

static void selftest_remove_db_artifacts(const char* db_path) {
    if (!db_path || !*db_path) return;
    selftest_unlink(db_path);
    char lock_path[PATH_CAP];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", db_path);
    selftest_unlink(lock_path);
    char wal_path[PATH_CAP];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", db_path);
    selftest_unlink(wal_path);
}

/* =========================================================================
 * Suite 1: Embedded C API & Transactions (from test_api.c)
 * ========================================================================= */
static bool run_suite_api(void) {
    selftest_mkdir("selftest_tmp");
    selftest_mkdir("selftest_tmp/backups");
    const char* db = "selftest_tmp/embedded.pcod";
    const char* backups = "selftest_tmp/backups";
    selftest_remove_db_artifacts(db);

    ST_CHECK(opendata_open(db, backups, 0, NULL));
    ST_CHECK(opendata_get_generation() == 0);
    ST_CHECK(!opendata_has_pending_changes());

    ST_CHECK(opendata_exec("CREATE TABLE departments (id NUMBER PRIMARY KEY, name VARCHAR2(50) NOT NULL);"));
    ST_CHECK(opendata_has_pending_changes());
    ST_CHECK(opendata_exec("CREATE TABLE employees (id NUMBER PRIMARY KEY, name VARCHAR2(50) NOT NULL, dept_id NUMBER REFERENCES departments(id));"));
    ST_CHECK(opendata_exec("INSERT INTO departments VALUES (1, 'Engineering');"));
    ST_CHECK(opendata_exec("INSERT INTO departments VALUES (2, 'Design');"));
    ST_CHECK(opendata_exec("INSERT INTO employees VALUES (10, 'Alice', 1);"));
    ST_CHECK(opendata_exec("INSERT INTO employees VALUES (20, 'Bob', 1);"));

    /* FK and PK constraint enforcement */
    ST_CHECK(!opendata_exec("INSERT INTO employees VALUES (30, 'Charlie', 99);"));
    ST_CHECK(!opendata_exec("INSERT INTO employees VALUES (10, 'Duplicate', 1);"));

    ST_CHECK(opendata_commit());
    ST_CHECK(opendata_get_generation() == 1);
    ST_CHECK(!opendata_has_pending_changes());

    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;
    ST_CHECK(opendata_exec("SELECT employees.name, departments.name FROM employees INNER JOIN departments ON employees.dept_id = departments.id;"));
    ST_CHECK(cap.row_count == 2);
    free(cap.rows);
    g_result_capture = NULL;

    ST_CHECK(opendata_exec("UPDATE employees SET name = 'Modified' WHERE id = 10;"));
    ST_CHECK(opendata_has_pending_changes());
    ST_CHECK(opendata_rollback());
    ST_CHECK(!opendata_has_pending_changes());
    ST_CHECK(opendata_get_generation() == 1);

    opendata_close();
    selftest_remove_db_artifacts(db);
    return true;
}

/* =========================================================================
 * Suite 2: Authentication Rate Limiting (from test_auth_rate_limit.c)
 * ========================================================================= */
static bool run_suite_auth_rate_limit(void) {
    reset_auth_rate_limit();
    for (unsigned i = 0; i < 5; i++)
        ST_CHECK(admit_auth_attempt(1, "admin", 0));
    ST_CHECK(!admit_auth_attempt(1, "admin", 0));
    ST_CHECK(admit_auth_attempt(1, "other-account", 0));
    ST_CHECK(admit_auth_attempt(2, "admin", 0));
    ST_CHECK(!admit_auth_attempt(1, "admin", 4999));
    ST_CHECK(admit_auth_attempt(1, "admin", 5000));
    ST_CHECK(!admit_auth_attempt(1, "admin", 5000));
    ST_CHECK(!admit_auth_attempt(1, "admin", 4999));
    ST_CHECK(admit_auth_attempt(1, "admin", 65000));

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 20; i++) {
        char user[32];
        snprintf(user, sizeof(user), "unknown-%u", i);
        ST_CHECK(admit_auth_attempt(1, user, 0));
    }
    ST_CHECK(!admit_auth_attempt(1, "new-name", 0));
    ST_CHECK(admit_auth_attempt(2, "admin", 0));
    ST_CHECK(!admit_auth_attempt(1, "new-name", 1999));
    ST_CHECK(admit_auth_attempt(1, "new-name", 2000));
    ST_CHECK(auth_rate_limit_stats().source_throttled == 2);

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 64; i++)
        ST_CHECK(admit_auth_attempt(i, "admin", 0));
    ST_CHECK(!admit_auth_attempt(100, "admin", 99));
    ST_CHECK(admit_auth_attempt(100, "admin", 100));
    ST_CHECK(auth_rate_limit_stats().global_throttled == 1);

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 256; i++)
        ST_CHECK(admit_auth_attempt(i, "admin", (uint64_t)i * 100));
    ST_CHECK(!admit_auth_attempt(256, "admin", 25600));
    ST_CHECK(auth_rate_limit_stats().source_saturated == 1);
    ST_CHECK(admit_auth_attempt(0, "admin", 25600));
    ST_CHECK(admit_auth_attempt(256, "admin", 85601));

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 5; i++)
        ST_CHECK(admit_auth_attempt(7, "disabled-or-unknown", 0));
    for (unsigned i = 0; i < 15; i++) {
        char user[32];
        snprintf(user, sizeof(user), "churn-%u", i);
        ST_CHECK(admit_auth_attempt(7, user, 0));
    }
    ST_CHECK(!admit_auth_attempt(7, "disabled-or-unknown", 0));
    ST_CHECK(admit_auth_attempt(8, "disabled-or-unknown", 0));

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 512; i++) {
        char user[32];
        snprintf(user, sizeof(user), "pair-%u", i);
        ST_CHECK(admit_auth_attempt(i % 64, user, (uint64_t)i * 100));
    }
    ST_CHECK(!admit_auth_attempt(100, "new-pair", 51200));
    ST_CHECK(admit_auth_attempt(0, "pair-0", 51200));
    ST_CHECK(!admit_auth_attempt(100, "new-pair", 59999));
    ST_CHECK(admit_auth_attempt(100, "new-pair", 60100));
    AuthRateLimitStats stats = auth_rate_limit_stats();
    ST_CHECK(stats.pair_saturated == 2);
    ST_CHECK(stats.admitted == 514);
    ST_CHECK(stats.credential_failures == 0);
    record_auth_credential_failure();
    ST_CHECK(auth_rate_limit_stats().credential_failures == 1);
    reset_auth_rate_limit();
    ST_CHECK(auth_rate_limit_stats().admitted == 0);
    return true;
}

/* =========================================================================
 * Suite 3: Memory Safety & Boundary Regressions (from test_memory_safety.c)
 * ========================================================================= */
static bool run_suite_memory_safety(void) {
    selftest_mkdir("selftest_tmp");
    const char* db = "selftest_tmp/memory_safety.pcod";
    selftest_remove_db_artifacts(db);

    ST_CHECK(opendata_open(db, NULL, 0, NULL));
    ST_CHECK(opendata_exec("CREATE TABLE numbers (n NUMBER, v VARCHAR2(255));"));
    for (int i = 0; i < 10; i++) {
        char sql[100];
        snprintf(sql, sizeof(sql), "INSERT INTO numbers VALUES (%d, 'aaaaab');", i);
        ST_CHECK(opendata_exec(sql));
    }
    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;
    bool quit = false;
    ST_CHECK(execute_console_statement("SELECT n FROM numbers ORDER BY n LIMIT 2 OFFSET 3;", &quit));
    ST_CHECK(cap.row_count == 2 && cap.capacity >= 2);
    ST_CHECK(!strcmp(cap.rows[0][0].text, "3") && !strcmp(cap.rows[1][0].text, "4"));
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;

    ST_CHECK(execute_console_statement("SELECT n FROM numbers ORDER BY n LIMIT 4 OFFSET 9;", &quit));
    ST_CHECK(cap.row_count == 1 && !strcmp(cap.rows[0][0].text, "9"));
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;

    ST_CHECK(execute_console_statement("SELECT a.n FROM numbers a WHERE EXISTS (SELECT b.n FROM numbers b WHERE a.n=b.n);", &quit));
    ST_CHECK(cap.row_count == 10);
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;

    ST_CHECK(execute_console_statement("SELECT n FROM numbers WHERE v LIKE '%a%a%a%a%a%b';", &quit));
    ST_CHECK(cap.row_count == 10);
    ST_CHECK(execute_console_statement("SELECT n FROM numbers WHERE v LIKE '%a%a%a%a%a%c';", &quit));
    ST_CHECK(cap.row_count == 0);
    free(cap.rows);
    g_result_capture = NULL;

    char* deep = malloc(10000);
    ST_CHECK(deep != NULL);
    size_t off = (size_t)snprintf(deep, 10000, "SELECT n FROM numbers WHERE ");
    for (int i = 0; i < 4000; i++) deep[off++] = '(';
    memcpy(deep + off, "n=1", 3); off += 3;
    for (int i = 0; i < 4000; i++) deep[off++] = ')';
    deep[off++] = ';'; deep[off] = 0;
    ST_CHECK(!opendata_exec(deep));
    free(deep);

    ST_CHECK(!opendata_exec("@U\x08#p"));
    ST_CHECK(!opendata_exec("SOURCE U\x08#p"));
    ST_CHECK(!opendata_exec("EXPLAIN SELECT n FROM numbers WHERE n=\x08"));

    const char* rec_sql = "selftest_tmp/recursive.sql";
    FILE* recursive = fopen(rec_sql, "w");
    ST_CHECK(recursive != NULL);
    ST_CHECK(fputs("SOURCE 'selftest_tmp/recursive.sql';\n", recursive) >= 0);
    ST_CHECK(fclose(recursive) == 0);
    ST_CHECK(!opendata_exec("SOURCE 'selftest_tmp/recursive.sql';"));
    selftest_unlink(rec_sql);

    opendata_close();
    selftest_remove_db_artifacts(db);
    return true;
}

/* =========================================================================
 * Suite 4: JSON, CSV, Formatting & Sockets (from test_features_suite.c)
 * ========================================================================= */
static bool run_suite_features(void) {
    selftest_mkdir("selftest_tmp");
    const char* db = "selftest_tmp/feat_data.pcod";
    const char* csv_file = "selftest_tmp/export_test.csv";
    const char* json_file = "selftest_tmp/export_test.json";
    selftest_remove_db_artifacts(db);
    selftest_unlink(csv_file);
    selftest_unlink(json_file);

    ST_CHECK(opendata_open(db, NULL, 0, NULL));
    ST_CHECK(opendata_exec("CREATE TABLE staff (id NUMBER PRIMARY KEY, name VARCHAR2(50), role VARCHAR2(50));"));
    ST_CHECK(opendata_exec("INSERT INTO staff VALUES (1, 'Alice, Developer', 'Lead'), (2, 'Bob \"The Builder\"', 'Engineer');"));

    ST_CHECK(opendata_export_csv("staff", csv_file));
    FILE* fp = fopen(csv_file, "r");
    ST_CHECK(fp != NULL);
    char buf[1024];
    bool found_header = false, found_alice = false, found_bob = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "ID,NAME,ROLE")) found_header = true;
        if (strstr(buf, "\"Alice, Developer\"")) found_alice = true;
        if (strstr(buf, "\"Bob \"\"The Builder\"\"\"")) found_bob = true;
    }
    fclose(fp);
    ST_CHECK(found_header && found_alice && found_bob);

    ST_CHECK(opendata_exec("CREATE TABLE staff_copy (id NUMBER PRIMARY KEY, name VARCHAR2(50), role VARCHAR2(50));"));
    ST_CHECK(opendata_import_csv("staff_copy", csv_file, true));

    OpendataStmt* stmt = opendata_prepare("SELECT COUNT(*) FROM staff_copy;");
    ST_CHECK(stmt != NULL);
    ST_CHECK(opendata_step(stmt));
    ST_CHECK(!strcmp(opendata_column_text(stmt, 0), "2"));
    opendata_finalize(stmt);

    ST_CHECK(opendata_export_json("staff", json_file));
    fp = fopen(json_file, "r");
    ST_CHECK(fp != NULL);
    bool json_has_array = false, json_has_lead = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "[")) json_has_array = true;
        if (strstr(buf, "\"Lead\"")) json_has_lead = true;
    }
    fclose(fp);
    ST_CHECK(json_has_array && json_has_lead);

    ST_CHECK(opendata_exec("CREATE TABLE staff_json (id NUMBER PRIMARY KEY, name VARCHAR2(50), role VARCHAR2(50));"));
    ST_CHECK(opendata_import_json("staff_json", json_file));

    stmt = opendata_prepare("SELECT COUNT(*) FROM staff_json;");
    ST_CHECK(stmt != NULL && opendata_step(stmt));
    ST_CHECK(!strcmp(opendata_column_text(stmt, 0), "2"));
    opendata_finalize(stmt);

    ST_CHECK(opendata_exec("CREATE TABLE events (id NUMBER PRIMARY KEY, payload VARCHAR2(255));"));
    ST_CHECK(opendata_exec("INSERT INTO events VALUES (1, '{\"type\": \"login\", \"user\": \"alex\", \"status\": 200}');"));
    stmt = opendata_prepare("SELECT id, JSON_EXTRACT(payload, '$.user') AS username, JSON_EXTRACT(payload, '$.status') AS code FROM events;");
    ST_CHECK(stmt != NULL && opendata_step(stmt));
    ST_CHECK(!strcmp(opendata_column_text(stmt, 1), "alex"));
    ST_CHECK(!strcmp(opendata_column_text(stmt, 2), "200"));
    opendata_finalize(stmt);

    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;
    opendata_set_format("csv");
    ST_CHECK(opendata_exec("SELECT id, name FROM staff;"));
    opendata_set_format("json");
    ST_CHECK(opendata_exec("SELECT id, name FROM staff;"));
    opendata_set_format("table");
    ST_CHECK(opendata_exec("SELECT id, name FROM staff;"));
    free(cap.rows);
    g_result_capture = NULL;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    selftest_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    ST_CHECK(s != SELFTEST_INVALID_SOCKET);
    SELFTEST_CLOSE_SOCK(s);

    opendata_close();
    selftest_remove_db_artifacts(db);
    selftest_unlink(csv_file);
    selftest_unlink(json_file);
    return true;
}

static void st_tp_task(void* arg) {
    atomic_int* counter = (atomic_int*)arg;
    atomic_fetch_add(counter, 1);
}

/* =========================================================================
 * Suite 5: Advanced Recommendations (from test_recommendations.c)
 * ========================================================================= */
static bool run_suite_recommendations(void) {
    selftest_mkdir("selftest_tmp");
    const char* db = "selftest_tmp/rec_data.pcod";
    selftest_remove_db_artifacts(db);

    ST_CHECK(opendata_open(db, NULL, 0, NULL));

    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;

    /* 1. Unified Storage Engine Dispatch (MEMORY vs DISK) */
    ST_CHECK(opendata_exec("CREATE TABLE cache_tbl (id NUMBER PRIMARY KEY, val VARCHAR2(50)) ENGINE = MEMORY;"));
    ST_CHECK(opendata_exec("CREATE TABLE store_tbl (id NUMBER PRIMARY KEY, val VARCHAR2(50)) ENGINE = DISK;"));
    ST_CHECK(opendata_exec("INSERT INTO store_tbl VALUES (1, 'DiskRecord1'), (2, 'DiskRecord2');"));
    ST_CHECK(opendata_exec("SELECT * FROM store_tbl ORDER BY id ASC;"));
    ST_CHECK(cap.row_count == 2);

    /* 2. N-Way Joins (4-table relational chain) */
    ST_CHECK(opendata_exec("CREATE TABLE customers (id NUMBER PRIMARY KEY, name VARCHAR2(50));"));
    ST_CHECK(opendata_exec("CREATE TABLE orders (id NUMBER PRIMARY KEY, cust_id NUMBER);"));
    ST_CHECK(opendata_exec("CREATE TABLE items (id NUMBER PRIMARY KEY, order_id NUMBER, prod_id NUMBER, qty NUMBER);"));
    ST_CHECK(opendata_exec("CREATE TABLE products (id NUMBER PRIMARY KEY, title VARCHAR2(50), price NUMBER);"));
    ST_CHECK(opendata_exec("INSERT INTO customers VALUES (1, 'Acme Corp'), (2, 'Beta LLC');"));
    ST_CHECK(opendata_exec("INSERT INTO orders VALUES (101, 1), (102, 2);"));
    ST_CHECK(opendata_exec("INSERT INTO items VALUES (1001, 101, 501, 10), (1002, 101, 502, 5), (1003, 102, 501, 2);"));
    ST_CHECK(opendata_exec("INSERT INTO products VALUES (501, 'Widget A', 25), (502, 'Widget B', 50);"));

    ST_CHECK(opendata_exec("SELECT customers.name, orders.id, products.title, items.qty "
                           "FROM customers "
                           "INNER JOIN orders ON customers.id = orders.cust_id "
                           "INNER JOIN items ON orders.id = items.order_id "
                           "LEFT JOIN products ON items.prod_id = products.id "
                           "ORDER BY items.id ASC;"));
    ST_CHECK(cap.row_count == 3);

    /* 3. Scalar Arithmetic Expressions */
    ST_CHECK(opendata_exec("CREATE TABLE payroll (id NUMBER PRIMARY KEY, name VARCHAR2(50), salary NUMBER);"));
    ST_CHECK(opendata_exec("INSERT INTO payroll VALUES (1, 'Alice', 10000), (2, 'Bob', 8000), (3, 'Charlie', 5000);"));
    ST_CHECK(opendata_exec("SELECT name, salary * 1.10 AS bonus_salary, salary + 500 AS adjusted, salary / 2 AS half FROM payroll ORDER BY id ASC;"));
    ST_CHECK(cap.row_count == 3);
    ST_CHECK(opendata_exec("SELECT name, salary FROM payroll WHERE salary * 12 > 100000 ORDER BY salary DESC;"));
    ST_CHECK(cap.row_count == 1);

    /* 4. Subqueries and CTEs */
    ST_CHECK(opendata_exec("WITH high_earners AS (SELECT name, salary FROM payroll WHERE salary >= 8000) SELECT name, salary FROM high_earners ORDER BY salary DESC;"));
    ST_CHECK(cap.row_count == 2);
    ST_CHECK(opendata_exec("SELECT name FROM payroll WHERE id IN (SELECT cust_id FROM orders WHERE id = 101);"));
    ST_CHECK(cap.row_count == 1);

    free(cap.rows);
    g_result_capture = NULL;

    /* 5. Prepared Statements & Parameter Binding */
    OpendataStmt* stmt = opendata_prepare("SELECT name, salary FROM payroll WHERE salary >= ? ORDER BY salary DESC;");
    ST_CHECK(stmt != NULL);
    ST_CHECK(opendata_bind_int(stmt, 1, 8000));
    int rows_found = 0;
    while (opendata_step(stmt)) {
        ST_CHECK(opendata_column_text(stmt, 0) != NULL && opendata_column_text(stmt, 1) != NULL);
        rows_found++;
    }
    ST_CHECK(rows_found == 2);

    ST_CHECK(opendata_reset(stmt));
    ST_CHECK(opendata_bind_int(stmt, 1, 10000));
    rows_found = 0;
    while (opendata_step(stmt)) rows_found++;
    ST_CHECK(rows_found == 1);
    opendata_finalize(stmt);

    ST_CHECK(opendata_commit());
    opendata_close();

    /* 6. SWMR Concurrency (Read-Only Mode) */
    ST_CHECK(opendata_open_readonly(db));
    memset(&cap, 0, sizeof(cap));
    cap.silent = true;
    g_result_capture = &cap;
    ST_CHECK(opendata_exec("SELECT name, salary FROM payroll ORDER BY salary DESC;"));
    ST_CHECK(cap.row_count == 3);
    free(cap.rows);
    g_result_capture = NULL;

    ST_CHECK(!opendata_exec("INSERT INTO payroll VALUES (4, 'Dave', 7000);"));
    ST_CHECK(!opendata_exec("UPDATE payroll SET salary = 20000 WHERE id = 1;"));
    ST_CHECK(!opendata_commit());

    /* 7. Phase 1 Lock Manager & Multi-Writer 2PL */
    lock_manager_init();
    ST_CHECK(lock_manager_acquire(101, "orders", LOCK_MODE_EXCLUSIVE, 100));
    ST_CHECK(lock_manager_acquire(101, "orders", LOCK_MODE_EXCLUSIVE, 100));
    ST_CHECK(lock_manager_acquire(102, "customers", LOCK_MODE_EXCLUSIVE, 100));
    ST_CHECK(!lock_manager_acquire(102, "orders", LOCK_MODE_EXCLUSIVE, 50));
    ST_CHECK(lock_manager_has_conflict(102, "orders", LOCK_MODE_EXCLUSIVE));
    ST_CHECK(!lock_manager_has_conflict(101, "orders", LOCK_MODE_EXCLUSIVE));
    lock_manager_release_session(101);
    ST_CHECK(!lock_manager_has_conflict(102, "orders", LOCK_MODE_EXCLUSIVE));
    ST_CHECK(lock_manager_acquire(102, "orders", LOCK_MODE_EXCLUSIVE, 100));
    lock_manager_release_session(102);

    /* 8. Phase 2 Worker Thread Pool & Scaled Concurrency */
    ThreadPool* tp = thread_pool_create(4, 32);
    ST_CHECK(tp != NULL);
    ST_CHECK(thread_pool_worker_count(tp) == 4);
    atomic_int tp_counter = 0;
    for (int k = 0; k < 16; k++) {
        ST_CHECK(thread_pool_submit(tp, st_tp_task, &tp_counter));
    }
    for (int poll = 0; poll < 100 && atomic_load(&tp_counter) < 16; poll++) {
        selftest_sleep_ms(10);
    }
    ST_CHECK(atomic_load(&tp_counter) == 16);
    thread_pool_destroy(tp);

    opendata_close();
    selftest_remove_db_artifacts(db);
    return true;
}

/* =========================================================================
 * Suite 6: SQL Engine Improvements (from test_sql_improvements.c)
 * ========================================================================= */
static bool run_suite_sql_improvements(void) {
    selftest_mkdir("selftest_tmp");
    const char* db = "selftest_tmp/sql_imp.pcod";
    selftest_remove_db_artifacts(db);

    ST_CHECK(opendata_open(db, NULL, 0, NULL));
    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;

    ST_CHECK(opendata_exec("CREATE TABLE staff (id NUMBER PRIMARY KEY, name VARCHAR2(50), dept VARCHAR2(30), salary NUMBER);"));
    ST_CHECK(opendata_exec("INSERT INTO staff VALUES "
                           "(1, 'Alice', 'Engineering', 95000), "
                           "(2, 'Bob', 'Engineering', 85000), "
                           "(3, 'Charlie', 'Marketing', 70000), "
                           "(4, 'Diana', 'Marketing', 90000), "
                           "(5, 'Eve', 'HR', 65000);"));
    ST_CHECK(opendata_exec("INSERT INTO staff (id, name) VALUES (6, 'Frank'), (7, 'Grace');"));

    ST_CHECK(opendata_exec("SELECT id, name, salary FROM staff WHERE salary IS NOT NULL ORDER BY salary DESC;"));
    ST_CHECK(cap.row_count == 5);
    ST_CHECK(opendata_exec("SELECT id, name, dept FROM staff ORDER BY dept ASC, name DESC;"));
    ST_CHECK(opendata_exec("SELECT id, name FROM staff ORDER BY id ASC LIMIT 3 OFFSET 2;"));
    ST_CHECK(cap.row_count == 3);

    ST_CHECK(opendata_exec("SELECT id, name FROM staff WHERE dept = 'HR' OR dept = 'Marketing' ORDER BY id ASC;"));
    ST_CHECK(cap.row_count == 3);
    ST_CHECK(opendata_exec("SELECT name, salary FROM staff WHERE (dept = 'Engineering' OR dept = 'Marketing') AND salary >= 90000 ORDER BY salary DESC;"));
    ST_CHECK(cap.row_count == 2);

    ST_CHECK(opendata_exec("SELECT name FROM staff WHERE name LIKE 'A%' ORDER BY name ASC;"));
    ST_CHECK(cap.row_count == 1);
    ST_CHECK(opendata_exec("SELECT name FROM staff WHERE name LIKE '%e' ORDER BY name ASC;"));
    ST_CHECK(cap.row_count == 4);
    ST_CHECK(opendata_exec("SELECT name FROM staff WHERE name LIKE '%ar%' ORDER BY name ASC;"));
    ST_CHECK(cap.row_count == 1);

    ST_CHECK(opendata_exec("SELECT name FROM staff WHERE id IN (1, 3, 5) ORDER BY id ASC;"));
    ST_CHECK(cap.row_count == 3);
    ST_CHECK(opendata_exec("SELECT name FROM staff WHERE id NOT IN (1, 2, 3, 4, 5) ORDER BY id ASC;"));
    ST_CHECK(cap.row_count == 2);
    ST_CHECK(opendata_exec("SELECT name, salary FROM staff WHERE salary BETWEEN 70000 AND 90000 ORDER BY salary ASC;"));
    ST_CHECK(cap.row_count == 3);

    ST_CHECK(opendata_exec("SELECT COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary) FROM staff WHERE salary IS NOT NULL;"));
    ST_CHECK(cap.row_count == 1);
    ST_CHECK(opendata_exec("SELECT dept, COUNT(*), SUM(salary), MIN(salary), MAX(salary) FROM staff WHERE dept IS NOT NULL GROUP BY dept ORDER BY dept ASC;"));
    ST_CHECK(cap.row_count == 3);

    ST_CHECK(opendata_exec("SELECT DISTINCT dept FROM staff WHERE dept IS NOT NULL ORDER BY dept ASC;"));
    ST_CHECK(cap.row_count == 3);
    ST_CHECK(opendata_exec("SELECT name AS employee_name, salary AS compensation FROM staff WHERE id = 1;"));
    ST_CHECK(opendata_exec("SELECT dept AS department, COUNT(*) AS head_count FROM staff WHERE dept IS NOT NULL GROUP BY dept ORDER BY department ASC;"));

    ST_CHECK(opendata_exec("CREATE TABLE projects (id NUMBER PRIMARY KEY, lead_id NUMBER, title VARCHAR2(50));"));
    ST_CHECK(opendata_exec("INSERT INTO projects VALUES (101, 1, 'Search Engine'), (102, 2, 'Cloud Storage'), (103, 99, 'Unassigned Project');"));
    ST_CHECK(opendata_exec("SELECT staff.name, projects.title FROM staff LEFT JOIN projects ON staff.id = projects.lead_id WHERE staff.id <= 3 ORDER BY staff.id ASC;"));
    ST_CHECK(cap.row_count == 3);

    ST_CHECK(opendata_exec("ALTER TABLE staff ADD COLUMN email VARCHAR2(80);"));
    ST_CHECK(opendata_exec("SELECT id, name, email FROM staff WHERE id = 1;"));
    ST_CHECK(opendata_exec("ALTER TABLE staff RENAME COLUMN email TO contact_email;"));
    ST_CHECK(opendata_exec("ALTER TABLE staff DROP COLUMN contact_email;"));
    ST_CHECK(opendata_exec("ALTER TABLE projects RENAME TO initiatives;"));
    ST_CHECK(opendata_exec("SELECT * FROM initiatives ORDER BY id ASC;"));
    ST_CHECK(opendata_exec("EXPLAIN SELECT staff.name, initiatives.title FROM staff LEFT JOIN initiatives ON staff.id = initiatives.lead_id WHERE staff.salary > 80000 ORDER BY staff.salary DESC LIMIT 5;"));

    bool saved_confirm = g_confirm_destructive;
    g_confirm_destructive = false;
    ST_CHECK(opendata_exec("TRUNCATE TABLE initiatives;"));
    g_confirm_destructive = saved_confirm;

    ST_CHECK(opendata_exec("SELECT COUNT(*) FROM initiatives;"));
    ST_CHECK(cap.row_count == 1 && !strcmp(cap.rows[0][0].text, "0"));

    /* Phase 4: Web OLTP SQL Primitives */
    /* 1. SELECT FOR UPDATE */
    g_current_session_id = 10;
    ST_CHECK(opendata_exec("SELECT * FROM staff WHERE id = 1 FOR UPDATE;"));
    ST_CHECK(cap.row_count == 1);
    ST_CHECK(lock_manager_has_row_conflict(20, "STAFF", 1, LOCK_MODE_EXCLUSIVE));

    /* 2. SELECT FOR UPDATE SKIP LOCKED */
    g_current_session_id = 20;
    ST_CHECK(opendata_exec("SELECT * FROM staff FOR UPDATE SKIP LOCKED;"));
    for (int i = 0; i < cap.row_count; i++) {
        ST_CHECK(strcmp(cap.rows[i][0].text, "1") != 0);
    }

    lock_manager_release_session(10);
    lock_manager_release_session(20);
    g_current_session_id = 1;

    /* 3. Keyset seek / early limit pagination */
    ST_CHECK(opendata_exec("SELECT * FROM staff LIMIT 2 OFFSET 1;"));
    ST_CHECK(cap.row_count == 2);

    /* 4. Deadlock Detection Engine (WFG cycle) */
    lock_manager_init();
    ST_CHECK(lock_manager_acquire_row(1, "STAFF", 100, LOCK_MODE_EXCLUSIVE, 100));
    ST_CHECK(lock_manager_acquire_row(2, "STAFF", 200, LOCK_MODE_EXCLUSIVE, 100));
    g_lock_manager.wait_for[1] = 2;
    ST_CHECK(!lock_manager_acquire_row(2, "STAFF", 100, LOCK_MODE_EXCLUSIVE, 100));
    ST_CHECK(strstr(g_last_error, "40P01") != NULL);
    lock_manager_init();

    /* 5. Fine-Grained In-Memory Undo Log & Zero-Copy Rollback */
    ST_CHECK(opendata_exec("CREATE TABLE audit_trail (id NUMBER PRIMARY KEY, action VARCHAR2(50));"));
    ST_CHECK(opendata_commit());

    ST_CHECK(opendata_exec("INSERT INTO audit_trail VALUES (1, 'LOGIN'), (2, 'READ'), (3, 'WRITE');"));
    ST_CHECK(g_undo_log.entry_count == 3);

    ST_CHECK(opendata_exec("UPDATE audit_trail SET action = 'LOGOUT' WHERE id = 2;"));
    ST_CHECK(g_undo_log.entry_count == 4);

    ST_CHECK(opendata_exec("DELETE FROM audit_trail WHERE id = 1;"));
    ST_CHECK(g_undo_log.entry_count == 5);

    ST_CHECK(opendata_rollback());
    ST_CHECK(g_undo_log.entry_count == 0);

    ST_CHECK(opendata_exec("SELECT COUNT(*) FROM audit_trail;"));
    ST_CHECK(cap.row_count == 1 && !strcmp(cap.rows[0][0].text, "0"));

    /* 6. Dynamic Sized Buffer Pool Testing */
    BufferPool dyn_bp;
    buffer_pool_init_sized(&dyn_bp, 4);
    ST_CHECK(dyn_bp.frame_count == 1024);
    ST_CHECK(dyn_bp.hash_size == 2048);
    for (int p = 0; p < 1500; p++) {
        uint32_t pid = buffer_pool_alloc_page(&dyn_bp, -1, PAGE_TYPE_DATA);
        uint8_t* pmem = buffer_pool_fetch(&dyn_bp, -1, pid, true);
        ST_CHECK(pmem != NULL);
        pmem[0] = (uint8_t)(pid & 0xFF);
        buffer_pool_unpin(&dyn_bp, pid, true);
    }
    buffer_pool_destroy(&dyn_bp, -1);

    free(cap.rows);
    g_result_capture = NULL;

    ST_CHECK(opendata_commit());
    opendata_close();
    selftest_remove_db_artifacts(db);
    return true;
}

/* =========================================================================
 * Suite 7: Usability & Dialect Tolerances (from test_usability.c)
 * ========================================================================= */
static bool run_suite_usability(void) {
    char size_buf[32];
    format_human_size(500, size_buf, sizeof(size_buf));
    ST_CHECK(strcmp(size_buf, "500 B") == 0);
    format_human_size(2048, size_buf, sizeof(size_buf));
    ST_CHECK(strcmp(size_buf, "2.0 KB") == 0);
    format_human_size(1048576 * 5, size_buf, sizeof(size_buf));
    ST_CHECK(strcmp(size_buf, "5.0 MB") == 0);

    char time_buf[32];
    format_relative_timestamp(time(NULL) - 10, time_buf, sizeof(time_buf));
    ST_CHECK(strstr(time_buf, "sec") != NULL);
    format_relative_timestamp(time(NULL) - 120, time_buf, sizeof(time_buf));
    ST_CHECK(strstr(time_buf, "min") != NULL);

    ST_CHECK(edit_distance_capped("custmers", "customers", 3) == 1);
    ST_CHECK(edit_distance_capped("totally_different", "customers", 3) > 3);

    selftest_mkdir("selftest_tmp");
    const char* db = "selftest_tmp/usability.pcod";
    selftest_remove_db_artifacts(db);
    ST_CHECK(opendata_open(db, NULL, 0, NULL));

    ST_CHECK(opendata_exec("CREATE TABLE IF NOT EXISTS test_users ("
                           "id SERIAL PRIMARY KEY, "
                           "name VARCHAR DEFAULT 'Anonymous', "
                           "is_active BOOLEAN DEFAULT TRUE, "
                           "bio TEXT);"));
    ST_CHECK(opendata_exec("CREATE TABLE IF NOT EXISTS test_users (id NUMBER);"));
    ST_CHECK(opendata_exec("INSERT INTO test_users (name, is_active, bio) VALUES ('Alice', TRUE, 'Loves coding');"));
    ST_CHECK(opendata_exec("INSERT INTO test_users (name, is_active, bio) VALUES ('Bob', FALSE, 'Prefers reading');"));
    ST_CHECK(opendata_exec("INSERT INTO test_users (is_active) VALUES (TRUE);"));
    ST_CHECK(opendata_exec("INSERT INTO `test_users` (\"name\", \"is_active\") VALUES ('Carol', TRUE);"));

    OpendataStmt* stmt = opendata_prepare("SELECT name FROM test_users WHERE name ILIKE 'a%';");
    ST_CHECK(stmt != NULL && opendata_step(stmt));
    ST_CHECK(strcmp(opendata_column_text(stmt, 0), "Alice") == 0);
    opendata_finalize(stmt);

    stmt = opendata_prepare("SELECT name FROM test_users WHERE name NOT ILIKE 'a%';");
    ST_CHECK(stmt != NULL);
    int non_a_count = 0;
    while (opendata_step(stmt)) non_a_count++;
    ST_CHECK(non_a_count >= 2);
    opendata_finalize(stmt);

    ST_CHECK(opendata_exec("CREATE TABLE user_names AS SELECT name, bio FROM test_users;"));
    stmt = opendata_prepare("SELECT * FROM user_names;");
    ST_CHECK(stmt != NULL);
    int ctas_rows = 0;
    while (opendata_step(stmt)) ctas_rows++;
    ST_CHECK(ctas_rows >= 3);
    opendata_finalize(stmt);

    ST_CHECK(opendata_exec("CREATE TABLE user_archive (name VARCHAR(100), bio VARCHAR(100));"));
    ST_CHECK(opendata_exec("INSERT INTO user_archive SELECT name, bio FROM test_users;"));
    stmt = opendata_prepare("SELECT COUNT(*) FROM user_archive;");
    ST_CHECK(stmt != NULL && opendata_step(stmt));
    ST_CHECK(opendata_column_int64(stmt, 0) >= 3);
    opendata_finalize(stmt);

    ST_CHECK(opendata_exec("ALTER TABLE user_archive MODIFY COLUMN bio VARCHAR(200);"));

    stmt = opendata_prepare("SELECT 1 + 1 AS calc, 50 * 2 AS mult;");
    ST_CHECK(stmt != NULL && opendata_step(stmt));
    ST_CHECK(strcmp(opendata_column_text(stmt, 0), "2") == 0);
    ST_CHECK(strcmp(opendata_column_text(stmt, 1), "100") == 0);
    opendata_finalize(stmt);

    ST_CHECK(opendata_exec("DROP TABLE IF EXISTS non_existent_table;"));

    ST_CHECK(opendata_exec("CREATE TABLE metrics (id NUMBER PRIMARY KEY, score NUMBER, emb VECTOR(3));"));
    stmt = opendata_prepare("INSERT INTO metrics VALUES (?, ?, ?);");
    ST_CHECK(stmt != NULL);
    opendata_bind_int(stmt, 1, 101);
    opendata_bind_double(stmt, 2, 98.75);
    float test_vec[3] = {1.0f, 0.5f, -0.5f};
    opendata_bind_vector(stmt, 3, test_vec, 3);
    (void)opendata_step(stmt);
    opendata_finalize(stmt);

    stmt = opendata_prepare("SELECT id, score, emb FROM metrics WHERE id = 101;");
    ST_CHECK(stmt != NULL && opendata_step(stmt));
    ST_CHECK(opendata_column_int64(stmt, 0) == 101);
    double score = opendata_column_double(stmt, 1);
    ST_CHECK(score > 98.7 && score < 98.8);
    float out_vec[3];
    int dim = opendata_column_vector(stmt, 2, out_vec, 3);
    ST_CHECK(dim == 3 && out_vec[0] > 0.99f && out_vec[0] < 1.01f);
    opendata_finalize(stmt);

    record_history_entry("SELECT * FROM test_users;");
    record_history_entry("SELECT id, name FROM test_users;");
    char exp_out[512];
    ST_CHECK(history_expand("!!", exp_out, sizeof(exp_out)));
    ST_CHECK(strcmp(exp_out, "SELECT id, name FROM test_users;") == 0);

    bool dummy_quit = false;
    ST_CHECK(execute_console_statement("SET CONFIRM OFF;", &dummy_quit));
    ST_CHECK(!g_confirm_destructive);
    ST_CHECK(execute_console_statement("SET CONFIRM ON;", &dummy_quit));
    ST_CHECK(g_confirm_destructive);

    ST_CHECK(execute_console_statement("DRYRUN DROP TABLE metrics;", &dummy_quit));
    ST_CHECK(find_table_index(g_db, "METRICS") >= 0);

    ST_CHECK(execute_console_statement("TEST TRUNCATE TABLE metrics;", &dummy_quit));
    int t_idx = find_table_index(g_db, "METRICS");
    ST_CHECK(t_idx >= 0 && g_db->tables[t_idx].row_count > 0);

    ST_CHECK(execute_console_statement("TEST DELETE FROM metrics;", &dummy_quit));
    ST_CHECK(g_db->tables[t_idx].row_count > 0);

    opendata_close();
    selftest_remove_db_artifacts(db);
    return true;
}

/* =========================================================================
 * Suite 8: Large Dataset Architecture Phases 1-4 (from test_large_data.c)
 * ========================================================================= */
static bool run_suite_large_data(void) {
    selftest_mkdir("selftest_tmp");

    /* Phase 1: Slotted Binary Rows */
    ColumnDef cols[3];
    memset(cols, 0, sizeof(cols));
    snprintf(cols[0].name, sizeof(cols[0].name), "ID");
    cols[0].type = TYPE_NUMBER; cols[0].size = 40;
    snprintf(cols[1].name, sizeof(cols[1].name), "NAME");
    cols[1].type = TYPE_VARCHAR2; cols[1].size = 50;
    snprintf(cols[2].name, sizeof(cols[2].name), "HIRED");
    cols[2].type = TYPE_DATE; cols[2].size = 19;

    Row r1;
    memset(&r1, 0, sizeof(r1));
    snprintf(r1.values[0].text, sizeof(r1.values[0].text), "42");
    snprintf(r1.values[1].text, sizeof(r1.values[1].text), "Grace Hopper");
    r1.values[2].is_null = true;

    uint8_t row_buf[512];
    size_t encoded_len = encode_slotted_row(cols, 3, &r1, row_buf, sizeof(row_buf));
    ST_CHECK(encoded_len > 0);

    Row r1_decoded;
    ST_CHECK(decode_slotted_row(cols, 3, row_buf, encoded_len, &r1_decoded));
    ST_CHECK(!strcmp(r1_decoded.values[0].text, "42"));
    ST_CHECK(!strcmp(r1_decoded.values[1].text, "Grace Hopper"));
    ST_CHECK(r1_decoded.values[2].is_null);

    Value cell;
    ST_CHECK(read_slotted_cell(&cols[1], row_buf, encoded_len, 1, 3, &cell));
    ST_CHECK(!cell.is_null && !strcmp(cell.text, "Grace Hopper"));

    /* Phase 2: Paged Storage & Buffer Pool */
    uint8_t page[PAGE_SIZE];
    ST_CHECK(slotted_page_init(page, 1, PAGE_TYPE_DATA));
    ST_CHECK(slotted_page_insert_record(page, row_buf, encoded_len) == 0);
    ST_CHECK(slotted_page_insert_record(page, row_buf, encoded_len) == 1);

    const uint8_t* rec_ptr = NULL;
    size_t rec_len = 0;
    ST_CHECK(slotted_page_get_record(page, 0, &rec_ptr, &rec_len));
    ST_CHECK(rec_len == encoded_len && !memcmp(rec_ptr, row_buf, encoded_len));

    const char* db_file = "selftest_tmp/test_paged.db";
    selftest_unlink(db_file);
    int fd = selftest_open_file(db_file);
    ST_CHECK(fd >= 0);

    BufferPool bp;
    buffer_pool_init(&bp);
    uint32_t p0 = buffer_pool_alloc_page(&bp, fd, PAGE_TYPE_DATA);
    ST_CHECK(p0 == 0);
    uint32_t p1 = buffer_pool_alloc_page(&bp, fd, PAGE_TYPE_DATA);
    ST_CHECK(p1 == 1);

    uint8_t* p0_mem = buffer_pool_fetch(&bp, fd, p0, true);
    ST_CHECK(p0_mem != NULL);
    slotted_page_insert_record(p0_mem, row_buf, encoded_len);
    buffer_pool_unpin(&bp, p0, true);

    ST_CHECK(buffer_pool_flush_all(&bp, fd));
    buffer_pool_destroy(&bp, fd);
    selftest_close_file(fd);
    selftest_unlink(db_file);

    /* Phase 3: WAL & Crash Recovery */
    const char* wal_base = "selftest_tmp/test_wal_db";
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s.dat", wal_base);
    char wal_file[256];
    snprintf(wal_file, sizeof(wal_file), "%s.wal", wal_base);
    selftest_unlink(db_path);
    selftest_unlink(wal_file);

    int w_fd = selftest_open_file(db_path);
    ST_CHECK(w_fd >= 0);

    BufferPool wal_bp;
    buffer_pool_init(&wal_bp);
    ST_CHECK(wal_init(wal_base));

    const char* delta_data = "COMMITTED_DELTA_PAYLOAD";
    ST_CHECK(wal_write_page_delta(0, 100, delta_data, (uint16_t)strlen(delta_data)));
    ST_CHECK(wal_commit_tx(101));
    wal_shutdown();

    ST_CHECK(wal_recover_and_replay(w_fd, &wal_bp));
    uint8_t* recovered_p = buffer_pool_fetch(&wal_bp, w_fd, 0, false);
    ST_CHECK(recovered_p != NULL);
    ST_CHECK(!memcmp(recovered_p + 100, delta_data, strlen(delta_data)));
    buffer_pool_unpin(&wal_bp, 0, false);

    ST_CHECK(wal_init(wal_base));
    ST_CHECK(wal_checkpoint_now(&wal_bp, w_fd));
    wal_shutdown();
    buffer_pool_destroy(&wal_bp, w_fd);
    selftest_close_file(w_fd);
    selftest_unlink(db_path);
    selftest_unlink(wal_file);

    /* Phase 4: Persistent On-Disk B+ Tree Index */
    const char* btree_file = "selftest_tmp/test_btree.db";
    selftest_unlink(btree_file);
    int b_fd = selftest_open_file(btree_file);
    ST_CHECK(b_fd >= 0);

    BufferPool btree_bp;
    buffer_pool_init(&btree_bp);
    uint32_t root_page = btree_create(&btree_bp, b_fd, TYPE_NUMBER);
    ST_CHECK(root_page != UINT32_MAX);

    for (int i = 1; i <= 60; i++) {
        Value k;
        snprintf(k.text, sizeof(k.text), "%03d", i);
        k.is_null = false;
        ST_CHECK(btree_insert(&btree_bp, b_fd, &root_page, TYPE_NUMBER, &k, (uint32_t)(i * 10), i));
    }

    Value query_key;
    snprintf(query_key.text, sizeof(query_key.text), "035");
    query_key.is_null = false;
    uint32_t found_page = 0;
    int found_slot = 0;
    ST_CHECK(btree_search(&btree_bp, b_fd, root_page, TYPE_NUMBER, &query_key, &found_page, &found_slot));
    ST_CHECK(found_page == 350 && found_slot == 35);

    Value low, high;
    snprintf(low.text, sizeof(low.text), "020");
    snprintf(high.text, sizeof(high.text), "025");
    low.is_null = high.is_null = false;
    uint32_t out_pages[10];
    int out_slots[10];
    int matches = btree_range_scan(&btree_bp, b_fd, root_page, TYPE_NUMBER, &low, &high, out_pages, out_slots, 10);
    ST_CHECK(matches == 6 && out_slots[0] == 20 && out_slots[5] == 25);

    buffer_pool_destroy(&btree_bp, b_fd);
    selftest_close_file(b_fd);
    selftest_unlink(btree_file);

    /* Phase 3: Dynamic Variable-Length Overflow Pages & Scaled Buffer Pool */
    const char* ovf_file = "selftest_tmp/test_overflow.dat";
    selftest_unlink(ovf_file);
    int ovf_fd = selftest_open_file(ovf_file);
    ST_CHECK(ovf_fd >= 0);

    BufferPool ovf_bp;
    buffer_pool_init(&ovf_bp);

    /* Test 1: Write and read multi-page overflow payload (10KB spanning 3 pages) */
    #define OVF_PAYLOAD_SIZE 10000
    uint8_t* send_buf = (uint8_t*)malloc(OVF_PAYLOAD_SIZE);
    uint8_t* recv_buf = (uint8_t*)malloc(OVF_PAYLOAD_SIZE + 64);
    ST_CHECK(send_buf != NULL && recv_buf != NULL);
    for (size_t i = 0; i < OVF_PAYLOAD_SIZE; i++) {
        send_buf[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }
    uint32_t first_ovf_page = UINT32_MAX;
    ST_CHECK(slotted_overflow_write(&ovf_bp, ovf_fd, send_buf, OVF_PAYLOAD_SIZE, &first_ovf_page));
    ST_CHECK(first_ovf_page != UINT32_MAX);

    size_t bytes_read = 0;
    ST_CHECK(slotted_overflow_read(&ovf_bp, ovf_fd, first_ovf_page, recv_buf, OVF_PAYLOAD_SIZE + 64, &bytes_read));
    ST_CHECK(bytes_read == OVF_PAYLOAD_SIZE);
    ST_CHECK(memcmp(send_buf, recv_buf, OVF_PAYLOAD_SIZE) == 0);

    /* Test 2: Scaled Buffer Pool Clock Eviction and Hash Table Re-fetch across 280 pages */
    uint32_t start_pid = ovf_bp.page_count;
    for (int p = 0; p < 280; p++) {
        uint32_t pid = buffer_pool_alloc_page(&ovf_bp, ovf_fd, PAGE_TYPE_DATA);
        uint8_t* pmem = buffer_pool_fetch(&ovf_bp, ovf_fd, pid, true);
        ST_CHECK(pmem != NULL);
        pmem[0] = (uint8_t)(pid & 0xFF);
        pmem[1] = (uint8_t)((pid >> 8) & 0xFF);
        buffer_pool_unpin(&ovf_bp, pid, true);
    }
    ST_CHECK(buffer_pool_flush_all(&ovf_bp, ovf_fd));

    /* Verify random-access re-fetch across evicted frames via hash table */
    for (int p = 0; p < 280; p += 17) {
        uint32_t pid = start_pid + (uint32_t)p;
        uint8_t* pmem = buffer_pool_fetch(&ovf_bp, ovf_fd, pid, false);
        ST_CHECK(pmem != NULL);
        ST_CHECK(pmem[0] == (uint8_t)(pid & 0xFF));
        ST_CHECK(pmem[1] == (uint8_t)((pid >> 8) & 0xFF));
        buffer_pool_unpin(&ovf_bp, pid, false);
    }

    free(send_buf);
    free(recv_buf);
    #undef OVF_PAYLOAD_SIZE
    buffer_pool_destroy(&ovf_bp, ovf_fd);
    selftest_close_file(ovf_fd);
    selftest_unlink(ovf_file);
    return true;
}

/* =========================================================================
 * Suite 9: Automated Fuzz & Grammar Stress (from test_fuzz.c)
 * ========================================================================= */
static uint32_t g_st_fuzz_seed = 133742;
static uint32_t st_fuzz_rand(void) {
    g_st_fuzz_seed = g_st_fuzz_seed * 1664525u + 1013904223u;
    return g_st_fuzz_seed;
}

static bool run_suite_fuzz(void) {
    selftest_mkdir("selftest_tmp");
    const char* db = "selftest_tmp/fuzz_test.pcod";
    selftest_remove_db_artifacts(db);

    ST_CHECK(opendata_open(db, NULL, 0, NULL));
    g_st_fuzz_seed = 133742;

    static const char* const keywords[] = {
        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
        "DELETE", "CREATE", "TABLE", "DROP", "ALTER", "ADD", "RENAME", "GROUP",
        "BY", "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "JOIN", "ON", "INNER",
        "LEFT", "BEGIN", "COMMIT", "ROLLBACK", "BACKUP", "STATUS", "EXPLAIN",
        "ROW_NUMBER()", "RANK()", "OVER()", "EXISTS", "NOT", "AND", "OR",
        "PRIMARY", "KEY", "REFERENCES", "NUMBER", "VARCHAR2", "DATE"
    };
    int kw_count = (int)(sizeof(keywords) / sizeof(keywords[0]));

    for (int i = 0; i < 300; i++) {
        char buf[512];
        size_t pos = 0;
        int parts = (int)(st_fuzz_rand() % 8) + 1;
        for (int p = 0; p < parts && pos + 64 < sizeof(buf); p++) {
            uint32_t choice = st_fuzz_rand() % 5;
            if (choice == 0) {
                const char* kw = keywords[st_fuzz_rand() % (uint32_t)kw_count];
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s ", kw);
            } else if (choice == 1) {
                static const char punct[] = "(),;.*<>=!+-/%'\"";
                buf[pos++] = punct[st_fuzz_rand() % (sizeof(punct) - 1)];
                buf[pos++] = ' ';
            } else if (choice == 2) {
                if (st_fuzz_rand() % 2 == 0)
                    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%u ", st_fuzz_rand() % 100000);
                else
                    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "col_%u ", st_fuzz_rand() % 50);
            } else if (choice == 3) {
                int glen = (int)(st_fuzz_rand() % 16) + 1;
                for (int g = 0; g < glen && pos + 1 < sizeof(buf); g++)
                    buf[pos++] = (char)(1 + (st_fuzz_rand() % 126));
                buf[pos++] = ' ';
            } else {
                buf[pos++] = '\'';
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "unclosed_%u ", st_fuzz_rand() % 100);
            }
        }
        buf[pos] = '\0';
        (void)opendata_exec(buf);
    }
    g_st_assertions += 300;

    /* Deep nesting */
    char nested[4096];
    size_t pos = (size_t)snprintf(nested, sizeof(nested), "SELECT ");
    for (int i = 0; i < 200 && pos + 4 < sizeof(nested); i++) nested[pos++] = '(';
    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, "1");
    for (int i = 0; i < 200 && pos + 4 < sizeof(nested); i++) nested[pos++] = ')';
    pos += (size_t)snprintf(nested + pos, sizeof(nested) - pos, " FROM DUAL;");
    (void)opendata_exec(nested);

    /* Numeric overflows */
    static const char* const overflows[] = {
        "SELECT 999999999999999999999999999999999999999999999999999999999 FROM DUAL;",
        "SELECT -99999999999999999999999999999999999999999999999999999999 FROM DUAL;",
        "SELECT 1.2.3.4.5 FROM DUAL;",
        "SELECT 1e999999999999999999999 FROM DUAL;",
        "SELECT 1/0 FROM DUAL;",
        "SELECT '2026-99-99' FROM DUAL;",
        "SELECT '1900-02-29' FROM DUAL;"
    };
    for (size_t i = 0; i < sizeof(overflows) / sizeof(overflows[0]); i++) {
        (void)opendata_exec(overflows[i]);
    }

    /* Malformed JSON */
    char out[128];
    static const char* const malformed_json[] = {
        "", "{", "}", "[", "]", "{\"a\":}", "{\"a\":", "{\"a\":[1,2,}",
        "{\"a\":\"unclosed", "{\"a\": \xFF\xFE\xFD}", "{\"k\": 1e9999999999}"
    };
    for (size_t i = 0; i < sizeof(malformed_json) / sizeof(malformed_json[0]); i++) {
        (void)opendata_extract_json(malformed_json[i], "a", out, sizeof(out));
    }

    /* State machine chaos */
    static const char* const ops[] = {
        "BEGIN;", "COMMIT;", "ROLLBACK;",
        "CREATE TABLE chaos_t (id NUMBER PRIMARY KEY, v VARCHAR2(20));",
        "INSERT INTO chaos_t VALUES (1, 'val');",
        "INSERT INTO chaos_t VALUES (1, 'dup');",
        "UPDATE chaos_t SET v='updated' WHERE id=1;",
        "DELETE FROM chaos_t WHERE id=1;",
        "DROP TABLE chaos_t;",
        "SELECT * FROM chaos_t;"
    };
    int op_count = (int)(sizeof(ops) / sizeof(ops[0]));
    bool saved_confirm = g_confirm_destructive;
    g_confirm_destructive = false;
    for (int i = 0; i < 150; i++) {
        (void)opendata_exec(ops[st_fuzz_rand() % (uint32_t)op_count]);
    }
    g_confirm_destructive = saved_confirm;
    (void)opendata_exec("ROLLBACK;");
    g_st_assertions += 150;

    opendata_close();
    selftest_remove_db_artifacts(db);
    return true;
}

/* =========================================================================
 * Suite 10: Enterprise Architecture: TDE, PITR, CBO, MVCC, Partitions
 *           (from test_enterprise_c.c)
 * ========================================================================= */
static bool run_suite_enterprise(void) {
    selftest_mkdir("selftest_tmp");

    /* 1. Transparent Data Encryption (TDE) AES-256 */
    const char* test_tde_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    ST_CHECK(opendata_set_tde_key(test_tde_key));
    ST_CHECK(g_tde_enabled);

    Database* db = calloc(1, sizeof(Database));
    ST_CHECK(db != NULL);
    db->table_count = 1;
    snprintf(db->tables[0].name, sizeof(db->tables[0].name), "CUSTOMERS");
    db->tables[0].col_count = 2;
    snprintf(db->tables[0].columns[0].name, sizeof(db->tables[0].columns[0].name), "ID");
    db->tables[0].columns[0].type = TYPE_NUMBER;
    db->tables[0].columns[0].size = 40;
    snprintf(db->tables[0].columns[1].name, sizeof(db->tables[0].columns[1].name), "EMAIL");
    db->tables[0].columns[1].type = TYPE_VARCHAR2;
    db->tables[0].columns[1].size = 100;

    Image image = {0};
    ST_CHECK(encode_database_snapshot(db, 1001, 1, &image));
    ST_CHECK(image.size >= 28 && memcmp(image.data, "PCODEN01", 8) == 0);

    uint64_t identity = 0, generation = 0;
    Database* decoded = decode_database_snapshot(&image, &identity, &generation);
    ST_CHECK(decoded != NULL && identity == 1001 && generation == 1);
    ST_CHECK(decoded->table_count == 1 && strcmp(decoded->tables[0].name, "CUSTOMERS") == 0);
    destroy_database(decoded);
    free(image.data);

    memset(&image, 0, sizeof(image));
    ST_CHECK(encode_database_snapshot(db, 2002, 2, &image));
    opendata_set_tde_key("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    decoded = decode_database_snapshot(&image, &identity, &generation);
    ST_CHECK(decoded == NULL);
    free(image.data);
    free(db);

    opendata_set_tde_key(NULL);
    ST_CHECK(!g_tde_enabled);

    /* 2. Point-In-Time Recovery (PITR) WAL Replay */
    const char* pitr_base = "selftest_tmp/pitr_db";
    char pitr_dat[256], pitr_wal[256];
    snprintf(pitr_dat, sizeof(pitr_dat), "%s.dat", pitr_base);
    snprintf(pitr_wal, sizeof(pitr_wal), "%s.wal", pitr_base);
    selftest_unlink(pitr_dat);
    selftest_unlink(pitr_wal);

    int p_fd = selftest_open_file(pitr_dat);
    ST_CHECK(p_fd >= 0);

    BufferPool bp;
    buffer_pool_init(&bp);
    ST_CHECK(wal_init(pitr_base));

    uint32_t t1 = (uint32_t)time(NULL);
    const char* payload1 = "DELTA_BEFORE_CUTOFF";
    ST_CHECK(wal_write_page_delta(0, 50, payload1, (uint16_t)strlen(payload1)));
    ST_CHECK(wal_commit_tx(501));
    wal_shutdown();

    BufferPool bp_old;
    buffer_pool_init(&bp_old);
    ST_CHECK(wal_recover_and_replay_to_timestamp(p_fd, &bp_old, t1 - 10));
    uint8_t* p_old = buffer_pool_fetch(&bp_old, p_fd, 0, false);
    ST_CHECK(p_old != NULL && memcmp(p_old + 50, payload1, strlen(payload1)) != 0);
    buffer_pool_unpin(&bp_old, 0, false);
    buffer_pool_destroy(&bp_old, p_fd);

    ST_CHECK(wal_recover_and_replay_to_timestamp(p_fd, &bp, t1 + 5));
    uint8_t* p_mem = buffer_pool_fetch(&bp, p_fd, 0, false);
    ST_CHECK(p_mem != NULL && memcmp(p_mem + 50, payload1, strlen(payload1)) == 0);
    buffer_pool_unpin(&bp, 0, false);
    buffer_pool_destroy(&bp, p_fd);
    selftest_close_file(p_fd);
    selftest_unlink(pitr_dat);
    selftest_unlink(pitr_wal);

    /* 3. Raft Dynamic Membership & Term Tracking */
    ST_CHECK(raft_get_current_term() >= 1);
    ST_CHECK(raft_add_peer("2@127.0.0.1:9002"));
    ST_CHECK(raft_remove_peer(2));

    /* 4. Session Registry & Observability */
    int s1 = register_client_session("192.168.1.100");
    ST_CHECK(s1 > 0 && get_active_session_count() >= 1);
    unregister_client_session(s1);

    /* 5. Multi-Version Garbage Collection (MVCC Vacuum) */
    Database* vdb = calloc(1, sizeof(Database));
    ST_CHECK(vdb != NULL);
    vdb->table_count = 1;
    Table* vtbl = &vdb->tables[0];
    snprintf(vtbl->name, sizeof(vtbl->name), "CHURN_TBL");
    vtbl->col_count = 1;
    snprintf(vtbl->columns[0].name, sizeof(vtbl->columns[0].name), "ID");
    vtbl->columns[0].type = TYPE_NUMBER;
    vtbl->columns[0].size = 40;
    ST_CHECK(allocate_table_storage(vtbl, 256) && vtbl->capacity == 256);
    vtbl->row_count = 10;
    vtbl->dead_row_count = 150;
    size_t freed_bytes = 0;
    int reclaimed = 0;
    ST_CHECK(vacuum_table_storage(vdb, 0, true, &freed_bytes, &reclaimed));
    ST_CHECK(reclaimed == 150 && vtbl->dead_row_count == 0 && vtbl->capacity == 10);
    destroy_database(vdb);

    /* 6. Cost-Based Optimizer (CBO) & Equi-Width Histograms */
    Database* cdb = calloc(1, sizeof(Database));
    ST_CHECK(cdb != NULL);
    g_db = cdb;
    g_readonly = false;
    cdb->table_count = 1;
    Table* ctbl = &cdb->tables[0];
    snprintf(ctbl->name, sizeof(ctbl->name), "ORDERS");
    ctbl->col_count = 1;
    snprintf(ctbl->columns[0].name, sizeof(ctbl->columns[0].name), "AMOUNT");
    ctbl->columns[0].type = TYPE_NUMBER;
    ctbl->columns[0].size = 40;
    ST_CHECK(allocate_table_storage(ctbl, 200));
    for (int i = 0; i < 100; i++) {
        snprintf(ctbl->rows[i].values[0].text, sizeof(ctbl->rows[i].values[0].text), "%d", i + 1);
        ctbl->rows[i].values[0].is_null = false;
    }
    ctbl->row_count = 100;
    ST_CHECK(execute_create_index_in_db(cdb, "IDX_ORDERS_AMT", "ORDERS", "AMOUNT", false));
    ST_CHECK(execute_analyze_table("ORDERS") && ctbl->stats.is_analyzed);
    ST_CHECK(ctbl->stats.col_stats[0].distinct_count == 100);

    Filter* sel_filter = calloc(1, sizeof(Filter));
    ST_CHECK(sel_filter != NULL);
    sel_filter->node_count = 1;
    sel_filter->root = 0;
    sel_filter->nodes[0].type = FILTER_NODE_PRED;
    sel_filter->nodes[0].pred.left = (Binding){0, 0};
    sel_filter->nodes[0].pred.op = TK_LT;
    snprintf(sel_filter->nodes[0].pred.value.text, sizeof(sel_filter->nodes[0].pred.value.text), "10");
    sel_filter->nodes[0].pred.value.is_null = false;

    bool use_idx = false;
    int idx_id = -1, est_rows = 0;
    double cost = cbo_estimate_query_cost(cdb, ctbl, 0, sel_filter, &use_idx, &idx_id, &est_rows);
    ST_CHECK(use_idx && idx_id == 0 && est_rows >= 5 && est_rows <= 15 && cost > 0.0 && cost < 100.0);
    free(sel_filter);
    g_db = NULL;
    destroy_database(cdb);

    /* 7. Table Partitioning & Columnar Spillover */
    Database* pdb = calloc(1, sizeof(Database));
    ST_CHECK(pdb != NULL);
    pdb->table_count = 1;
    Table* ptbl = &pdb->tables[0];
    snprintf(ptbl->name, sizeof(ptbl->name), "METRICS_TS");
    ptbl->col_count = 2;
    snprintf(ptbl->columns[0].name, sizeof(ptbl->columns[0].name), "TS_ID");
    ptbl->columns[0].type = TYPE_NUMBER;
    ptbl->columns[0].size = 40;
    snprintf(ptbl->columns[1].name, sizeof(ptbl->columns[1].name), "VAL");
    ptbl->columns[1].type = TYPE_NUMBER;
    ptbl->columns[1].size = 40;
    ptbl->engine_type = ENGINE_COLUMNAR;
    ptbl->part_type = PART_RANGE;
    ptbl->part_col_idx = 0;
    ptbl->part_count = 3;
    snprintf(ptbl->partitions[0].name, sizeof(ptbl->partitions[0].name), "P1");
    snprintf(ptbl->partitions[0].upper_bound, sizeof(ptbl->partitions[0].upper_bound), "100");
    snprintf(ptbl->partitions[1].name, sizeof(ptbl->partitions[1].name), "P2");
    snprintf(ptbl->partitions[1].upper_bound, sizeof(ptbl->partitions[1].upper_bound), "200");
    snprintf(ptbl->partitions[2].name, sizeof(ptbl->partitions[2].name), "P3");
    ptbl->partitions[2].is_maxvalue = true;

    ST_CHECK(allocate_table_storage(ptbl, 16));
    int sample_ids[3] = {50, 150, 250};
    int sample_vals[3] = {10, 20, 30};
    for (int i = 0; i < 3; i++) {
        snprintf(ptbl->rows[i].values[0].text, sizeof(ptbl->rows[i].values[0].text), "%d", sample_ids[i]);
        ptbl->rows[i].values[0].is_null = false;
        snprintf(ptbl->rows[i].values[1].text, sizeof(ptbl->rows[i].values[1].text), "%d", sample_vals[i]);
        ptbl->rows[i].values[1].is_null = false;
        ST_CHECK(get_row_partition(ptbl, &ptbl->rows[i]) == i);
    }
    ptbl->row_count = 3;

    Filter* p_filter = calloc(1, sizeof(Filter));
    ST_CHECK(p_filter != NULL);
    p_filter->node_count = 1;
    p_filter->root = 0;
    p_filter->nodes[0].type = FILTER_NODE_PRED;
    p_filter->nodes[0].pred.left = (Binding){0, 0};
    p_filter->nodes[0].pred.op = TK_LT;
    snprintf(p_filter->nodes[0].pred.value.text, sizeof(p_filter->nodes[0].pred.value.text), "100");
    p_filter->nodes[0].pred.value.is_null = false;

    uint32_t mask = prune_partitions_for_filter(ptbl, 0, p_filter);
    ST_CHECK(mask == 1u);

    long double v_sum = 0, v_min = 0, v_max = 0;
    int v_cnt = 0;
    vector_aggregate_numeric_partitioned(ptbl, 1, p_filter, mask, &v_sum, &v_min, &v_max, &v_cnt);
    ST_CHECK(v_cnt == 1 && v_sum == 10.0L);
    free(p_filter);
    destroy_database(pdb);

    return true;
}

/* =========================================================================
 * Suite 11: HNSW Vector, Window/JSON, Directory Auth, Oracle & MSSQL
 *           (from test_new_features.c)
 * ========================================================================= */
static bool run_suite_new_features(void) {
    selftest_mkdir("selftest_tmp");
    Database* db = calloc(1, sizeof(Database));
    ST_CHECK(db != NULL);

    /* 1. Window Functions */
    int tidx = db->table_count++;
    Table* tbl = &db->tables[tidx];
    snprintf(tbl->name, sizeof(tbl->name), "EMP");
    tbl->col_count = 3;
    snprintf(tbl->columns[0].name, sizeof(tbl->columns[0].name), "DEPT");
    tbl->columns[0].type = TYPE_VARCHAR2; tbl->columns[0].size = 20;
    snprintf(tbl->columns[1].name, sizeof(tbl->columns[1].name), "NAME");
    tbl->columns[1].type = TYPE_VARCHAR2; tbl->columns[1].size = 20;
    snprintf(tbl->columns[2].name, sizeof(tbl->columns[2].name), "SALARY");
    tbl->columns[2].type = TYPE_NUMBER;

    const char* depts[5] = {"HR", "HR", "HR", "IT", "IT"};
    const char* names[5] = {"Alice", "Bob", "Charlie", "Dave", "Eve"};
    const char* sals[5]  = {"5000", "5000", "6000", "7000", "8000"};
    for (int r = 0; r < 5; r++) {
        reserve_insert_capacity(tbl);
        snprintf(tbl->rows[r].values[0].text, sizeof(tbl->rows[r].values[0].text), "%s", depts[r]);
        snprintf(tbl->rows[r].values[1].text, sizeof(tbl->rows[r].values[1].text), "%s", names[r]);
        snprintf(tbl->rows[r].values[2].text, sizeof(tbl->rows[r].values[2].text), "%s", sals[r]);
        tbl->row_count++;
    }

    g_db = db;
    g_committed = db;

    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;
    bool quit = false;
    ST_CHECK(execute_console_statement(
        "SELECT dept, name, salary, "
        "ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary ASC) AS rn, "
        "RANK() OVER (PARTITION BY dept ORDER BY salary ASC) AS rk, "
        "DENSE_RANK() OVER (PARTITION BY dept ORDER BY salary ASC) AS dr "
        "FROM emp;", &quit));
    ST_CHECK(cap.row_count == 5);
    free(cap.rows);
    g_result_capture = NULL;

    /* 2. JSON Path Extraction (->, ->>, JSON_VALUE) */
    char out_val[128];
    const char* sample_json = "{\"user\": \"alice\", \"age\": 30, \"active\": true}";
    ST_CHECK(extract_json_value(sample_json, "user", out_val, sizeof(out_val)) && !strcmp(out_val, "alice"));
    ST_CHECK(extract_json_value(sample_json, "$.age", out_val, sizeof(out_val)) && !strcmp(out_val, "30"));

    int tj_idx = db->table_count++;
    Table* tj = &db->tables[tj_idx];
    snprintf(tj->name, sizeof(tj->name), "DOCS");
    tj->col_count = 2;
    snprintf(tj->columns[0].name, sizeof(tj->columns[0].name), "ID");
    tj->columns[0].type = TYPE_NUMBER;
    snprintf(tj->columns[1].name, sizeof(tj->columns[1].name), "DATA");
    tj->columns[1].type = TYPE_VARCHAR2; tj->columns[1].size = 200;
    reserve_insert_capacity(tj);
    snprintf(tj->rows[0].values[0].text, sizeof(tj->rows[0].values[0].text), "1");
    snprintf(tj->rows[0].values[1].text, sizeof(tj->rows[0].values[1].text), "{\"product\": \"OpenData\", \"price\": 99.95}");
    tj->row_count = 1;

    memset(&cap, 0, sizeof(cap));
    cap.silent = true;
    g_result_capture = &cap;
    ST_CHECK(execute_console_statement("SELECT id, data->'product', data->>'price', JSON_VALUE(data, 'product') FROM docs;", &quit));
    ST_CHECK(cap.row_count == 1 && !strcmp(cap.rows[0][1].text, "OpenData") && !strcmp(cap.rows[0][2].text, "99.95"));
    free(cap.rows);
    g_result_capture = NULL;

    /* 3. HNSW Vector Indexing */
    int tv_idx = db->table_count++;
    Table* tv = &db->tables[tv_idx];
    snprintf(tv->name, sizeof(tv->name), "VEC_ITEMS");
    tv->col_count = 2;
    snprintf(tv->columns[0].name, sizeof(tv->columns[0].name), "ID");
    tv->columns[0].type = TYPE_NUMBER;
    snprintf(tv->columns[1].name, sizeof(tv->columns[1].name), "EMBEDDING");
    tv->columns[1].type = TYPE_VECTOR; tv->columns[1].size = 4;

    const char* vecs[] = {
        "[1.0, 0.0, 0.0, 0.0]",
        "[0.9, 0.1, 0.0, 0.0]",
        "[0.0, 1.0, 0.0, 0.0]",
        "[0.0, 0.0, 1.0, 0.0]"
    };
    for (int r = 0; r < 4; r++) {
        reserve_insert_capacity(tv);
        snprintf(tv->rows[r].values[0].text, sizeof(tv->rows[r].values[0].text), "%d", r + 1);
        snprintf(tv->rows[r].values[1].text, sizeof(tv->rows[r].values[1].text), "%s", vecs[r]);
        tv->row_count++;
    }

    ST_CHECK(execute_create_hnsw_index_in_db(db, "idx_hnsw_test", "VEC_ITEMS", "EMBEDDING", 0));
    ST_CHECK(db->index_count == 1 && db->indexes[0].is_hnsw);

    float q_vec[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    int out_r[2] = { -1, -1 };
    float out_d[2] = { 0.0f, 0.0f };
    int hits = hnsw_index_search(&db->indexes[0], q_vec, 4, 2, out_r, out_d);
    ST_CHECK(hits == 2 && out_r[0] == 0 && out_r[1] == 1);

    /* 4. Continuous WAL Archiving */
    selftest_unlink("selftest_tmp/wal_pitr.wal");
    ST_CHECK(wal_init("selftest_tmp/wal_pitr"));
    char dummy_data[64] = "wal payload test";
    ST_CHECK(wal_write_page_delta(1, 0, dummy_data, 16));
    ST_CHECK(wal_commit_tx(100));
    selftest_mkdir("selftest_tmp/wal_archive");
    ST_CHECK(wal_archive_segment("selftest_tmp/wal_archive"));
    wal_shutdown();
    selftest_unlink("selftest_tmp/wal_pitr.wal");

    /* 5. Tokenizer & Operator Verification */
    Parser p = { .cursor = "-> ->> <=> <->" };
    scan_next_token(&p); ST_CHECK(p.kind == TK_JSON_ARROW);
    scan_next_token(&p); ST_CHECK(p.kind == TK_JSON_TEXT_ARROW);
    scan_next_token(&p); ST_CHECK(p.kind == TK_COSINE_DIST);
    scan_next_token(&p); ST_CHECK(p.kind == TK_L2_DIST);

    /* 6. LDAP & Active Directory & Kerberos Configuration */
    configure_directory_ad(NULL, NULL, NULL);
    configure_directory_ldap(NULL, 0, false, NULL, NULL, NULL);
    ST_CHECK(!is_directory_auth_enabled());
    configure_directory_ad("dc01.corp.local", "corp.local", "Domain Admins");
    ST_CHECK(is_directory_auth_enabled());
    configure_directory_ldap("ldap.company.com", 636, true, "dc=company,dc=com", "uid", "admins");
    ST_CHECK(is_directory_auth_enabled());
    UserRole dummy_role = USER_ROLE_READ_ONLY;
    ST_CHECK(!authenticate_directory_user(NULL, NULL, &dummy_role));
    ST_CHECK(!authenticate_directory_user("", "", &dummy_role));
    configure_directory_ad(NULL, NULL, NULL);
    configure_directory_ldap(NULL, 0, false, NULL, NULL, NULL);

    /* 7. Security & Robustness */
    char json_buf[64];
    ST_CHECK(extract_json_value("{\"key\": \"val\"}", "key", json_buf, sizeof(json_buf)) && !strcmp(json_buf, "val"));
    ST_CHECK(!extract_json_value("{\"key\"   ", "key", json_buf, sizeof(json_buf)));
    g_network_user_role = USER_ROLE_ADMIN;
    ST_CHECK(user_has_table_privilege(db, "ad_domain_admin", "EMP", PRIV_DELETE));
    g_network_user_role = USER_ROLE_READ_ONLY;
    ST_CHECK(!user_has_table_privilege(db, "ad_read_only_user", "EMP", PRIV_DELETE));
    g_network_user_role = 0;

    /* 8. Statement Timeout, SQ8, Auto-Vacuum, Kerberos */
    g_statement_timeout_ms = 50;
    g_query_start_time_ms = (uint64_t)(monotonic_seconds() * 1000.0) - 100;
    ST_CHECK(is_query_timed_out());
    g_statement_timeout_ms = 0;
    g_query_start_time_ms = 0;
    ST_CHECK(!is_query_timed_out());

    float v1[4] = { 1.0f, 0.0f, 0.5f, -0.5f };
    float v2[4] = { 0.9f, 0.1f, 0.4f, -0.4f };
    VectorSQ8 sq1, sq2;
    ST_CHECK(vector_quantize_sq8(v1, 4, &sq1) && vector_quantize_sq8(v2, 4, &sq2));
    ST_CHECK(vector_sq8_cosine_distance(&sq1, &sq2) < 0.1);
    ST_CHECK(vector_sq8_l2_distance(&sq1, &sq2) < 0.5);

    tbl->dead_row_count = 20;
    ST_CHECK(run_autovacuum_sweep(db) >= 0);

    configure_kerberos(NULL, NULL, NULL);
    ST_CHECK(!is_kerberos_enabled());
    configure_kerberos("opendata", "EXAMPLE.COM", "/etc/krb5.keytab");
    ST_CHECK(is_kerberos_enabled());
    unsigned char dummy_ticket[16] = { 0x60, 0x0e, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x02 };
    char krb_user[128] = {0};
    UserRole krb_role = USER_ROLE_ADMIN;
    ST_CHECK(authenticate_kerberos_ticket(dummy_ticket, sizeof(dummy_ticket), NULL, NULL, krb_user, sizeof(krb_user), &krb_role));
    ST_CHECK(strstr(krb_user, "@EXAMPLE.COM") != NULL);
    configure_kerberos(NULL, NULL, NULL);

    /* 9. Oracle & MSSQL Syntax Compatibility */
    Parser p_ms_create = { .cursor = "CREATE TABLE [dbo].[mssql_test] ([id] INT IDENTITY(1,1) PRIMARY KEY, [guid] UNIQUEIDENTIFIER, [bin] VARBINARY(MAX), [doc] XML);" };
    scan_next_token(&p_ms_create);
    consume_keyword(&p_ms_create, "CREATE");
    ST_CHECK(execute_create_table(&p_ms_create, db));

    int ins_cnt = 0;
    Parser p_ms_ins1 = { .cursor = "INSERT INTO [dbo].[mssql_test] VALUES (1, 'A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11', 'FF00AA', '<root/>');" };
    scan_next_token(&p_ms_ins1);
    consume_keyword(&p_ms_ins1, "INSERT");
    ST_CHECK(execute_insert_row(&p_ms_ins1, db, &ins_cnt) && ins_cnt == 1);

    QueryResultCapture cap_ms = { .silent = true };
    g_result_capture = &cap_ms;
    Parser p_ms_sel = { .cursor = "SELECT TOP 1 [id], ISNULL([guid], 'NONE') FROM [dbo].[mssql_test];" };
    scan_next_token(&p_ms_sel);
    consume_keyword(&p_ms_sel, "SELECT");
    ST_CHECK(execute_select_query(&p_ms_sel) && cap_ms.row_count == 1);
    free(cap_ms.rows);

    Parser p_ora_create = { .cursor = "CREATE TABLE hr.ora_test (id NUMBER PRIMARY KEY, rid ROWID, name VARCHAR2(50 BYTE), val NUMBER);" };
    scan_next_token(&p_ora_create);
    consume_keyword(&p_ora_create, "CREATE");
    ST_CHECK(execute_create_table(&p_ora_create, db));

    Parser p_ora_ins1 = { .cursor = "INSERT INTO hr.ora_test VALUES (1, 'AAARiSAABAAALpkAAA', 'Alpha', 100);" };
    scan_next_token(&p_ora_ins1);
    consume_keyword(&p_ora_ins1, "INSERT");
    ST_CHECK(execute_insert_row(&p_ora_ins1, db, &ins_cnt) && ins_cnt == 1);

    QueryResultCapture cap_ora = { .silent = true };
    g_result_capture = &cap_ora;
    Parser p_ora_sel = { .cursor = "SELECT id, NVL2(rid, 'HAS_ROWID', 'NO_ROWID'), TO_CHAR(name) FROM hr.ora_test WHERE ROWNUM <= 1;" };
    scan_next_token(&p_ora_sel);
    consume_keyword(&p_ora_sel, "SELECT");
    ST_CHECK(execute_select_query(&p_ora_sel) && cap_ora.row_count == 1);
    ST_CHECK(!strcmp(cap_ora.rows[0][1].text, "HAS_ROWID") && !strcmp(cap_ora.rows[0][2].text, "Alpha"));
    free(cap_ora.rows);
    g_result_capture = NULL;

    g_db = NULL;
    g_committed = NULL;
    destroy_database(db);
    return true;
}

/* =========================================================================
 * Suite 12: Cluster Quorum, Multi-Instance, IRIS Editor, Wire Protocols,
 *           and MSSQL/Oracle Binary Files (from test_cluster_quorum.c)
 * ========================================================================= */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t msg_type;
    uint32_t term;
    uint32_t node_id;
    uint32_t payload_len;
} SelfTestRaftHdr;
#pragma pack(pop)

typedef struct {
    int port;
    int node_id;
    atomic_bool running;
    atomic_bool acknowledge;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
} SelfTestFollowerPeer;

#ifdef _WIN32
typedef int st_socklen_t;
#else
typedef size_t st_socklen_t;
#endif

static bool st_mock_recv_all(selftest_socket_t s, void* buf, size_t len) {
    char* ptr = (char*)buf;
    size_t rec = 0;
    while (rec < len) {
        int n = (int)recv(s, ptr + rec, (st_socklen_t)(len - rec), 0);
        if (n <= 0) return false;
        rec += (size_t)n;
    }
    return true;
}

#ifdef _WIN32
static DWORD WINAPI st_mock_follower_worker(LPVOID arg) {
#else
static void* st_mock_follower_worker(void* arg) {
#endif
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SelfTestFollowerPeer* peer = (SelfTestFollowerPeer*)arg;
    selftest_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)peer->port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, (struct sockaddr*)&sin, sizeof(sin));
    listen(listener, 8);

    while (atomic_load(&peer->running)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        struct timeval tv = {0, 50000};
        int sel = select((int)listener + 1, &fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(listener, &fds)) {
            selftest_socket_t s = accept(listener, NULL, NULL);
            if (s != SELFTEST_INVALID_SOCKET) {
                SelfTestRaftHdr hdr;
                if (st_mock_recv_all(s, &hdr, sizeof(hdr)) && hdr.magic == 0x50434F44 && hdr.msg_type == 1) {
                    char* payload = malloc(hdr.payload_len);
                    if (payload) {
                        bool received = st_mock_recv_all(s, payload, hdr.payload_len);
                        free(payload);
                        if (received && atomic_load(&peer->acknowledge)) {
                            SelfTestRaftHdr ack = { 0x50434F44, 2, hdr.term, (uint32_t)peer->node_id, 0 };
                            send(s, (const char*)&ack, sizeof(ack), 0);
                        }
                    }
                }
#ifdef _WIN32
                shutdown(s, SD_SEND);
                char dummy[8];
                recv(s, dummy, sizeof(dummy), 0);
#endif
                SELFTEST_CLOSE_SOCK(s);
            }
        }
    }
    SELFTEST_CLOSE_SOCK(listener);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI st_async_server_thread(LPVOID arg) {
#else
static void* st_async_server_thread(void* arg) {
#endif
    ServerConfig* cfg = (ServerConfig*)arg;
    run_nonblocking_wire_server(cfg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void st_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static bool selftest_can_connect_loopback(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    selftest_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == SELFTEST_INVALID_SOCKET) return false;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(srv, 1) != 0) {
        SELFTEST_CLOSE_SOCK(srv);
        return false;
    }
#ifdef _WIN32
    int alen = (int)sizeof(addr);
#else
    socklen_t alen = (socklen_t)sizeof(addr);
#endif
    if (getsockname(srv, (struct sockaddr*)&addr, &alen) != 0) {
        SELFTEST_CLOSE_SOCK(srv);
        return false;
    }
    selftest_socket_t cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli == SELFTEST_INVALID_SOCKET) {
        SELFTEST_CLOSE_SOCK(srv);
        return false;
    }
#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(cli, (long)FIONBIO, &nonblock);
#else
    int flags = fcntl(cli, F_GETFL, 0);
    if (flags >= 0) fcntl(cli, F_SETFL, flags | O_NONBLOCK);
#endif
    bool ok = false;
    int rc = connect(cli, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == 0) {
        ok = true;
    } else {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(cli, &wfds);
        struct timeval tv = {0, 80000};
        if (select((int)cli + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
#ifdef _WIN32
            int elen = sizeof(err);
            if (getsockopt(cli, SOL_SOCKET, SO_ERROR, (char*)&err, &elen) == 0 && err == 0) ok = true;
#else
            socklen_t elen = sizeof(err);
            if (getsockopt(cli, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) ok = true;
#endif
        }
    }
    SELFTEST_CLOSE_SOCK(cli);
    SELFTEST_CLOSE_SOCK(srv);
    return ok;
}

static bool run_suite_cluster_quorum(void) {
#ifdef _WIN32
    _putenv_s("OPENDATA_SERVER_USER", "network-test-admin");
    _putenv_s("OPENDATA_SERVER_PASSWORD", "test-only-password-123456");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    setenv("OPENDATA_SERVER_USER", "network-test-admin", 1);
    setenv("OPENDATA_SERVER_PASSWORD", "test-only-password-123456", 1);
#endif
    selftest_mkdir("selftest_tmp");
    selftest_mkdir("selftest_tmp/backups");

    /* 1. Multi-Instance Contexts */
    const char* dbA = "selftest_tmp/ctx_a.pcod";
    const char* dbB = "selftest_tmp/ctx_b.pcod";
    selftest_remove_db_artifacts(dbA);
    selftest_remove_db_artifacts(dbB);

    OpendataCtx* ctxA = opendata_ctx_create();
    OpendataCtx* ctxB = opendata_ctx_create();
    ST_CHECK(ctxA && ctxB);
    ST_CHECK(opendata_ctx_open(ctxA, dbA, NULL, 0, NULL));
    ST_CHECK(opendata_ctx_open(ctxB, dbB, NULL, 0, NULL));
    ST_CHECK(opendata_ctx_exec(ctxA, "CREATE TABLE table_a (id NUMBER PRIMARY KEY, val VARCHAR2(50));"));
    ST_CHECK(opendata_ctx_exec(ctxA, "INSERT INTO table_a VALUES (10, 'Instance A data');"));
    ST_CHECK(opendata_ctx_commit(ctxA));
    ST_CHECK(opendata_ctx_exec(ctxB, "CREATE TABLE table_b (id NUMBER PRIMARY KEY, num NUMBER);"));
    ST_CHECK(opendata_ctx_exec(ctxB, "INSERT INTO table_b VALUES (20, 9999);"));
    ST_CHECK(opendata_ctx_commit(ctxB));
    ST_CHECK(!opendata_ctx_exec(ctxA, "SELECT * FROM table_b;"));
    ST_CHECK(!opendata_ctx_exec(ctxB, "SELECT * FROM table_a;"));
    opendata_ctx_close(ctxA);
    opendata_ctx_close(ctxB);
    opendata_ctx_destroy(ctxA);
    opendata_ctx_destroy(ctxB);
    selftest_remove_db_artifacts(dbA);
    selftest_remove_db_artifacts(dbB);

    /* 2. Correlated Subqueries & Window Functions & Streaming Loader */
    const char* db_sub = "selftest_tmp/subquery.pcod";
    selftest_remove_db_artifacts(db_sub);
    ST_CHECK(opendata_open(db_sub, NULL, 0, NULL));
    ST_CHECK(opendata_exec("CREATE TABLE authors (id NUMBER PRIMARY KEY, name VARCHAR2(50));"));
    ST_CHECK(opendata_exec("CREATE TABLE books (id NUMBER PRIMARY KEY, author_id NUMBER, title VARCHAR2(50));"));
    ST_CHECK(opendata_exec("INSERT INTO authors VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie');"));
    ST_CHECK(opendata_exec("INSERT INTO books VALUES (101, 1, 'Algo'), (102, 1, 'DS'), (103, 2, 'DistSys');"));
    OpendataStmt* s1 = opendata_prepare("SELECT name FROM authors a WHERE EXISTS (SELECT * FROM books b WHERE b.author_id = a.id) ORDER BY a.id;");
    ST_CHECK(s1 != NULL && opendata_step(s1) && !strcmp(opendata_column_text(s1, 0), "Alice"));
    ST_CHECK(opendata_step(s1) && !strcmp(opendata_column_text(s1, 0), "Bob") && !opendata_step(s1));
    opendata_finalize(s1);
    opendata_close();
    selftest_remove_db_artifacts(db_sub);

    /* 3. IRIS Internal Text Editor */
    IrisEditor* ed = iris_create();
    ST_CHECK(ed != NULL);
    ST_CHECK(iris_load_text(ed, "SELECT id, name\nFROM authors\nWHERE id = 1;"));
    ST_CHECK(iris_get_line_count(ed) == 3);
    ST_CHECK(iris_feed_key(ed, IRIS_KEY_DOWN) && iris_feed_key(ed, IRIS_KEY_END));
    ST_CHECK(iris_feed_key(ed, IRIS_KEY_CTRL_K) && iris_get_line_count(ed) == 2);
    ST_CHECK(iris_feed_key(ed, IRIS_KEY_CTRL_U) && iris_get_line_count(ed) == 3);
    ST_CHECK(iris_search(ed, "WHERE"));
    ST_CHECK(iris_load_text(ed, "SELECT   id,   name   FROM   users   WHERE  active = 1   ORDER BY  id DESC;"));
    ST_CHECK(iris_feed_key(ed, IRIS_KEY_CTRL_J) && iris_get_line_count(ed) == 4);
    iris_destroy(ed);

    /* 4. 3-Node Quorum Consensus & Synchronous WAL Shipping */
    const char* db_quorum = "selftest_tmp/quorum_leader.pcod";
    selftest_remove_db_artifacts(db_quorum);
    g_cluster_plaintext_mode = true;
    bool loopback_ok = selftest_can_connect_loopback();

    SelfTestFollowerPeer p2 = { .port = 17502, .node_id = 2 };
    SelfTestFollowerPeer p3 = { .port = 17503, .node_id = 3 };
    atomic_init(&p2.running, true); atomic_init(&p2.acknowledge, true);
    atomic_init(&p3.running, true); atomic_init(&p3.acknowledge, true);
    if (loopback_ok) {
#ifdef _WIN32
        p2.thread = CreateThread(NULL, 0, st_mock_follower_worker, &p2, 0, NULL);
        p3.thread = CreateThread(NULL, 0, st_mock_follower_worker, &p3, 0, NULL);
#else
        pthread_create(&p2.thread, NULL, st_mock_follower_worker, &p2);
        pthread_create(&p3.thread, NULL, st_mock_follower_worker, &p3);
#endif
        selftest_sleep_ms(100);
    }

    ClusterConfig cfg1;
    memset(&cfg1, 0, sizeof(cfg1));
    cfg1.node_id = 1;
    cfg1.port = 17501;
    cfg1.is_leader = true;
    cfg1.sync_replication = loopback_ok;
    cfg1.peer_count = 2;
    snprintf(cfg1.peer_addrs[0], sizeof(cfg1.peer_addrs[0]), "2@127.0.0.1:17502");
    snprintf(cfg1.peer_addrs[1], sizeof(cfg1.peer_addrs[1]), "3@127.0.0.1:17503");
    ST_CHECK(cluster_init(&cfg1));
    ST_CHECK(raft_get_current_term() >= 1);

    ST_CHECK(opendata_open(db_quorum, NULL, 0, NULL));
    ST_CHECK(opendata_exec("CREATE TABLE resilient_data (id NUMBER PRIMARY KEY, info VARCHAR2(100));"));
    ST_CHECK(opendata_exec("INSERT INTO resilient_data VALUES (1, 'Guaranteed zero data loss');"));
    ST_CHECK(opendata_commit());
    uint64_t committed_gen = opendata_get_generation();

    if (loopback_ok) {
        atomic_store(&p2.acknowledge, false);
        atomic_store(&p3.acknowledge, false);
        ST_CHECK(opendata_exec("INSERT INTO resilient_data VALUES (2, 'Must fail without quorum');"));
        ST_CHECK(!opendata_commit());
        ST_CHECK(opendata_get_generation() == committed_gen);
        ST_CHECK(opendata_rollback());
    }
    opendata_close();

    if (loopback_ok) {
        atomic_store(&p2.running, false);
        atomic_store(&p3.running, false);
#ifdef _WIN32
        WaitForSingleObject(p2.thread, 1000); CloseHandle(p2.thread); p2.thread = NULL;
        WaitForSingleObject(p3.thread, 1000); CloseHandle(p3.thread); p3.thread = NULL;
#else
        pthread_join(p2.thread, NULL);
        pthread_join(p3.thread, NULL);
#endif
    }

    cluster_shutdown();
    g_cluster_plaintext_mode = false;
    selftest_remove_db_artifacts(db_quorum);

    /* 5. SQL Savepoints, Composite Keys, Scalar Functions, Sets, FK Cascades, UPSERT, Vector SIMD, PGv3 Wire, MSSQL/Oracle Files */
    const char* db_adv = "selftest_tmp/adv_features.pcod";
    const char* mssql_mdf = "selftest_tmp/mssql_store.mdf";
    const char* oracle_dbf = "selftest_tmp/oracle_store.dbf";
    selftest_remove_db_artifacts(db_adv);
    selftest_remove_db_artifacts(mssql_mdf);
    selftest_remove_db_artifacts(oracle_dbf);

    ST_CHECK(opendata_open(db_adv, NULL, 0, NULL));
    bool quit = false;
    QueryResultCapture cap = { .silent = true };
    g_result_capture = &cap;

    /* Savepoints */
    ST_CHECK(execute_console_statement("CREATE TABLE sp_demo (id NUMBER PRIMARY KEY, val VARCHAR2(20));", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO sp_demo VALUES (1, 'One');", &quit));
    ST_CHECK(execute_console_statement("SAVEPOINT s1;", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO sp_demo VALUES (2, 'Two');", &quit));
    ST_CHECK(execute_console_statement("ROLLBACK TO SAVEPOINT s1;", &quit));
    ST_CHECK(execute_console_statement("SELECT COUNT(*) FROM sp_demo;", &quit));
    ST_CHECK(cap.row_count == 1 && !strcmp(cap.rows[0][0].text, "1"));
    ST_CHECK(execute_console_statement("RELEASE SAVEPOINT s1;", &quit));

    /* Composite Keys */
    ST_CHECK(execute_console_statement("CREATE TABLE order_lines (order_id NUMBER, line_no NUMBER, sku VARCHAR2(30), PRIMARY KEY (order_id, line_no), UNIQUE (sku, order_id));", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO order_lines VALUES (101, 1, 'WIDGET-A');", &quit));
    int prev_errs = opendata_get_error_count();
    execute_console_statement("INSERT INTO order_lines VALUES (101, 1, 'WIDGET-B');", &quit);
    ST_CHECK(opendata_get_error_count() > prev_errs);

    /* UPSERT & ON DUPLICATE KEY */
    ST_CHECK(execute_console_statement("CREATE TABLE inv (id NUMBER PRIMARY KEY, name VARCHAR2(50), qty NUMBER);", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO inv VALUES (1, 'Rack Server', 10), (2, 'Switch', 5);", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO inv VALUES (1, 'Blade Server', 15) ON CONFLICT (id) DO UPDATE SET qty = inv.qty + EXCLUDED.qty, name = EXCLUDED.name;", &quit));
    ST_CHECK(execute_console_statement("SELECT name, qty FROM inv WHERE id = 1;", &quit));
    ST_CHECK(cap.row_count == 1 && !strcmp(cap.rows[0][0].text, "Blade Server") && !strcmp(cap.rows[0][1].text, "25"));

    /* Native VECTOR(dim) & SIMD Distance */
    ST_CHECK(execute_console_statement("CREATE TABLE embeddings (id NUMBER PRIMARY KEY, doc VARCHAR2(40), vec VECTOR(3));", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO embeddings VALUES (1, 'SQL Kernel', '[1.0, 0.0, 0.0]'), (2, 'Vector Index', '[0.8, 0.6, 0.0]');", &quit));
    ST_CHECK(execute_console_statement("SELECT id, doc FROM embeddings WHERE vec <=> '[1.0, 0.0, 0.0]' < 0.5 ORDER BY vec <=> '[1.0, 0.0, 0.0]' ASC;", &quit));
    ST_CHECK(cap.row_count == 2 && !strcmp(cap.rows[0][0].text, "1"));
    ST_CHECK(execute_console_statement("COMMIT;", &quit));

    free(cap.rows);
    g_result_capture = NULL;

    /* PostgreSQL v3 Wire Protocol Front-End Shim */
    if (loopback_ok) {
#ifdef _WIN32
        WSADATA wsa_pg;
        WSAStartup(MAKEWORD(2, 2), &wsa_pg);
#endif
        static ServerConfig pg_cfg = { .port = 17352, .bind_addr = "127.0.0.1", .tcps = false };
        atomic_store(&g_stop, false);
#ifdef _WIN32
        HANDLE pg_srv_th = CreateThread(NULL, 0, st_async_server_thread, &pg_cfg, 0, NULL);
        ST_CHECK(pg_srv_th != NULL);
#else
        pthread_t pg_srv_th;
        ST_CHECK(pthread_create(&pg_srv_th, NULL, st_async_server_thread, &pg_cfg) == 0);
#endif
        selftest_sleep_ms(150);

        selftest_socket_t pg_sock = socket(AF_INET, SOCK_STREAM, 0);
        ST_CHECK(pg_sock != SELFTEST_INVALID_SOCKET);
#ifdef _WIN32
        DWORD tv_ms = 2000;
        setsockopt(pg_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
        setsockopt(pg_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
        struct timeval tv_sock = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(pg_sock, SOL_SOCKET, SO_RCVTIMEO, (const void*)&tv_sock, sizeof(tv_sock));
        setsockopt(pg_sock, SOL_SOCKET, SO_SNDTIMEO, (const void*)&tv_sock, sizeof(tv_sock));
#endif
        struct sockaddr_in pg_addr;
        memset(&pg_addr, 0, sizeof(pg_addr));
        pg_addr.sin_family = AF_INET;
        pg_addr.sin_port = htons(17352);
        pg_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        int conn_res = -1;
        for (int retry = 0; retry < 10; retry++) {
            conn_res = connect(pg_sock, (struct sockaddr*)&pg_addr, sizeof(pg_addr));
            if (conn_res == 0) break;
            selftest_sleep_ms(100);
        }
        ST_CHECK(conn_res == 0);

        uint8_t ssl_req[8];
        st_write_be32(ssl_req, 8);
        st_write_be32(ssl_req + 4, 80877103u);
        send(pg_sock, (const char*)ssl_req, 8, 0);
        char ssl_resp = 0;
        ST_CHECK(recv(pg_sock, &ssl_resp, 1, 0) == 1 && ssl_resp == 'N');

        uint8_t startup_pkt[33] = {0};
        st_write_be32(startup_pkt, sizeof(startup_pkt));
        st_write_be32(startup_pkt + 4, 196608u);
        memcpy(startup_pkt + 8, "user\0network-test-admin\0", 25);
        send(pg_sock, (const char*)startup_pkt, (st_socklen_t)sizeof(startup_pkt), 0);

        uint8_t pg_buf[2048];
        int auth_read = 0;
        while (auth_read < 9) {
            int rlen = (int)recv(pg_sock, (char*)pg_buf + auth_read, (st_socklen_t)(9 - auth_read), 0);
            ST_CHECK(rlen > 0);
            auth_read += rlen;
        }
        ST_CHECK(pg_buf[0] == 'R' && pg_buf[8] == 3);

        const char* password = "test-only-password-123456";
        uint8_t password_pkt[64] = {0};
        password_pkt[0] = 'p';
        st_write_be32(password_pkt + 1, (uint32_t)(strlen(password) + 5));
        memcpy(password_pkt + 5, password, strlen(password) + 1);
        send(pg_sock, (const char*)password_pkt, (st_socklen_t)(strlen(password) + 6), 0);

        int total_rlen = 0;
        while (total_rlen < (int)sizeof(pg_buf)) {
            int rlen = (int)recv(pg_sock, (char*)pg_buf + total_rlen, (st_socklen_t)(sizeof(pg_buf) - (size_t)total_rlen), 0);
            ST_CHECK(rlen > 0);
            total_rlen += rlen;
            if (total_rlen >= 6 && pg_buf[total_rlen - 6] == 'Z') break;
        }
        ST_CHECK(pg_buf[0] == 'R' && pg_buf[total_rlen - 6] == 'Z');

        uint8_t term_pkt[5] = { 'X', 0, 0, 0, 4 };
        send(pg_sock, (const char*)term_pkt, (st_socklen_t)5, 0);
        SELFTEST_CLOSE_SOCK(pg_sock);

        atomic_store(&g_stop, true);
#ifdef _WIN32
        WaitForSingleObject(pg_srv_th, 2000);
        CloseHandle(pg_srv_th);
        pg_srv_th = NULL;
#else
        pthread_join(pg_srv_th, NULL);
#endif
    }
    /* MSSQL .mdf & Oracle .dbf Binary File Export/Import */
    ST_CHECK(opendata_export_mssql(NULL, mssql_mdf));
    ST_CHECK(opendata_export_oracle(NULL, "selftest_tmp/oracle_export.dbf"));
    selftest_remove_db_artifacts("selftest_tmp/oracle_export.dbf");
    opendata_close();

    ST_CHECK(opendata_open(mssql_mdf, NULL, 0, NULL));
    ST_CHECK(execute_console_statement("CREATE TABLE [Employees] ([EmpID] INT IDENTITY(1,1) PRIMARY KEY, [FullName] NVARCHAR(50) NOT NULL);", &quit));
    ST_CHECK(execute_console_statement("INSERT INTO [Employees] VALUES (100, 'Ada Lovelace');", &quit));
    ST_CHECK(execute_console_statement("COMMIT;", &quit));
    opendata_close();

    ST_CHECK(opendata_open(oracle_dbf, NULL, 0, NULL));
    ST_CHECK(opendata_import_mssql("Employees", mssql_mdf));
    ST_CHECK(execute_console_statement("COMMIT;", &quit));
    opendata_close();

    selftest_remove_db_artifacts(db_adv);
    selftest_remove_db_artifacts(mssql_mdf);
    selftest_remove_db_artifacts(oracle_dbf);
    return true;
}

/* =========================================================================
 * Suite 13: Cluster Mutual-TLS & Wallet Validation (from test_cluster_tls.c)
 * ========================================================================= */
static bool run_suite_cluster_tls(void) {
    selftest_mkdir("selftest_tmp");
    selftest_mkdir("selftest_tmp/bad_wallet");

    /* Verify wallet rejection on missing or invalid configuration */
    TlsWallet wallet;
    memset(&wallet, 0, sizeof(wallet));
    ST_CHECK(!load_tls_wallet("selftest_tmp/non_existent_wallet_dir", &wallet));
    ST_CHECK(!load_tls_wallet("selftest_tmp/bad_wallet", &wallet));
    ST_CHECK(!cluster_tls_create("selftest_tmp/missing.pem", "selftest_tmp/missing.key", "selftest_tmp/missing_ca.pem", 1));

    /* If external cluster TLS fixture certificates are configured in env, run live mTLS handshake */
    const char* ca = getenv("OPENDATA_TEST_CLUSTER_CA");
    const char* client_cert = getenv("OPENDATA_TEST_CLUSTER_NODE1_CERT");
    const char* client_key = getenv("OPENDATA_TEST_CLUSTER_NODE1_KEY");
    const char* server_cert = getenv("OPENDATA_TEST_CLUSTER_NODE2_CERT");
    const char* server_key = getenv("OPENDATA_TEST_CLUSTER_NODE2_KEY");
    if (ca && client_cert && server_cert) {
        ClusterTlsContext* server = cluster_tls_create(server_cert, server_key, ca, 2);
        ClusterTlsContext* client = cluster_tls_create(client_cert, client_key, ca, 1);
        ST_CHECK(server != NULL && client != NULL);
        cluster_tls_destroy(client);
        cluster_tls_destroy(server);
    }

    selftest_rmdir("selftest_tmp/bad_wallet");
    return true;
}

/* =========================================================================
 * Master Self-Test Runner & State Preservation
 * ========================================================================= */
typedef struct {
    const char* id;
    const char* description;
    bool (*fn)(void);
} SelfTestSuite;

static const SelfTestSuite g_self_test_suites[] = {
    { "api",              "Embedded C API, DDL/DML, FK/PK & Transactions",        run_suite_api },
    { "auth_rate_limit",  "Authentication Admission & Rate-Limiting Guardrails",  run_suite_auth_rate_limit },
    { "memory_safety",    "Memory Safety, Deep Nesting & Boundary Regressions",   run_suite_memory_safety },
    { "features_suite",   "RFC 4180 CSV, JSON Interchange, Formatting & Sockets", run_suite_features },
    { "recommendations",  "Storage Engines, N-Way Joins, CTEs, Prepared & SWMR",  run_suite_recommendations },
    { "sql_improvements", "Batch DML, Predicates, Aggregates, Joins & ALTER",     run_suite_sql_improvements },
    { "usability",        "Dialect Tolerances, ILIKE, CTAS, Vector Bind & DryRun",run_suite_usability },
    { "large_data",       "Slotted Rows, Buffer Pool, WAL Replay & On-Disk B+Tree",run_suite_large_data },
    { "fuzz",             "Automated SQL Grammar Fuzzing & State Machine Chaos",  run_suite_fuzz },
    { "enterprise_c",     "AES-256 TDE, PITR, Raft, MVCC Vacuum, CBO & Partitions",run_suite_enterprise },
    { "new_features",     "Window Functions, HNSW Vector, Directory Auth & T-SQL",run_suite_new_features },
    { "cluster_quorum",   "3-Node Quorum WAL Shipping, IRIS, UPSERT & PGv3 Wire", run_suite_cluster_quorum },
    { "cluster_tls",      "Cluster Mutual-TLS Context & Wallet Security Checks",  run_suite_cluster_tls }
};

static void selftest_reset_suite_state(void) {
    g_result_capture = NULL;
    if (g_db || g_committed) opendata_close();
    g_path[0] = '\0';
    g_backup_dir[0] = '\0';
    g_backup_interval = 0;
    g_readonly = false;
    g_pending = false;
    g_io_uncertain = false;
    g_autocommit = false;
    g_timing = false;
    g_output_format = FORMAT_TABLE;
    g_interactive_shell = false;
    g_confirm_destructive = false;
    g_spool_file = NULL;
    g_network_user[0] = '\0';
    g_network_user_role = 0;
    g_statement_timeout_ms = 0;
    g_query_start_time_ms = 0;
    g_cluster_plaintext_mode = false;
    g_cluster_active = false;
    g_sync_replication = false;
    atomic_store(&g_raft_role, RAFT_LEADER);
    atomic_store(&g_stop, false);
    atomic_store(&g_backup_failed, false);
    clear_all_savepoints();
}

bool execute_self_test(const char* filter) {
    g_self_test_running = true;
    /* Snapshot and detach the active session state (including live in-memory
       uncommitted transactions, file locks, and backup workers) so running
       SELF-TEST mid-session is completely non-destructive. */
    Database* saved_db = g_db;
    Database* saved_committed = g_committed;
    bool saved_pending = g_pending;
    bool saved_io_uncertain = g_io_uncertain;
    uint64_t saved_generation = g_generation;
    uint64_t saved_identity = g_identity;
    char saved_path[PATH_CAP];
    char saved_bkp[PATH_CAP];
    memcpy(saved_path, g_path, sizeof(saved_path));
    memcpy(saved_bkp, g_backup_dir, sizeof(saved_bkp));
    unsigned saved_interval = g_backup_interval;
    bool saved_readonly = g_readonly;
    bool saved_autocommit = g_autocommit;
    bool saved_interactive = g_interactive_shell;
    bool saved_confirm = g_confirm_destructive;
    bool saved_timing = g_timing;
    OutputFormat saved_format = g_output_format;
    FILE* saved_spool = g_spool_file;
#ifdef _WIN32
    HANDLE saved_lock = get_database_file_lock();
    HANDLE saved_bkp_worker = get_backup_worker_handle();
#else
    int saved_lock = get_database_file_lock();
    pthread_t saved_bkp_worker = get_backup_worker_handle();
#endif
    atomic_bool* saved_bkp_stop = get_backup_worker_stop_flag();
    bool saved_bkp_failed = atomic_load(&g_backup_failed);

    /* Detach active session pointers so self-test suites run in an isolated state */
    g_db = NULL;
    g_committed = NULL;
    g_pending = false;
    g_io_uncertain = false;
    g_generation = 0;
    g_identity = 0;
    g_path[0] = '\0';
    g_backup_dir[0] = '\0';
    g_backup_interval = 0;
    g_readonly = false;
#ifdef _WIN32
    set_database_file_lock(INVALID_HANDLE_VALUE);
#else
    set_database_file_lock(-1);
#endif
    clear_backup_worker_handle();
    set_backup_worker_stop_flag(NULL);
    atomic_store(&g_backup_failed, false);

    g_spool_file = NULL;
    g_interactive_shell = false;
    g_confirm_destructive = false;
    g_autocommit = false;
    g_timing = false;
    g_output_format = FORMAT_TABLE;

    puts("================================================================================");
    puts("          PC/OpenData SQL Server Enterprise - Built-In Self-Test Suite          ");
    puts("================================================================================");

    int total_suites = (int)(sizeof(g_self_test_suites) / sizeof(g_self_test_suites[0]));
    int passed_suites = 0;
    int failed_suites = 0;
    int ran_suites = 0;
    g_st_assertions = 0;
    g_st_failures = 0;

    double total_start = monotonic_seconds();

    /* Redirect stderr noise from intentional negative tests during suites */
    for (int i = 0; i < total_suites; i++) {
        const SelfTestSuite* suite = &g_self_test_suites[i];
        if (filter && *filter) {
            /* Case-insensitive substring, number, or ALL */
            char f_up[64] = {0}, id_up[64] = {0};
            for (size_t c = 0; filter[c] && c + 1 < sizeof(f_up); c++) f_up[c] = uppercase_ascii(filter[c]);
            for (size_t c = 0; suite->id[c] && c + 1 < sizeof(id_up); c++) id_up[c] = uppercase_ascii(suite->id[c]);
            int filter_idx = atoi(filter);
            if (filter_idx != (i + 1) && !strstr(id_up, f_up) && strcmp(f_up, "ALL") != 0) continue;
        }

        ran_suites++;
        int before_assertions = g_st_assertions;
        double s_start = monotonic_seconds();
        printf("  [%02d/%02d] %-18s %-50s ", i + 1, total_suites, suite->id, suite->description);
        fflush(stdout);

        selftest_reset_suite_state();
        selftest_mute_io();
        bool ok = suite->fn();
        selftest_unmute_io();
        selftest_reset_suite_state();

        double elapsed_ms = (monotonic_seconds() - s_start) * 1000.0;
        int suite_checks = g_st_assertions - before_assertions;

        if (ok) {
            printf("PASS (%d checks, %.1f ms)\n", suite_checks, elapsed_ms);
            passed_suites++;
        } else {
            printf("FAIL (%.1f ms)\n", elapsed_ms);
            failed_suites++;
        }
    }

    double total_ms = (monotonic_seconds() - total_start) * 1000.0;
    selftest_rmdir("selftest_tmp/wal_archive");
    selftest_rmdir("selftest_tmp/backups");
    selftest_rmdir("selftest_tmp");

    puts("--------------------------------------------------------------------------------");
    if (ran_suites == 0) {
        printf("No self-test suites matched filter '%s'.\n", filter ? filter : "");
        puts("Available suites: api, auth_rate_limit, memory_safety, features_suite,");
        puts("  recommendations, sql_improvements, usability, large_data, fuzz,");
        puts("  enterprise_c, new_features, cluster_quorum, cluster_tls");
    } else {
        printf("Summary: %d/%d suites passed | %d total checks verified | %.1f ms total\n",
               passed_suites, ran_suites, g_st_assertions, total_ms);
        if (failed_suites == 0) {
            puts("RESULT:  ALL SELF-TEST SUITES PASSED SUCCESSFULLY.");
        } else {
            printf("RESULT:  %d SUITE(S) FAILED.\n", failed_suites);
        }
    }
    puts("================================================================================");

    /* Restore previous live session state untouched */
    g_db = saved_db;
    g_committed = saved_committed;
    g_pending = saved_pending;
    g_io_uncertain = saved_io_uncertain;
    g_generation = saved_generation;
    g_identity = saved_identity;
    memcpy(g_path, saved_path, sizeof(g_path));
    memcpy(g_backup_dir, saved_bkp, sizeof(g_backup_dir));
    g_backup_interval = saved_interval;
    g_readonly = saved_readonly;
    set_database_file_lock(saved_lock);
    set_backup_worker_handle(saved_bkp_worker);
    set_backup_worker_stop_flag(saved_bkp_stop);
    atomic_store(&g_backup_failed, saved_bkp_failed);

    g_spool_file = saved_spool;
    g_interactive_shell = saved_interactive;
    g_confirm_destructive = saved_confirm;
    g_autocommit = saved_autocommit;
    g_timing = saved_timing;
    g_output_format = saved_format;
    atomic_store(&g_errors, 0);
    g_self_test_running = false;

    return (ran_suites > 0 && failed_suites == 0);
}
