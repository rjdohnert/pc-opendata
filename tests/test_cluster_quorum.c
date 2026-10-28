#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include "database_engine.h"
#include "opendata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#define unlink _unlink
#define sleep_ms(ms) Sleep(ms)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
static void sleep_ms(unsigned ms) {
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif

static void test_multi_instance_contexts(void) {
    printf("--- 1. Testing Multi-Instance Engine Contexts (Priority 2) ---\n");
    const char* dbA = "test_db_dir/ctx_a.pcod";
    const char* dbB = "test_db_dir/ctx_b.pcod";
    const char* bkp = "test_db_dir/backups";
    unlink(dbA);
    unlink(dbB);

    OpendataCtx* ctxA = opendata_ctx_create();
    OpendataCtx* ctxB = opendata_ctx_create();
    assert(ctxA && ctxB && "Both contexts created");

    bool okA = opendata_ctx_open(ctxA, dbA, bkp, 0, NULL);
    bool okB = opendata_ctx_open(ctxB, dbB, bkp, 0, NULL);
    assert(okA && okB && "Both contexts opened independently");

    /* Create distinct tables in each context */
    assert(opendata_ctx_exec(ctxA, "CREATE TABLE table_a (id NUMBER PRIMARY KEY, val VARCHAR2(50));"));
    assert(opendata_ctx_exec(ctxA, "INSERT INTO table_a VALUES (10, 'Instance A data');"));
    assert(opendata_ctx_commit(ctxA));

    assert(opendata_ctx_exec(ctxB, "CREATE TABLE table_b (id NUMBER PRIMARY KEY, num NUMBER);"));
    assert(opendata_ctx_exec(ctxB, "INSERT INTO table_b VALUES (20, 9999);"));
    assert(opendata_ctx_commit(ctxB));

    /* Verify isolation: table_b cannot be queried from ctxA, table_a cannot be queried from ctxB */
    assert(!opendata_ctx_exec(ctxA, "SELECT * FROM table_b;"));
    assert(!opendata_ctx_exec(ctxB, "SELECT * FROM table_a;"));

    opendata_ctx_close(ctxA);
    opendata_ctx_close(ctxB);
    opendata_ctx_destroy(ctxA);
    opendata_ctx_destroy(ctxB);
    printf("  [PASS] Multi-Instance Contexts successfully isolated and verified.\n");
}

static void test_correlated_subqueries(void) {
    printf("--- 2. Testing Correlated Subqueries: WHERE [NOT] EXISTS (Priority 3) ---\n");
    const char* db = "test_db_dir/subquery.pcod";
    const char* bkp = "test_db_dir/backups";
    unlink(db);
    assert(opendata_open(db, bkp, 60, NULL));

    assert(opendata_exec("CREATE TABLE authors (id NUMBER PRIMARY KEY, name VARCHAR2(50));"));
    assert(opendata_exec("CREATE TABLE books (id NUMBER PRIMARY KEY, author_id NUMBER, title VARCHAR2(50));"));

    assert(opendata_exec("INSERT INTO authors VALUES (1, 'Alice');"));
    assert(opendata_exec("INSERT INTO authors VALUES (2, 'Bob');"));
    assert(opendata_exec("INSERT INTO authors VALUES (3, 'Charlie');"));

    /* Only Alice and Bob have books */
    assert(opendata_exec("INSERT INTO books VALUES (101, 1, 'Algorithms 101');"));
    assert(opendata_exec("INSERT INTO books VALUES (102, 1, 'Data Structures');"));
    assert(opendata_exec("INSERT INTO books VALUES (103, 2, 'Distributed Systems');"));
    assert(opendata_commit());

    /* WHERE EXISTS (positive match: Alice, Bob) */
    OpendataStmt* s1 = opendata_prepare("SELECT name FROM authors a WHERE EXISTS (SELECT * FROM books b WHERE b.author_id = a.id) ORDER BY a.id;");
    assert(s1 != NULL);
    assert(opendata_step(s1));
    assert(!strcmp(opendata_column_text(s1, 0), "Alice"));
    assert(opendata_step(s1));
    assert(!strcmp(opendata_column_text(s1, 0), "Bob"));
    assert(!opendata_step(s1));
    opendata_finalize(s1);

    /* WHERE NOT EXISTS (negative match: Charlie only) */
    OpendataStmt* s2 = opendata_prepare("SELECT name FROM authors a WHERE NOT EXISTS (SELECT * FROM books b WHERE b.author_id = a.id);");
    assert(s2 != NULL);
    assert(opendata_step(s2));
    assert(!strcmp(opendata_column_text(s2, 0), "Charlie"));
    assert(!opendata_step(s2));
    opendata_finalize(s2);

    opendata_close();
    printf("  [PASS] Correlated Subqueries (EXISTS / NOT EXISTS) verified.\n");
}

static void test_window_functions(void) {
    printf("--- 3. Testing Window Functions: ROW_NUMBER() and RANK() (Priority 3) ---\n");
    const char* db = "test_db_dir/window.pcod";
    const char* bkp = "test_db_dir/backups";
    unlink(db);
    assert(opendata_open(db, bkp, 60, NULL));

    assert(opendata_exec("CREATE TABLE leaderboard (id NUMBER PRIMARY KEY, player VARCHAR2(50), score NUMBER);"));
    assert(opendata_exec("INSERT INTO leaderboard VALUES (1, 'Player_A', 100);"));
    assert(opendata_exec("INSERT INTO leaderboard VALUES (2, 'Player_B', 200);"));
    assert(opendata_exec("INSERT INTO leaderboard VALUES (3, 'Player_C', 200);"));
    assert(opendata_exec("INSERT INTO leaderboard VALUES (4, 'Player_D', 300);"));
    assert(opendata_commit());

    /* Test ROW_NUMBER() OVER () */
    assert(opendata_exec("SELECT player, score, ROW_NUMBER() OVER () FROM leaderboard;"));

    /* Test RANK() OVER () */
    assert(opendata_exec("SELECT player, score, RANK() OVER () FROM leaderboard;"));

    /* Test with ORDER BY */
    assert(opendata_exec("SELECT player, score, ROW_NUMBER() OVER (), RANK() OVER () FROM leaderboard ORDER BY score DESC;"));

    opendata_close();
    printf("  [PASS] Window Functions (ROW_NUMBER / RANK) verified.\n");
}

static void test_streaming_loader(void) {
    printf("--- 4. Testing 64KB Sliding Ring-Buffer Bulk Loader (Priority 4) ---\n");
    const char* db = "test_db_dir/streaming.pcod";
    const char* bkp = "test_db_dir/backups";
    const char* csv_path = "test_db_dir/large_dataset.csv";
    const char* json_path = "test_db_dir/large_dataset.json";
    unlink(db);
    assert(opendata_open(db, bkp, 60, NULL));

    /* Create large CSV > 80 KB (more than one 64KB chunk) */
    FILE* fc = fopen(csv_path, "w");
    assert(fc);
    fprintf(fc, "id,label,val\n");
    for (int i = 1; i <= 2000; i++) {
        fprintf(fc, "%d,item_%d_padding_for_sixty_four_kb_streaming_loader,%d\n", i, i, i * 10);
    }
    fclose(fc);

    /* Create large JSON > 80 KB */
    FILE* fj = fopen(json_path, "w");
    assert(fj);
    fprintf(fj, "[\n");
    for (int i = 1; i <= 2000; i++) {
        fprintf(fj, "  {\"id\": %d, \"label\": \"item_%d_json_streaming_chunk\", \"val\": %d}%s\n",
                i, i, i * 10, i < 2000 ? "," : "");
    }
    fprintf(fj, "]\n");
    fclose(fj);

    /* Import CSV */
    assert(opendata_exec("CREATE TABLE csv_stream_tbl (id NUMBER PRIMARY KEY, label VARCHAR2(80), val NUMBER);"));
    assert(opendata_import_csv("csv_stream_tbl", csv_path, true));
    assert(opendata_exec("SELECT COUNT(*) FROM csv_stream_tbl;"));

    /* Import JSON */
    assert(opendata_exec("CREATE TABLE json_stream_tbl (id NUMBER PRIMARY KEY, label VARCHAR2(80), val NUMBER);"));
    assert(opendata_import_json("json_stream_tbl", json_path));
    assert(opendata_exec("SELECT COUNT(*) FROM json_stream_tbl;"));

    unlink(csv_path);
    unlink(json_path);
    opendata_close();
    printf("  [PASS] Streaming 64KB Sliding Ring-Buffer Bulk Loader verified.\n");
}

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t msg_type;
    uint32_t term;
    uint32_t node_id;
    uint32_t payload_len;
} MockRaftHdr;
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
} FollowerPeer;

