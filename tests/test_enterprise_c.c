#include "database_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(void) {
    printf("===================================================================\n");
    printf("   PC/OpenData Enterprise C Test Suite (Items 1 - 4)              \n");
    printf("===================================================================\n\n");

    /* --------------------------------------------------------------------- */
    /* 1. Transparent Data Encryption (TDE) AES-256 Test                     */
    /* --------------------------------------------------------------------- */
    printf("[Item 3] Testing Transparent Data Encryption (TDE) at rest...\n");
    const char* test_tde_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    bool ok = opendata_set_tde_key(test_tde_key);
    assert(ok && "TDE key initialization must succeed");
    assert(g_tde_enabled && "TDE must be enabled");

    /* Create sample database */
    Database* db = calloc(1, sizeof(Database));
    assert(db != NULL);
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
    ok = encode_database_snapshot(db, 1001, 1, &image);
    assert(ok && "Snapshot encode with TDE must succeed");
    assert(image.size >= 28);
    assert(memcmp(image.data, "PCODEN01", 8) == 0 && "Snapshot must have encrypted PCODEN01 magic header");

    /* Decrypt snapshot with valid key */
    uint64_t identity = 0, generation = 0;
    Database* decoded = decode_database_snapshot(&image, &identity, &generation);
    assert(decoded != NULL && "TDE decryption with valid key must succeed");
    assert(identity == 1001 && generation == 1);
    assert(decoded->table_count == 1);
    assert(strcmp(decoded->tables[0].name, "CUSTOMERS") == 0);
    destroy_database(decoded);
    free(image.data);

    /* Test invalid key fails closed */
    encode_database_snapshot(db, 2002, 2, &image);
    opendata_set_tde_key("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    decoded = decode_database_snapshot(&image, &identity, &generation);
    assert(decoded == NULL && "TDE decryption with wrong key must fail closed");
    free(image.data);
    free(db);

    /* Reset TDE */
    opendata_set_tde_key(NULL);
    assert(!g_tde_enabled);
    printf("  [PASS] Transparent Data Encryption (TDE) verified.\n\n");

    /* --------------------------------------------------------------------- */
    /* 2. Point-In-Time Recovery (PITR) WAL Replay Test                      */
    /* --------------------------------------------------------------------- */
    printf("[Item 2] Testing Point-In-Time Recovery (PITR) WAL replay...\n");
    const char* pitr_base = "test_pitr_db";
    char pitr_dat[256];
    snprintf(pitr_dat, sizeof(pitr_dat), "%s.dat", pitr_base);
#ifdef _WIN32
    int p_fd = -1;
    _sopen_s(&p_fd, pitr_dat, _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
#else
    int p_fd = open(pitr_dat, O_CREAT | O_RDWR, 0644);
#endif
    assert(p_fd >= 0);

    BufferPool bp;
    buffer_pool_init(&bp);
    ok = wal_init(pitr_base);
    assert(ok);

    /* Write record 1 with current timestamp */
    uint32_t t1 = (uint32_t)time(NULL);
    const char* payload1 = "DELTA_BEFORE_CUTOFF";
    ok = wal_write_page_delta(0, 50, payload1, (uint16_t)strlen(payload1));
    assert(ok);
    ok = wal_commit_tx(501);
    assert(ok);
    wal_shutdown();

    /* 1. Replay with target_timestamp older than record -> should stop before record */
    BufferPool bp_old;
    buffer_pool_init(&bp_old);
    ok = wal_recover_and_replay_to_timestamp(p_fd, &bp_old, t1 - 10);
    assert(ok && "PITR replay before cutoff must succeed");
    uint8_t* p_old = buffer_pool_fetch(&bp_old, p_fd, 0, false);
    assert(p_old != NULL);
    /* Delta should NOT have been applied */
    assert(memcmp(p_old + 50, payload1, strlen(payload1)) != 0);
    buffer_pool_unpin(&bp_old, 0, false);
    buffer_pool_destroy(&bp_old, p_fd);

    /* 2. Replay with target_timestamp >= t1 -> should replay record 1 */
    ok = wal_recover_and_replay_to_timestamp(p_fd, &bp, t1 + 5);
    assert(ok && "PITR replay to target timestamp must succeed");
    uint8_t* p_mem = buffer_pool_fetch(&bp, p_fd, 0, false);
    assert(p_mem != NULL);
    assert(memcmp(p_mem + 50, payload1, strlen(payload1)) == 0);
    buffer_pool_unpin(&bp, 0, false);
    buffer_pool_destroy(&bp, p_fd);
#ifdef _WIN32
    _close(p_fd);
    DeleteFileA(pitr_dat);
    char wal_del[256];
    snprintf(wal_del, sizeof(wal_del), "%s.wal", pitr_base);
    DeleteFileA(wal_del);
#else
    close(p_fd);
    unlink(pitr_dat);
    char wal_del[256];
    snprintf(wal_del, sizeof(wal_del), "%s.wal", pitr_base);
    unlink(wal_del);
#endif
    printf("  [PASS] Point-In-Time Recovery (PITR) WAL replay verified.\n\n");

    /* --------------------------------------------------------------------- */
    /* 3. Raft Dynamic Membership and Term Tracking Test                     */
    /* --------------------------------------------------------------------- */
    printf("[Item 2] Testing Raft consensus term tracking and membership...\n");
    uint32_t current_term = raft_get_current_term();
    assert(current_term >= 1);
    ok = raft_add_peer("2@127.0.0.1:9002");
    assert(ok && "Dynamic raft peer add must succeed");
    ok = raft_remove_peer(2);
    assert(ok && "Dynamic raft peer remove must succeed");
    printf("  [PASS] Raft consensus controls verified.\n\n");

    /* --------------------------------------------------------------------- */
    /* 4. Session Registry & Observability Counter Test                      */
    /* --------------------------------------------------------------------- */
    printf("[Item 4] Testing Session Registry & Observability Metrics...\n");
    int s1 = register_client_session("192.168.1.100");
    assert(s1 > 0);
    int active = get_active_session_count();
    assert(active >= 1);
    unregister_client_session(s1);
    printf("  [PASS] Session Registry & Observability counters verified.\n\n");

    printf("===================================================================\n");
    printf("   All Enterprise Architecture C Assertions Passed! (100%%)        \n");
    printf("===================================================================\n");
    return 0;
}