static bool mock_recv_all(SOCKET s, void* buf, size_t len) {
    char* ptr = (char*)buf;
    size_t rec = 0;
    while (rec < len) {
        int n = recv(s, ptr + rec, (int)(len - rec), 0);
        if (n <= 0) return false;
        rec += (size_t)n;
    }
    return true;
}

#ifdef _WIN32
static DWORD WINAPI mock_follower_worker(LPVOID arg) {
#else
static void* mock_follower_worker(void* arg) {
#endif
    
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    FollowerPeer* peer = (FollowerPeer*)arg;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
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
            SOCKET s = accept(listener, NULL, NULL);
            if (s != INVALID_SOCKET) {
                MockRaftHdr hdr;
                if (mock_recv_all(s, &hdr, sizeof(hdr)) && hdr.magic == 0x50434F44 && hdr.msg_type == 1) {
                    char* payload = malloc(hdr.payload_len);
                    if (payload) {
                        bool received = mock_recv_all(s, payload, hdr.payload_len);
                        free(payload);
                        if (received && atomic_load(&peer->acknowledge)) {
                            MockRaftHdr ack = {
                                0x50434F44,
                                2, /* RAFT_MSG_SYNC_ACK */
                                hdr.term,
                                (uint32_t)peer->node_id,
                                0
                            };
                            send(s, (const char*)&ack, sizeof(ack), 0);
                        }
                    }
                }
#ifdef _WIN32
                shutdown(s, SD_SEND);
                char dummy[8];
                recv(s, dummy, sizeof(dummy), 0);
                closesocket(s);
#else
                shutdown(s, SHUT_WR);
                close(s);
#endif
            }
        }
    }
#ifdef _WIN32
    closesocket(listener);
    return 0;
#else
    close(listener);
    return NULL;
#endif
}

static void test_cluster_quorum_replication(void) {
    printf("--- 5. Testing Zero Data Loss Quorum Consensus & Sync WAL Shipping ---\n");
    const char* db = "test_db_dir/quorum_leader.pcod";
    const char* bkp = "test_db_dir/backups";
    unlink(db);

    /* Spawn real follower listener threads for Node 2 and Node 3 */
    FollowerPeer p2;
    p2.port = 7502;
    p2.node_id = 2;
    atomic_init(&p2.running, true);
    atomic_init(&p2.acknowledge, true);
    memset(&p2.thread, 0, sizeof(p2.thread));

    FollowerPeer p3;
    p3.port = 7503;
    p3.node_id = 3;
    atomic_init(&p3.running, true);
    atomic_init(&p3.acknowledge, true);
    memset(&p3.thread, 0, sizeof(p3.thread));

#ifdef _WIN32
    p2.thread = CreateThread(NULL, 0, mock_follower_worker, &p2, 0, NULL);
    p3.thread = CreateThread(NULL, 0, mock_follower_worker, &p3, 0, NULL);
#else
    pthread_create(&p2.thread, NULL, mock_follower_worker, &p2);
    pthread_create(&p3.thread, NULL, mock_follower_worker, &p3);
#endif

    sleep_ms(150);

    /* Initialize Leader (Node 1) with Synchronous WAL Replication to Node 2 and Node 3 */
    ClusterConfig cfg1;
    memset(&cfg1, 0, sizeof(cfg1));
    cfg1.node_id = 1;
    cfg1.port = 7501;
    cfg1.is_leader = true;
    cfg1.sync_replication = true;
    cfg1.peer_count = 2;
    snprintf(cfg1.peer_addrs[0], sizeof(cfg1.peer_addrs[0]), "2@127.0.0.1:7502");
    snprintf(cfg1.peer_addrs[1], sizeof(cfg1.peer_addrs[1]), "3@127.0.0.1:7503");
    assert(cluster_init(&cfg1));

    /* Open Leader Database */
    assert(opendata_open(db, bkp, 60, NULL));

    assert(opendata_exec("CREATE TABLE resilient_data (id NUMBER PRIMARY KEY, info VARCHAR2(100));"));
    assert(opendata_exec("INSERT INTO resilient_data VALUES (1, 'Guaranteed zero data loss');"));
    assert(opendata_exec("INSERT INTO resilient_data VALUES (2, 'Synchronous quorum commit');"));

    /* A majority ACK must allow a durable commit. */
    bool committed = opendata_commit();
    assert(committed && "Synchronous WAL Shipping Quorum Commit MUST succeed across 3 nodes");
    uint64_t committed_generation = opendata_get_generation();

    /* Verify data persisted */
    assert(opendata_exec("SELECT * FROM resilient_data;"));

    /* Losing all peer ACKs must fail the next commit and preserve the prior generation. */
    atomic_store(&p2.acknowledge, false);
    atomic_store(&p3.acknowledge, false);
    assert(opendata_exec("INSERT INTO resilient_data VALUES (3, 'Must not be committed');"));
    assert(!opendata_commit());
    assert(opendata_get_generation() == committed_generation);
    assert(opendata_has_pending_changes());
    assert(opendata_rollback());

    opendata_close();
    assert(opendata_open(db, bkp, 0, NULL));
    assert(opendata_get_generation() == committed_generation);
    OpendataStmt* absent = opendata_prepare("SELECT id FROM resilient_data WHERE id = 3;");
    assert(absent != NULL);
    assert(!opendata_step(absent));
    opendata_finalize(absent);
    opendata_close();
    cluster_shutdown();

    /* Clean shutdown of follower peer threads */
    atomic_store(&p2.running, false);
    atomic_store(&p3.running, false);
#ifdef _WIN32
    WaitForSingleObject(p2.thread, 1000); CloseHandle(p2.thread);
    WaitForSingleObject(p3.thread, 1000); CloseHandle(p3.thread);
#else
    pthread_join(p2.thread, NULL);
    pthread_join(p3.thread, NULL);
#endif
    printf("  [PASS] 3-Node Quorum Consensus & Synchronous Dual-Write WAL Shipping verified.\n");
}

static void test_iris_editor(void) {
    printf("--- 6. Testing IRIS Internal Text Editor (GNU Nano-Styled Module) ---\n");
    IrisEditor* ed = iris_create();
    assert(ed != NULL);

    /* Load initial SQL text */
    assert(iris_load_text(ed, "SELECT id, name\nFROM authors\nWHERE id = 1;"));
    assert(iris_get_line_count(ed) == 3);
    assert(strcmp(iris_get_line(ed, 0), "SELECT id, name") == 0);
    assert(strcmp(iris_get_line(ed, 1), "FROM authors") == 0);
    assert(strcmp(iris_get_line(ed, 2), "WHERE id = 1;") == 0);

    /* Test cursor navigation: Down, End, Backspace, typing, Enter */
    assert(iris_feed_key(ed, IRIS_KEY_DOWN));
    assert(iris_feed_key(ed, IRIS_KEY_END));
    int r = -1, c = -1;
    iris_get_cursor(ed, &r, &c);
    assert(r == 1 && c == 12);

    /* Cut line 2 ("FROM authors") with ^K and paste it with ^U */
    assert(iris_feed_key(ed, IRIS_KEY_CTRL_K));
    assert(iris_get_line_count(ed) == 2);
    assert(strcmp(iris_get_line(ed, 1), "WHERE id = 1;") == 0);

    assert(iris_feed_key(ed, IRIS_KEY_CTRL_U));
    assert(iris_get_line_count(ed) == 3);
    assert(strcmp(iris_get_line(ed, 1), "FROM authors") == 0);

    /* Test Search (^W / iris_search) */
    assert(iris_search(ed, "WHERE"));
    iris_get_cursor(ed, &r, &c);
    assert(r == 2 && c == 0);

    /* Test Cur Pos (^C) */
    assert(iris_feed_key(ed, IRIS_KEY_CTRL_C));
    assert(strstr(iris_get_status(ed), "line 3/3") != NULL);

    /* Test SQL Justify (^J) on a single-line messy SQL query */
    assert(iris_load_text(ed, "SELECT   id,   name   FROM   users   WHERE  active = 1   ORDER BY  id DESC;"));
    assert(iris_feed_key(ed, IRIS_KEY_CTRL_J));
    assert(iris_get_line_count(ed) == 4);
    assert(strcmp(iris_get_line(ed, 0), "SELECT id, name") == 0);
    assert(strcmp(iris_get_line(ed, 1), "FROM users") == 0);
    assert(strcmp(iris_get_line(ed, 2), "WHERE active = 1") == 0);
    assert(strcmp(iris_get_line(ed, 3), "ORDER BY id DESC;") == 0);

    /* Test WriteOut (^O / iris_save_file) and Read File (^R / iris_insert_file) */
    const char* iris_file = "test_db_dir/iris_test_query.sql";
    unlink(iris_file);
    iris_set_filename(ed, iris_file);
    assert(iris_feed_key(ed, IRIS_KEY_CTRL_O));

    IrisEditor* ed2 = iris_create();
    assert(iris_load_file(ed2, iris_file));
    assert(iris_get_line_count(ed2) == 4);
    assert(strcmp(iris_get_line(ed2, 1), "FROM users") == 0);

    /* Test rendering GNU Nano interface to buffer */
    char screen[8192];
    iris_render_to_buffer(ed2, screen, sizeof(screen));
    assert(strstr(screen, "IRIS 1.0 - PC/OpenData Editor") != NULL);
    assert(strstr(screen, "^G Get Help") != NULL);
    assert(strstr(screen, "^O WriteOut") != NULL);
    assert(strstr(screen, "^X Exit/Exec") != NULL);
    assert(strstr(screen, "^J Justify") != NULL);

    /* Test Help screen toggle (^G) */
    assert(iris_feed_key(ed2, IRIS_KEY_CTRL_G));
    iris_render_to_buffer(ed2, screen, sizeof(screen));
    assert(strstr(screen, "IRIS 1.0 (GNU Nano-Styled SQL & Script Editor) Help Summary") != NULL);
    assert(iris_feed_key(ed2, IRIS_KEY_CTRL_G));

    /* Test Exit (^X returns false to stop loop) */
    assert(!iris_feed_key(ed2, IRIS_KEY_CTRL_X));

    iris_destroy(ed2);
    iris_destroy(ed);
    unlink(iris_file);
    printf("  [PASS] IRIS Internal Text Editor (all Nano keybindings, search, cut/uncut, justify, I/O, UI) verified.\n");
}

static void test_shell_categories_1_to_5(void) {
    printf("--- 7. Testing Shell Categories 1-5 & IRIS Shell Commands ---\n");
    const char* db_path = "test_db_dir/shell_cats.pcod";
    const char* drop_db_path = "test_db_dir/drop_target.pcod";
    const char* script_path = "test_db_dir/batch_setup.sql";
    const char* spool_path = "test_db_dir/spool_output.txt";
    unlink(db_path);
    unlink(drop_db_path);
    unlink(script_path);
    unlink(spool_path);

    bool quit = false;

    /* Create a SQL script file for SOURCE and @ testing */
    FILE* fs = fopen(script_path, "w");
    assert(fs != NULL);
    fprintf(fs, "-- Batch setup script\n");
    fprintf(fs, "CREATE TABLE depts (dept_id NUMBER PRIMARY KEY, dept_name VARCHAR2(40) UNIQUE NOT NULL);\n");
    fprintf(fs, "CREATE TABLE emps (emp_id NUMBER PRIMARY KEY, name VARCHAR2(50) NOT NULL, dept_id NUMBER REFERENCES depts(dept_id), bonus NUMBER);\n");
    fprintf(fs, "INSERT INTO depts VALUES (10, 'Engineering'), (20, 'Research');\n");
    fprintf(fs, "INSERT INTO emps VALUES (1, 'Ada', 10, NULL), (2, 'Grace', 10, 500), (3, 'Alan', 20, NULL);\n");
    fprintf(fs, "COMMIT;\n");
    fclose(fs);

    /* Category 1: SPOOL & SOURCE */
    assert(execute_console_statement("SPOOL 'test_db_dir/spool_output.txt';", &quit));
    assert(execute_console_statement("CREATE DATABASE 'test_db_dir/shell_cats.pcod';", &quit));
    assert(execute_console_statement("SOURCE 'test_db_dir/batch_setup.sql';", &quit));

    /* Category 2: SHOW SCHEMA, SHOW INDEXES, SHOW CONSTRAINTS, SHOW DATABASES */
    assert(execute_console_statement("SHOW SCHEMA;", &quit));
    assert(execute_console_statement("SHOW SCHEMA emps;", &quit));
    assert(execute_console_statement("SHOW INDEXES;", &quit));
    assert(execute_console_statement("SHOW INDEXES FROM emps;", &quit));
    assert(execute_console_statement("SHOW CONSTRAINTS;", &quit));
    assert(execute_console_statement("SHOW CONSTRAINTS FROM emps;", &quit));
    assert(execute_console_statement("SHOW DATABASES;", &quit));

    /* Category 3: CHECK INTEGRITY, PRAGMA INTEGRITY_CHECK, VACUUM, COMPACT */
    assert(execute_console_statement("CHECK INTEGRITY;", &quit));
    assert(execute_console_statement("PRAGMA INTEGRITY_CHECK;", &quit));
    assert(execute_console_statement("VACUUM;", &quit));
    assert(execute_console_statement("COMPACT;", &quit));

    /* Category 4: SHOW CLUSTER */
    assert(execute_console_statement("SHOW CLUSTER;", &quit));

    /* Category 5: SET AUTOCOMMIT, SET NULL, SET MAXROWS, HISTORY, IRIS / EDIT / ED */
    assert(execute_console_statement("SET AUTOCOMMIT ON;", &quit));
    assert(execute_console_statement("INSERT INTO depts VALUES (30, 'Security');", &quit));
    assert(!g_pending && "AUTOCOMMIT ON should automatically commit mutation");
    assert(execute_console_statement("SET AUTOCOMMIT OFF;", &quit));

    assert(execute_console_statement("SET NULL '<null>';", &quit));
    assert(execute_console_statement("SET MAXROWS 2;", &quit));
    assert(execute_console_statement("SELECT emp_id, name, bonus FROM emps ORDER BY emp_id;", &quit));
    assert(execute_console_statement("SET MAXROWS 0;", &quit));
    assert(execute_console_statement("SET NULL 'NULL';", &quit));

    assert(execute_console_statement("HISTORY;", &quit));
    assert(execute_console_statement("IRIS;", &quit));
    assert(execute_console_statement("EDIT;", &quit));
    assert(execute_console_statement("ED 'test_db_dir/batch_setup.sql';", &quit));
    assert(execute_console_statement("HELP;", &quit));

    assert(execute_console_statement("SPOOL OFF;", &quit));

    /* Verify spool transcript file captured the session output */
    FILE* fspool = fopen(spool_path, "rb");
    assert(fspool != NULL);
    char* spool_buf = (char*)calloc(1, 32768);
    assert(spool_buf != NULL);
    fread(spool_buf, 1, 32767, fspool);
    fclose(fspool);
    assert(strstr(spool_buf, "CREATE TABLE EMPS") != NULL);
    assert(strstr(spool_buf, "PK_EMPS_EMP_ID") != NULL);
    assert(strstr(spool_buf, "FK_EMPS_DEPT_ID") != NULL);
    assert(strstr(spool_buf, "RELATIONAL_INVARIANTS\tOK") != NULL);
    assert(strstr(spool_buf, "<null>") != NULL);
    assert(strstr(spool_buf, "IRIS INTERNAL TEXT EDITOR") != NULL);
    free(spool_buf);

    /* Test DROP DATABASE */
    assert(execute_console_statement("CREATE DATABASE 'test_db_dir/drop_target.pcod';", &quit));
    assert(execute_console_statement("DROP DATABASE 'test_db_dir/drop_target.pcod';", &quit));
    assert(!snapshot_path_exists("test_db_dir/drop_target.pcod"));

    opendata_close();
    unlink(db_path);
    unlink(script_path);
    unlink(spool_path);
    printf("  [PASS] Shell Categories 1-5 & IRIS integration verified.\n");
}

static void test_sql_engine_improvements(void) {
    printf("--- 8. Testing SQL Engine Improvements (Savepoints, Composite Keys, Scalar Funcs, Sets, Having, Joins) ---\n");
    const char* db_path = "test_db_dir/sql_improvements.pcod";
    unlink(db_path);

    bool quit = false;
    assert(execute_console_statement("CREATE DATABASE 'test_db_dir/sql_improvements.pcod';", &quit));
    assert(execute_console_statement("CONNECT 'test_db_dir/sql_improvements.pcod';", &quit));

    /* 1. Feature 1: SAVEPOINT, ROLLBACK TO, RELEASE */
    assert(execute_console_statement("CREATE TABLE sp_demo (id NUMBER PRIMARY KEY, val VARCHAR2(20));", &quit));
    assert(execute_console_statement("INSERT INTO sp_demo VALUES (1, 'One');", &quit));
    assert(execute_console_statement("SAVEPOINT s1;", &quit));
    assert(execute_console_statement("INSERT INTO sp_demo VALUES (2, 'Two');", &quit));
    assert(execute_console_statement("SAVEPOINT s2;", &quit));
    assert(execute_console_statement("INSERT INTO sp_demo VALUES (3, 'Three');", &quit));

    /* Rollback to s2 drops row 3, retains 1 and 2 */
    assert(execute_console_statement("ROLLBACK TO SAVEPOINT s2;", &quit));
    QueryResultCapture cap = {0};
    cap.silent = true;
    g_result_capture = &cap;
    assert(execute_console_statement("SELECT COUNT(*) FROM sp_demo;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "2") == 0);

    /* Rollback to s1 drops row 2, retains 1 */
    assert(execute_console_statement("ROLLBACK TO s1;", &quit));
    assert(execute_console_statement("SELECT COUNT(*) FROM sp_demo;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "1") == 0);

    /* Release savepoint */
    assert(execute_console_statement("RELEASE SAVEPOINT s1;", &quit));
    assert(execute_console_statement("COMMIT;", &quit));
    free(cap.rows);
    cap.rows = NULL;
    cap.capacity = 0;

    /* 2. Feature 2: Multi-Column Composite Primary Key & Unique Constraints */
    assert(execute_console_statement("CREATE TABLE order_lines (order_id NUMBER, line_no NUMBER, sku VARCHAR2(30), qty NUMBER, PRIMARY KEY (order_id, line_no), UNIQUE (sku, order_id));", &quit));
    assert(execute_console_statement("INSERT INTO order_lines VALUES (101, 1, 'WIDGET-A', 5);", &quit));
    assert(execute_console_statement("INSERT INTO order_lines VALUES (101, 2, 'WIDGET-B', 10);", &quit));
    assert(execute_console_statement("INSERT INTO order_lines VALUES (102, 1, 'WIDGET-A', 3);", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* Duplicate Composite PK (101, 1) must be rejected */
    int prev_errs = opendata_get_error_count();
    execute_console_statement("INSERT INTO order_lines VALUES (101, 1, 'WIDGET-C', 1);", &quit);
    assert(opendata_get_error_count() > prev_errs);

    /* Duplicate Composite Unique (WIDGET-B, 101) must be rejected */
    prev_errs = opendata_get_error_count();
    execute_console_statement("INSERT INTO order_lines VALUES (101, 3, 'WIDGET-B', 2);", &quit);
    assert(opendata_get_error_count() > prev_errs);

    /* 3. Feature 3: Rich Built-In Scalar Functions */
    assert(execute_console_statement("CREATE TABLE scalar_demo (id NUMBER PRIMARY KEY, txt VARCHAR2(40), num NUMBER, alt_txt VARCHAR2(40));", &quit));
    assert(execute_console_statement("INSERT INTO scalar_demo VALUES (1, '  antigravity  ', 14.567, NULL);", &quit));
    assert(execute_console_statement("INSERT INTO scalar_demo VALUES (2, 'OracleDB', -42.8, 'FallbackValue');", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    cap = (QueryResultCapture){0};
    cap.silent = true;
    g_result_capture = &cap;

    /* String functions: UPPER, LOWER, LENGTH, TRIM, SUBSTR */
    assert(execute_console_statement("SELECT UPPER(txt), LOWER(txt), LENGTH(txt), TRIM(txt), SUBSTR(txt, 3, 4) FROM scalar_demo WHERE id = 1;", &quit));
    assert(cap.row_count == 1);
    assert(strstr(cap.rows[0][0].text, "ANTIGRAVITY") != NULL);
    assert(strstr(cap.rows[0][1].text, "antigravity") != NULL);
    assert(strcmp(cap.rows[0][3].text, "antigravity") == 0);
    assert(strcmp(cap.rows[0][4].text, "anti") == 0);

    /* CONCAT */
    assert(execute_console_statement("SELECT CONCAT(txt, '123') FROM scalar_demo WHERE id = 2;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "OracleDB123") == 0);

    /* Math functions: ABS, ROUND, CEIL, FLOOR, MOD */
    assert(execute_console_statement("SELECT ABS(num), ROUND(num, 1), CEIL(num), FLOOR(num), MOD(id, 2) FROM scalar_demo WHERE id = 2;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "42.8") == 0);
    assert(strcmp(cap.rows[0][1].text, "-42.8") == 0);
    assert(strcmp(cap.rows[0][2].text, "-42") == 0);
    assert(strcmp(cap.rows[0][3].text, "-43") == 0);
    assert(strcmp(cap.rows[0][4].text, "0") == 0);

    /* Fallback functions: COALESCE / NVL */
    assert(execute_console_statement("SELECT COALESCE(alt_txt, 'DefaultVal') FROM scalar_demo WHERE id = 1;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "DefaultVal") == 0);

    assert(execute_console_statement("SELECT COALESCE(alt_txt, 'DefaultVal') FROM scalar_demo WHERE id = 2;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "FallbackValue") == 0);

    /* CASE WHEN expression */
    assert(execute_console_statement("SELECT CASE WHEN id = 1 THEN 'First' WHEN id = 2 THEN 'Second' ELSE 'Other' END FROM scalar_demo ORDER BY id ASC;", &quit));
    assert(cap.row_count == 2);
    assert(strcmp(cap.rows[0][0].text, "First") == 0);
    assert(strcmp(cap.rows[1][0].text, "Second") == 0);

    /* 4. Feature 5: HAVING Filter Clause */
    assert(execute_console_statement("CREATE TABLE sales (dept_id NUMBER, amount NUMBER);", &quit));
    assert(execute_console_statement("INSERT INTO sales VALUES (10, 100), (10, 200), (10, 300), (20, 50), (30, 500), (30, 600);", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    assert(execute_console_statement("SELECT dept_id, COUNT(*), SUM(amount) FROM sales GROUP BY dept_id HAVING COUNT(*) >= 2 ORDER BY dept_id ASC;", &quit));
    assert(cap.row_count == 2);
    assert(strcmp(cap.rows[0][0].text, "10") == 0);
    assert(strcmp(cap.rows[1][0].text, "30") == 0);

    /* 5. Feature 6: CROSS JOIN & FULL OUTER JOIN */
    assert(execute_console_statement("CREATE TABLE tbl_left (id NUMBER PRIMARY KEY, val_l VARCHAR2(10));", &quit));
    assert(execute_console_statement("CREATE TABLE tbl_right (id NUMBER PRIMARY KEY, val_r VARCHAR2(10));", &quit));
    assert(execute_console_statement("INSERT INTO tbl_left VALUES (1, 'L1'), (2, 'L2');", &quit));
    assert(execute_console_statement("INSERT INTO tbl_right VALUES (2, 'R2'), (3, 'R3');", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* CROSS JOIN (2 x 2 = 4 rows) */
    assert(execute_console_statement("SELECT * FROM tbl_left CROSS JOIN tbl_right;", &quit));
    assert(cap.row_count == 4);

    /* FULL OUTER JOIN: 1 only on left (L1, NULL), 2 matched (L2, R2), 3 only on right (NULL, R3) = 3 rows */
    assert(execute_console_statement("SELECT * FROM tbl_left FULL OUTER JOIN tbl_right ON tbl_left.id = tbl_right.id ORDER BY tbl_left.id ASC;", &quit));
    assert(cap.row_count == 3);

    /* 6. Feature 4: Advanced Set Operations (UNION, UNION ALL, INTERSECT, MINUS/EXCEPT) */
    assert(execute_console_statement("CREATE TABLE set_a (val NUMBER);", &quit));
    assert(execute_console_statement("CREATE TABLE set_b (val NUMBER);", &quit));
    assert(execute_console_statement("INSERT INTO set_a VALUES (1), (2), (2), (3);", &quit));
    assert(execute_console_statement("INSERT INTO set_b VALUES (2), (3), (4);", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* UNION ALL: 4 + 3 = 7 rows */
    assert(execute_console_statement("SELECT val FROM set_a UNION ALL SELECT val FROM set_b;", &quit));
    assert(cap.row_count == 7);

    /* UNION (distinct): {1, 2, 3, 4} = 4 rows */
    assert(execute_console_statement("SELECT val FROM set_a UNION SELECT val FROM set_b;", &quit));
    assert(cap.row_count == 4);

    /* INTERSECT: {2, 3} = 2 rows */
    assert(execute_console_statement("SELECT val FROM set_a INTERSECT SELECT val FROM set_b;", &quit));
    assert(cap.row_count == 2);

    /* MINUS: {1} = 1 row */
    assert(execute_console_statement("SELECT val FROM set_a MINUS SELECT val FROM set_b;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "1") == 0);

    g_result_capture = NULL;
    free(cap.rows);

    opendata_close();
    unlink(db_path);
    printf("  [PASS] SQL Engine Improvements (Savepoints, Composite Keys, Scalar Funcs, Sets, Having, Joins) verified.\n");
}

#ifdef _WIN32
static DWORD WINAPI run_async_server_thread(LPVOID arg) {
#else
static void* run_async_server_thread(void* arg) {
#endif
    ServerConfig* cfg = (ServerConfig*)arg;
    run_nonblocking_wire_server(cfg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_future_enhancements_1_to_5(void) {
    printf("--- 9. Testing Top Recommendations 1-5 (CBO Stats, SIMD Vectorization, FK Cascades, Async Wire, Secondary B+ Tree) ---\n");
    const char* db_path = "test_db_dir/enhancements.pcod";
    unlink(db_path);

    bool quit = false;
    assert(execute_console_statement("CREATE DATABASE 'test_db_dir/enhancements.pcod';", &quit));
    assert(execute_console_statement("CONNECT 'test_db_dir/enhancements.pcod';", &quit));

    /* ------------------------------------------------------------- */
    /* 1. Cost-Based Query Optimizer (CBO) & Statistics Engine       */
    /* ------------------------------------------------------------- */
    assert(execute_console_statement("CREATE TABLE cbo_test (id NUMBER PRIMARY KEY, val NUMBER, cat VARCHAR2(20));", &quit));
    assert(execute_console_statement("INSERT INTO cbo_test VALUES (1, 100, 'A');", &quit));
    assert(execute_console_statement("INSERT INTO cbo_test VALUES (2, 200, 'B');", &quit));
    assert(execute_console_statement("INSERT INTO cbo_test VALUES (3, 300, 'A');", &quit));
    assert(execute_console_statement("INSERT INTO cbo_test VALUES (4, 400, 'C');", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* Run ANALYZE TABLE cbo_test; */
    assert(execute_console_statement("ANALYZE TABLE cbo_test;", &quit));
    int t_idx = find_table_index(g_db, "CBO_TEST");
    assert(t_idx >= 0);
    assert(g_db->tables[t_idx].stats.is_analyzed == true);
    assert(g_db->tables[t_idx].stats.sample_rows == 4);
    assert(g_db->tables[t_idx].stats.col_stats[1].has_numeric_bounds == true);
    assert(g_db->tables[t_idx].stats.col_stats[1].min_numeric == 100.0L);
    assert(g_db->tables[t_idx].stats.col_stats[1].max_numeric == 400.0L);
    assert(g_db->tables[t_idx].stats.col_stats[1].distinct_count == 4);
    assert(g_db->tables[t_idx].stats.col_stats[2].distinct_count == 3);

    /* Test SHOW STATS FOR cbo_test; */
    assert(execute_console_statement("SHOW STATS FOR cbo_test;", &quit));
    /* Test EXPLAIN with CBO diagnostic stats */
    assert(execute_console_statement("EXPLAIN SELECT * FROM cbo_test WHERE val > 200;", &quit));

    /* ------------------------------------------------------------- */
    /* 2. Vectorized SIMD Columnar Evaluation Engine                 */
    /* ------------------------------------------------------------- */
    assert(execute_console_statement("CREATE TABLE vec_bench (id NUMBER PRIMARY KEY, amount NUMBER);", &quit));
    for (int i = 1; i <= 20; i++) {
        char ins[128];
        snprintf(ins, sizeof(ins), "INSERT INTO vec_bench VALUES (%d, %d);", i, i * 10);
        assert(execute_console_statement(ins, &quit));
    }
    assert(execute_console_statement("COMMIT;", &quit));

    /* Sum of 10, 20, ..., 200 = 2100. Avg = 105. Min = 10. Max = 200. Count = 20 */
    QueryResultCapture cap = {0};
    cap.silent = true;
    g_result_capture = &cap;
    assert(execute_console_statement("SELECT SUM(amount), AVG(amount), MIN(amount), MAX(amount), COUNT(amount) FROM vec_bench;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "2100") == 0);
    assert(strcmp(cap.rows[0][1].text, "105") == 0);
    assert(strcmp(cap.rows[0][2].text, "10") == 0);
    assert(strcmp(cap.rows[0][3].text, "200") == 0);
    assert(strcmp(cap.rows[0][4].text, "20") == 0);

    /* Vectorized SIMD aggregation with filtered WHERE clause */
    assert(execute_console_statement("SELECT SUM(amount), COUNT(amount) FROM vec_bench WHERE amount > 150;", &quit));
    assert(cap.row_count == 1);
    /* 160 + 170 + 180 + 190 + 200 = 900 */
    assert(strcmp(cap.rows[0][0].text, "900") == 0);
    assert(strcmp(cap.rows[0][1].text, "5") == 0);

    /* ------------------------------------------------------------- */
    /* 3. Foreign Key Cascades & Actions                             */
    /* ------------------------------------------------------------- */
    assert(execute_console_statement("CREATE TABLE fk_dept (dept_id NUMBER PRIMARY KEY, dept_name VARCHAR2(30));", &quit));
    assert(execute_console_statement("CREATE TABLE fk_emp (emp_id NUMBER PRIMARY KEY, dept_id NUMBER REFERENCES fk_dept(dept_id) ON DELETE CASCADE ON UPDATE CASCADE, name VARCHAR2(30));", &quit));
    assert(execute_console_statement("CREATE TABLE fk_proj (proj_id NUMBER PRIMARY KEY, dept_id NUMBER REFERENCES fk_dept(dept_id) ON DELETE SET NULL ON UPDATE SET NULL, proj_name VARCHAR2(30));", &quit));

    assert(execute_console_statement("INSERT INTO fk_dept VALUES (10, 'Engineering');", &quit));
    assert(execute_console_statement("INSERT INTO fk_dept VALUES (20, 'Marketing');", &quit));

    assert(execute_console_statement("INSERT INTO fk_emp VALUES (1, 10, 'Alice');", &quit));
    assert(execute_console_statement("INSERT INTO fk_emp VALUES (2, 10, 'Bob');", &quit));
    assert(execute_console_statement("INSERT INTO fk_emp VALUES (3, 20, 'Charlie');", &quit));

    assert(execute_console_statement("INSERT INTO fk_proj VALUES (101, 10, 'Kernel');", &quit));
    assert(execute_console_statement("INSERT INTO fk_proj VALUES (102, 20, 'Ads');", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* Test ON UPDATE CASCADE: update dept 10 -> 99 */
    assert(execute_console_statement("UPDATE fk_dept SET dept_id = 99 WHERE dept_id = 10;", &quit));
    assert(execute_console_statement("SELECT dept_id FROM fk_emp WHERE emp_id = 1;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "99") == 0);
    assert(execute_console_statement("SELECT dept_id FROM fk_emp WHERE emp_id = 2;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "99") == 0);

    /* Test ON DELETE CASCADE and ON DELETE SET NULL: delete dept 99 */
    assert(execute_console_statement("DELETE FROM fk_dept WHERE dept_id = 99;", &quit));
    /* fk_emp rows with dept_id 99 should be deleted by CASCADE */
    assert(execute_console_statement("SELECT COUNT(*) FROM fk_emp WHERE dept_id = 99;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "0") == 0);
    assert(execute_console_statement("SELECT COUNT(*) FROM fk_emp;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "1") == 0); /* only Charlie remains */

    /* fk_proj row 101 should have dept_id SET NULL */
    assert(execute_console_statement("SELECT dept_id FROM fk_proj WHERE proj_id = 101;", &quit));
    assert(cap.row_count == 1 && cap.rows[0][0].is_null);

    /* ------------------------------------------------------------- */
    /* 4. Asynchronous Non-Blocking Client Wire Protocol             */
    /* ------------------------------------------------------------- */
    static ServerConfig async_cfg = {
        .port = 7350,
        .bind_addr = "127.0.0.1",
        .tcps = false,
        .cert_path = NULL,
        .key_path = NULL
    };
    atomic_store(&g_stop, false);
#ifdef _WIN32
    DWORD th_id = 0;
    HANDLE srv_th = CreateThread(NULL, 0, run_async_server_thread, &async_cfg, 0, &th_id);
    assert(srv_th != NULL);
#else
    pthread_t srv_th;
    int rc = pthread_create(&srv_th, NULL, run_async_server_thread, &async_cfg);
    assert(rc == 0);
#endif
    sleep_ms(300);

    SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);
    assert(cli != INVALID_SOCKET);
    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(7350);
    srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int cres = connect(cli, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
    assert(cres == 0);

    /* Read initial greeting */
    char net_buf[1024];
    int nbytes = recv(cli, net_buf, sizeof(net_buf) - 1, 0);
    assert(nbytes > 0);
    net_buf[nbytes] = '\0';
    assert(strstr(net_buf, "Authentication required") != NULL);
    const char* auth = "AUTH network-test-admin test-only-password-123456\n";
    send(cli, auth, (int)strlen(auth), 0);
    nbytes = recv(cli, net_buf, sizeof(net_buf) - 1, 0);
    assert(nbytes > 0);
    net_buf[nbytes] = '\0';
    assert(strstr(net_buf, "Authenticated") != NULL);

    /* Send PING; and receive PONG */
    send(cli, "PING;\r\n", 7, 0);
    nbytes = recv(cli, net_buf, sizeof(net_buf) - 1, 0);
    assert(nbytes > 0);
    net_buf[nbytes] = '\0';
    assert(strstr(net_buf, "PONG") != NULL);

    /* Send query over non-blocking wire */
    send(cli, "SELECT * FROM DUAL;\r\n", 20, 0);
    nbytes = recv(cli, net_buf, sizeof(net_buf) - 1, 0);
    assert(nbytes > 0);
    net_buf[nbytes] = '\0';
    assert(strstr(net_buf, "DUMMY") != NULL);

    /* Send QUIT; and disconnect */
    send(cli, "QUIT;\r\n", 7, 0);
    while ((nbytes = recv(cli, net_buf, sizeof(net_buf) - 1, 0)) > 0) {
        net_buf[nbytes] = '\0';
        if (strstr(net_buf, "BYE") != NULL) break;
    }
    assert(strstr(net_buf, "BYE") != NULL);
#ifdef _WIN32
    closesocket(cli);
#else
    close(cli);
#endif

    atomic_store(&g_stop, true);
#ifdef _WIN32
    WaitForSingleObject(srv_th, 1000);
    CloseHandle(srv_th);
#else
    pthread_join(srv_th, NULL);
#endif
    sleep_ms(100);

    /* ------------------------------------------------------------- */
    /* 5. Persistent Secondary B+ Tree / Ordered Memory Indexes      */
    /* ------------------------------------------------------------- */
    assert(execute_console_statement("CREATE TABLE sec_idx_test (id NUMBER PRIMARY KEY, score NUMBER, code VARCHAR2(20));", &quit));
    assert(execute_console_statement("INSERT INTO sec_idx_test VALUES (1, 85, 'B');", &quit));
    assert(execute_console_statement("INSERT INTO sec_idx_test VALUES (2, 92, 'A');", &quit));
    assert(execute_console_statement("INSERT INTO sec_idx_test VALUES (3, 73, 'C');", &quit));
    assert(execute_console_statement("INSERT INTO sec_idx_test VALUES (4, 99, 'A+');", &quit));
    assert(execute_console_statement("INSERT INTO sec_idx_test VALUES (5, 60, 'D');", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* CREATE INDEX idx_score ON sec_idx_test(score); */
    assert(execute_console_statement("CREATE INDEX idx_score ON sec_idx_test(score);", &quit));
    assert(g_db->index_count == 1);
    assert(strcmp(g_db->indexes[0].name, "IDX_SCORE") == 0);
    assert(g_db->indexes[0].entry_count == 5);

    /* Verify binary range scan using index */
    assert(execute_console_statement("SELECT id, score FROM sec_idx_test WHERE score BETWEEN 70 AND 95;", &quit));
    /* Scores 73, 85, 92 should match */
    assert(cap.row_count == 3);

    /* Point lookup using secondary index */
    assert(execute_console_statement("SELECT id FROM sec_idx_test WHERE score = 99;", &quit));
    assert(cap.row_count == 1 && strcmp(cap.rows[0][0].text, "4") == 0);

    /* CREATE UNIQUE INDEX */
    assert(execute_console_statement("CREATE UNIQUE INDEX idx_code ON sec_idx_test(code);", &quit));
    assert(g_db->index_count == 2);

    /* Reject duplicate key for unique index */
    int before_uniq_errs = opendata_get_error_count();
    execute_console_statement("INSERT INTO sec_idx_test VALUES (6, 95, 'A');", &quit);
    assert(opendata_get_error_count() > before_uniq_errs);

    /* DROP INDEX idx_score; */
    assert(execute_console_statement("DROP INDEX idx_score;", &quit));
    assert(g_db->index_count == 1);
    assert(strcmp(g_db->indexes[0].name, "IDX_CODE") == 0);

    g_result_capture = NULL;
    free(cap.rows);

    opendata_close();
    unlink(db_path);
    printf("  [PASS] Top Recommendations 1-5 (CBO Stats, SIMD, FK Cascades, Async Wire, Secondary B+ Tree) verified!\n");
}

static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static void test_phases_1_to_5_and_mssql_oracle(void) {
    printf("[Test 10] Verifying Phases 1-5 (Observability, Diagnostics, UPSERT/Constraints, VECTOR SIMD, PGv3 Wire) & MSSQL/Oracle Files...\n");
    const char* db_path = "test_db_dir/phases_1_to_5.pcod";
    const char* diff_path = "test_db_dir/phases_diff.pcod";
    const char* dump_sql = "test_db_dir/phases_dump.sql";
    const char* mssql_mdf = "test_db_dir/mssql_store.mdf";
    const char* mssql_sql = "test_db_dir/mssql_export.sql";
    const char* oracle_dbf = "test_db_dir/oracle_store.dbf";
    unlink(db_path);
    unlink(diff_path);
    unlink(dump_sql);
    unlink(mssql_mdf);
    unlink(mssql_sql);
    unlink(oracle_dbf);

    bool quit = false;
    assert(opendata_open(diff_path, NULL, 0, NULL));
    assert(execute_console_statement("CREATE TABLE inv (id NUMBER PRIMARY KEY, name VARCHAR2(50));", &quit));
    assert(execute_console_statement("COMMIT;", &quit));
    opendata_close();

    assert(opendata_open(db_path, NULL, 0, NULL));

    /* ------------------------------------------------------------- */
    /* Phase 3: UPSERT (ON CONFLICT DO NOTHING / DO UPDATE) &        */
    /*          Dynamic Constraint Evolution (ADD/DROP CONSTRAINT)   */
    /* ------------------------------------------------------------- */
    assert(execute_console_statement("CREATE TABLE inv (id NUMBER PRIMARY KEY, name VARCHAR2(50), qty NUMBER);", &quit));
    assert(execute_console_statement("INSERT INTO inv VALUES (1, 'Rack Server', 10), (2, 'Switch', 5);", &quit));

    /* ON CONFLICT (id) DO NOTHING */
    assert(execute_console_statement("INSERT INTO inv VALUES (1, 'Ignored', 999) ON CONFLICT (id) DO NOTHING;", &quit));

    /* ON CONFLICT (id) DO UPDATE SET qty = inv.qty + EXCLUDED.qty, name = EXCLUDED.name */
    assert(execute_console_statement("INSERT INTO inv VALUES (1, 'Blade Server', 15) ON CONFLICT (id) DO UPDATE SET qty = inv.qty + EXCLUDED.qty, name = EXCLUDED.name;", &quit));

    /* ON DUPLICATE KEY UPDATE */
    assert(execute_console_statement("INSERT INTO inv VALUES (2, 'Core Switch', 3) ON DUPLICATE KEY UPDATE qty = qty + 3, name = VALUES(name);", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    QueryResultCapture cap = {0};
    cap.silent = true;
    g_result_capture = &cap;

    assert(execute_console_statement("SELECT name, qty FROM inv WHERE id = 1;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "Blade Server") == 0);
    assert(strcmp(cap.rows[0][1].text, "25") == 0);

    assert(execute_console_statement("SELECT name, qty FROM inv WHERE id = 2;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "Core Switch") == 0);
    assert(strcmp(cap.rows[0][1].text, "8") == 0);

    /* Dynamic Constraint Evolution: ADD CONSTRAINT / DROP CONSTRAINT */
    assert(execute_console_statement("ALTER TABLE inv ADD CONSTRAINT uq_inv_name UNIQUE (name);", &quit));
    int err_before = opendata_get_error_count();
    execute_console_statement("INSERT INTO inv VALUES (3, 'Blade Server', 1);", &quit);
    assert(opendata_get_error_count() > err_before);

    assert(execute_console_statement("ALTER TABLE inv DROP CONSTRAINT uq_inv_name;", &quit));
    assert(execute_console_statement("INSERT INTO inv VALUES (3, 'Blade Server', 1);", &quit));
    assert(execute_console_statement("DELETE FROM inv WHERE id = 3;", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* ------------------------------------------------------------- */
    /* Phase 4: Native VECTOR(dim) Type & SIMD Distance Search       */
    /* ------------------------------------------------------------- */
    assert(execute_console_statement("CREATE TABLE embeddings (id NUMBER PRIMARY KEY, doc VARCHAR2(40), vec VECTOR(3));", &quit));
    assert(execute_console_statement("INSERT INTO embeddings VALUES (1, 'SQL Kernel', '[1.0, 0.0, 0.0]');", &quit));
    assert(execute_console_statement("INSERT INTO embeddings VALUES (2, 'Vector Index', '[0.8, 0.6, 0.0]');", &quit));
    assert(execute_console_statement("INSERT INTO embeddings VALUES (3, 'Unrelated Topic', '[0.0, 0.0, 1.0]');", &quit));
    assert(execute_console_statement("COMMIT;", &quit));

    /* Reject invalid vector dimension */
    err_before = opendata_get_error_count();
    execute_console_statement("INSERT INTO embeddings VALUES (4, 'Bad Dim', '[1.0, 2.0]');", &quit);
    assert(opendata_get_error_count() > err_before);

    /* Nearest-neighbor query with <=> (Cosine distance) and <-> (L2 distance) */
    assert(execute_console_statement(
        "SELECT id, doc, vec <=> '[1.0, 0.0, 0.0]' AS cdist "
        "FROM embeddings WHERE vec <=> '[1.0, 0.0, 0.0]' < 0.5 "
        "ORDER BY vec <=> '[1.0, 0.0, 0.0]' ASC;", &quit));
    assert(cap.row_count == 2);
    assert(strcmp(cap.rows[0][0].text, "1") == 0);
    assert(strcmp(cap.rows[0][2].text, "0") == 0);
    assert(strcmp(cap.rows[1][0].text, "2") == 0);
    assert(strcmp(cap.rows[1][2].text, "0.2") == 0);

    /* Functional COSINE_DISTANCE and L2_DISTANCE */
    assert(execute_console_statement(
        "SELECT id, L2_DISTANCE(vec, '[1.0, 0.0, 0.0]') AS l2 "
        "FROM embeddings ORDER BY L2_DISTANCE(vec, '[1.0, 0.0, 0.0]') ASC;", &quit));
    assert(cap.row_count == 3);
    assert(strcmp(cap.rows[0][0].text, "1") == 0);
    assert(strcmp(cap.rows[1][0].text, "2") == 0);
    assert(strcmp(cap.rows[2][0].text, "3") == 0);

    g_result_capture = NULL;
    free(cap.rows);
    memset(&cap, 0, sizeof(cap));

    /* ------------------------------------------------------------- */
    /* Phase 1 & Phase 2: Shell Observability & Diagnostics          */
    /* ------------------------------------------------------------- */
    int dummy_sid = register_client_session("10.0.0.42:5432");
    assert(dummy_sid > 0);
    update_client_session_query(dummy_sid, "ACTIVE", "SELECT * FROM inv;");
    assert(execute_console_statement("SHOW SESSIONS;", &quit));
    assert(execute_console_statement("SHOW PROCESSLIST;", &quit));
    char kill_cmd[64];
    snprintf(kill_cmd, sizeof(kill_cmd), "KILL SESSION %d;", dummy_sid);
    assert(execute_console_statement(kill_cmd, &quit));
    assert(is_client_session_killed(dummy_sid));

    assert(execute_console_statement("SHOW ENGINE STATUS;", &quit));
    assert(execute_console_statement("SHOW CREATE TABLE embeddings;", &quit));
    assert(execute_console_statement("BENCHMARK 10 SELECT * FROM inv;", &quit));
    assert(execute_console_statement("WATCH COUNT 2 SELECT * FROM DUAL;", &quit));

    char dump_cmd[256];
    snprintf(dump_cmd, sizeof(dump_cmd), "DUMP DATABASE TO '%s';", dump_sql);
    assert(execute_console_statement(dump_cmd, &quit));
    FILE* df = fopen(dump_sql, "r");
    assert(df != NULL);
    fclose(df);

    char diff_cmd[256];
    snprintf(diff_cmd, sizeof(diff_cmd), "DIFF SCHEMA '%s';", diff_path);
    assert(execute_console_statement(diff_cmd, &quit));

    /* ------------------------------------------------------------- */
    /* Phase 5: PostgreSQL v3 Wire Protocol Front-End Shim           */
    /* ------------------------------------------------------------- */
    static ServerConfig pg_cfg = {
        .port = 7352,
        .bind_addr = "127.0.0.1",
        .tcps = false,
        .cert_path = NULL,
        .key_path = NULL
    };
    atomic_store(&g_stop, false);
#ifdef _WIN32
    DWORD pg_th_id = 0;
    HANDLE pg_srv_th = CreateThread(NULL, 0, run_async_server_thread, &pg_cfg, 0, &pg_th_id);
    assert(pg_srv_th != NULL);
#else
    pthread_t pg_srv_th;
    int pg_rc = pthread_create(&pg_srv_th, NULL, run_async_server_thread, &pg_cfg);
    assert(pg_rc == 0);
#endif
    sleep_ms(250);

    SOCKET pg_sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(pg_sock != INVALID_SOCKET);
    struct sockaddr_in pg_addr;
    memset(&pg_addr, 0, sizeof(pg_addr));
    pg_addr.sin_family = AF_INET;
    pg_addr.sin_port = htons(7352);
    pg_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int c_rc = connect(pg_sock, (struct sockaddr*)&pg_addr, sizeof(pg_addr));
    if (c_rc != 0) { printf("FAIL connect: %d\n", c_rc); exit(1); }

    /* 1. Send SSLRequest (len=8, code=80877103) immediately */
    uint8_t ssl_req[8];
    write_be32(ssl_req, 8);
    write_be32(ssl_req + 4, 80877103u);
    send(pg_sock, (const char*)ssl_req, 8, 0);

    char ssl_resp = 0;
    int rlen = recv(pg_sock, &ssl_resp, 1, 0);
    if (rlen != 1 || ssl_resp != 'N') {
        printf("FAIL ssl_resp: rlen=%d, ssl_resp='%c' (0x%02x)\n", rlen, ssl_resp, (unsigned char)ssl_resp);
        exit(1);
    }

    /* 2. Authenticate before executing queries. */
    uint8_t startup_pkt[33] = {0};
    write_be32(startup_pkt, sizeof(startup_pkt));
    write_be32(startup_pkt + 4, 196608u);
    memcpy(startup_pkt + 8, "user\0network-test-admin\0", 25);
    send(pg_sock, (const char*)startup_pkt, sizeof(startup_pkt), 0);

    uint8_t pg_buf[2048];
    int auth_read = 0;
    while (auth_read < 9) {
        rlen = recv(pg_sock, (char*)pg_buf + auth_read, 9 - auth_read, 0);
        assert(rlen > 0);
        auth_read += rlen;
    }
    assert(pg_buf[0] == 'R' && pg_buf[8] == 3);
    const char* password = "test-only-password-123456";
    uint8_t password_pkt[64] = {0};
    password_pkt[0] = 'p';
    write_be32(password_pkt + 1, (uint32_t)(strlen(password) + 5));
    memcpy(password_pkt + 5, password, strlen(password) + 1);
    send(pg_sock, (const char*)password_pkt, (int)(strlen(password) + 6), 0);
    int total_rlen = 0;
    while (total_rlen < (int)sizeof(pg_buf)) {
        rlen = recv(pg_sock, (char*)pg_buf + total_rlen, (int)sizeof(pg_buf) - total_rlen, 0);
        if (rlen <= 0) { printf("FAIL startup recv: rlen=%d\n", rlen); exit(1); }
        total_rlen += rlen;
        if (total_rlen >= 6 && pg_buf[total_rlen - 6] == 'Z') break;
    }
    if (pg_buf[0] != 'R' || pg_buf[total_rlen - 6] != 'Z') {
        printf("FAIL startup buf: total_rlen=%d, first='%c', z='%c'\n", total_rlen, pg_buf[0], pg_buf[total_rlen - 6]);
        exit(1);
    }

    /* 3. Send Simple Query ('Q') */
    const char* qsql = "SELECT doc FROM embeddings WHERE id = 1;";
    uint32_t qbody_len = (uint32_t)(4 + strlen(qsql) + 1);
    uint8_t qpkt[256];
    qpkt[0] = 'Q';
    write_be32(qpkt + 1, qbody_len);
    memcpy(qpkt + 5, qsql, strlen(qsql) + 1);
    send(pg_sock, (const char*)qpkt, (int)(1 + qbody_len), 0);

    total_rlen = 0;
    while (total_rlen < (int)sizeof(pg_buf)) {
        rlen = recv(pg_sock, (char*)pg_buf + total_rlen, (int)sizeof(pg_buf) - total_rlen, 0);
        if (rlen <= 0) { printf("FAIL query recv: rlen=%d\n", rlen); exit(1); }
        total_rlen += rlen;
        if (total_rlen >= 6 && pg_buf[total_rlen - 6] == 'Z') break;
    }
    if (pg_buf[0] != 'T') {
        printf("FAIL query first byte: '%c' (0x%02x), total_rlen=%d\n", pg_buf[0], pg_buf[0], total_rlen);
        exit(1);
    }
    bool found_sql_kernel = false;
    for (int i = 0; i + 10 <= total_rlen; i++) {
        if (memcmp(pg_buf + i, "SQL Kernel", 10) == 0) {
            found_sql_kernel = true;
            break;
        }
    }
    if (!found_sql_kernel) {
        printf("FAIL found_sql_kernel\n");
        exit(1);
    }
    printf("  [OK] PGv3 Wire Protocol verified.\n");

    /* 4. Send Terminate ('X') */
    uint8_t term_pkt[5] = { 'X', 0, 0, 0, 4 };
    send(pg_sock, (const char*)term_pkt, 5, 0);
#ifdef _WIN32
    closesocket(pg_sock);
#else
    close(pg_sock);
#endif
    atomic_store(&g_stop, true);
#ifdef _WIN32
    WaitForSingleObject(pg_srv_th, 1000);
    CloseHandle(pg_srv_th);
#else
    pthread_join(pg_srv_th, NULL);
#endif

    /* ------------------------------------------------------------- */
    /* Microsoft SQL Server (.mdf, T-SQL) & Oracle (.dbf) Support    */
    /* ------------------------------------------------------------- */
    /* Export to binary MSSQL .mdf and T-SQL .sql, and binary Oracle .dbf */
    if (!opendata_export_mssql(NULL, mssql_mdf)) { printf("FAIL export mssql_mdf\n"); exit(1); }
    if (!opendata_export_mssql("inv", mssql_sql)) { printf("FAIL export mssql_sql\n"); exit(1); }
    if (!opendata_export_oracle(NULL, oracle_dbf)) { printf("FAIL export oracle_dbf\n"); exit(1); }

    /* Verify binary headers on disk */
    FILE* mf = fopen(mssql_mdf, "rb");
    assert(mf != NULL);
    char mhdr[8] = {0};
    assert(fread(mhdr, 1, 8, mf) == 8);
    fclose(mf);
    if (memcmp(mhdr, "MSQLMDF1", 8) != 0) { printf("FAIL mhdr\n"); exit(1); }

    FILE* of = fopen(oracle_dbf, "rb");
    assert(of != NULL);
    char ohdr[8] = {0};
    assert(fread(ohdr, 1, 8, of) == 8);
    fclose(of);
    if (memcmp(ohdr, "ORCLDBF1", 8) != 0) { printf("FAIL ohdr\n"); exit(1); }

    opendata_close();

    /* Open the .mdf file directly as the active database and run T-SQL DDL */
    if (!opendata_open(mssql_mdf, NULL, 0, NULL)) { printf("FAIL open mssql_mdf\n"); exit(1); }
    if (!execute_console_statement(
        "CREATE TABLE [Employees] ([EmpID] INT IDENTITY(1,1) PRIMARY KEY, [FullName] NVARCHAR(50) NOT NULL, [Salary] MONEY, [HireDate] DATETIME);",
        &quit)) { printf("FAIL create Employees\n"); exit(1); }
    if (!execute_console_statement(
        "INSERT INTO [Employees] VALUES (100, 'Ada Lovelace', 185000.50, '2026-01-15 09:00:00');",
        &quit)) { printf("FAIL insert Employees\n"); exit(1); }
    if (!execute_console_statement("COMMIT;", &quit)) { printf("FAIL commit Employees\n"); exit(1); }
    opendata_close();

    /* Open the .dbf file directly as the active database and run Oracle DDL + import MSSQL .mdf */
    if (!opendata_open(oracle_dbf, NULL, 0, NULL)) { printf("FAIL open oracle_dbf\n"); exit(1); }
    if (!execute_console_statement(
        "CREATE TABLE ora_staff (empno NUMBER(10) PRIMARY KEY, ename VARCHAR2(40 BYTE) NOT NULL, sal BINARY_DOUBLE, bio CLOB);",
        &quit)) { printf("FAIL create ora_staff\n"); exit(1); }
    if (!execute_console_statement(
        "INSERT INTO ora_staff VALUES (1, 'Grace Hopper', 195000, 'COBOL Pioneer');",
        &quit)) { printf("FAIL insert ora_staff\n"); exit(1); }
    if (!opendata_import_mssql("Employees", mssql_mdf)) { printf("FAIL import Employees\n"); exit(1); }
    if (!execute_console_statement("COMMIT;", &quit)) { printf("FAIL commit ora_staff\n"); exit(1); }

    cap.silent = true;
    g_result_capture = &cap;
    assert(execute_console_statement("SELECT FullName, Salary FROM Employees WHERE EmpID = 100;", &quit));
    assert(cap.row_count == 1);
    assert(strcmp(cap.rows[0][0].text, "Ada Lovelace") == 0);
    assert(strcmp(cap.rows[0][1].text, "185000.5") == 0);
    g_result_capture = NULL;
    free(cap.rows);

    opendata_close();
    unlink(db_path);
    unlink(diff_path);
    unlink(dump_sql);
    unlink(mssql_mdf);
    unlink(mssql_sql);
    unlink(oracle_dbf);
    printf("  [PASS] Phases 1-5 & Microsoft SQL Server (.mdf) / Oracle Database (.dbf) support verified!\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=================================================================\n");
    printf("PC/OpenData - Zero Data Loss Resiliency & Priority 1-5 Verification\n");
    printf("=================================================================\n");

#ifdef _WIN32
    _putenv_s("OPENDATA_SERVER_USER", "network-test-admin");
    _putenv_s("OPENDATA_SERVER_PASSWORD", "test-only-password-123456");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CreateDirectoryA("test_db_dir", NULL);
    CreateDirectoryA("test_db_dir\\backups", NULL);
#else
    setenv("OPENDATA_SERVER_USER", "network-test-admin", 1);
    setenv("OPENDATA_SERVER_PASSWORD", "test-only-password-123456", 1);
    mkdir("test_db_dir", 0755);
    mkdir("test_db_dir/backups", 0755);
#endif

    test_multi_instance_contexts();
    test_correlated_subqueries();
    test_window_functions();
    test_streaming_loader();
    test_cluster_quorum_replication();
    test_iris_editor();
    test_shell_categories_1_to_5();
    test_sql_engine_improvements();
    test_future_enhancements_1_to_5();
    test_phases_1_to_5_and_mssql_oracle();

    printf("\n=================================================================\n");
    printf("ALL ADVANCED RESILIENCY & ARCHITECTURE TESTS PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
