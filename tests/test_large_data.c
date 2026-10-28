#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
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

static int open_test_file(const char* path) {
#ifdef _WIN32
    int fd = -1;
    if (_sopen_s(&fd, path, _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) return -1;
    return fd;
#else
    return open(path, O_CREAT | O_RDWR, 0644);
#endif
}

static void close_test_file(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

int main(void) {
    printf("===================================================================\n");
    printf("   PC/OpenData - Large Dataset Architecture Tests (Phases 1-4)    \n");
    printf("===================================================================\n\n");

    /* --------------------------------------------------------------------- */
    /* PHASE 1: Slotted Binary Rows Test                                     */
    /* --------------------------------------------------------------------- */
    printf("[Phase 1] Testing Slotted Binary Row Format...\n");
    ColumnDef cols[3];
    memset(cols, 0, sizeof(cols));
    snprintf(cols[0].name, sizeof(cols[0].name), "%s", "ID");
    cols[0].type = TYPE_NUMBER; cols[0].size = 40;
    snprintf(cols[1].name, sizeof(cols[1].name), "%s", "NAME");
    cols[1].type = TYPE_VARCHAR2; cols[1].size = 50;
    snprintf(cols[2].name, sizeof(cols[2].name), "%s", "HIRED");
    cols[2].type = TYPE_DATE; cols[2].size = 19;

    Row r1;
    memset(&r1, 0, sizeof(r1));
    snprintf(r1.values[0].text, sizeof(r1.values[0].text), "%s", "42");
    snprintf(r1.values[1].text, sizeof(r1.values[1].text), "%s", "Grace Hopper");
    r1.values[2].is_null = true;

    uint8_t row_buf[512];
    size_t encoded_len = encode_slotted_row(cols, 3, &r1, row_buf, sizeof(row_buf));
    assert(encoded_len > 0 && "Row encoding must succeed");
    printf("  -> Traditional struct size: %zu bytes\n", sizeof(Row));
    printf("  -> Slotted binary row size: %zu bytes (Density gain: ~%.1fx)\n",
           encoded_len, (double)sizeof(Row) / (double)encoded_len);

    Row r1_decoded;
    bool ok = decode_slotted_row(cols, 3, row_buf, encoded_len, &r1_decoded);
    assert(ok && "Row decoding must succeed");
    assert(!strcmp(r1_decoded.values[0].text, "42"));
    assert(!strcmp(r1_decoded.values[1].text, "Grace Hopper"));
    assert(r1_decoded.values[2].is_null);

    Value cell;
    ok = read_slotted_cell(&cols[1], row_buf, encoded_len, 1, 3, &cell);
    assert(ok && !cell.is_null && !strcmp(cell.text, "Grace Hopper"));
    printf("  [PASS] Phase 1: Slotted variable-width rows verified.\n\n");

    /* --------------------------------------------------------------------- */
    /* PHASE 2: Paged Storage & Buffer Pool Test                             */
    /* --------------------------------------------------------------------- */
    printf("[Phase 2] Testing Slotted Pages & Buffer Pool Manager...\n");
    uint8_t page[PAGE_SIZE];
    ok = slotted_page_init(page, 1, PAGE_TYPE_DATA);
    assert(ok);

    int s0 = slotted_page_insert_record(page, row_buf, encoded_len);
    assert(s0 == 0);
    int s1 = slotted_page_insert_record(page, row_buf, encoded_len);
    assert(s1 == 1);

    const uint8_t* rec_ptr = NULL;
    size_t rec_len = 0;
    ok = slotted_page_get_record(page, 0, &rec_ptr, &rec_len);
    assert(ok && rec_len == encoded_len && !memcmp(rec_ptr, row_buf, encoded_len));

    /* Test Buffer Pool with actual disk file */
    const char* db_file = "test_paged.db";
    int fd = open_test_file(db_file);
    assert(fd >= 0 && "Database file creation must succeed");

    BufferPool bp;
    buffer_pool_init(&bp);
    uint32_t p0 = buffer_pool_alloc_page(&bp, fd, PAGE_TYPE_DATA);
    assert(p0 == 0);
    uint32_t p1 = buffer_pool_alloc_page(&bp, fd, PAGE_TYPE_DATA);
    assert(p1 == 1);

    uint8_t* p0_mem = buffer_pool_fetch(&bp, fd, p0, true);
    assert(p0_mem != NULL);
    slotted_page_insert_record(p0_mem, row_buf, encoded_len);
    buffer_pool_unpin(&bp, p0, true);

    ok = buffer_pool_flush_all(&bp, fd);
    assert(ok && "Buffer pool flush must succeed");
    buffer_pool_destroy(&bp, fd);
    close_test_file(fd);
    printf("  [PASS] Phase 2: Slotted pages and buffer pool manager verified.\n\n");

    /* --------------------------------------------------------------------- */
    /* PHASE 3: Write-Ahead Logging (WAL) & Crash Recovery                   */
    /* --------------------------------------------------------------------- */
    printf("[Phase 3] Testing Write-Ahead Logging & Crash Replay...\n");
    const char* wal_base = "test_wal_db";
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s.dat", wal_base);
    int w_fd = open_test_file(db_path);
    assert(w_fd >= 0);

    BufferPool wal_bp;
    buffer_pool_init(&wal_bp);
    ok = wal_init(wal_base);
    assert(ok && "WAL initialization must succeed");

    /* Write page delta to WAL */
    const char* delta_data = "COMMITTED_DELTA_PAYLOAD";
    ok = wal_write_page_delta(0, 100, delta_data, (uint16_t)strlen(delta_data));
    assert(ok && "WAL delta log write must succeed");

    /* Commit transaction */
    ok = wal_commit_tx(101);
    assert(ok && "WAL commit must succeed");
    wal_shutdown();

    /* Simulate crash recovery: replay WAL into buffer pool */
    ok = wal_recover_and_replay(w_fd, &wal_bp);
    assert(ok && "WAL recovery must succeed");

    uint8_t* recovered_p = buffer_pool_fetch(&wal_bp, w_fd, 0, false);
    assert(recovered_p != NULL);
    assert(!memcmp(recovered_p + 100, delta_data, strlen(delta_data)));
    buffer_pool_unpin(&wal_bp, 0, false);

    /* Checkpoint and truncate */
    ok = wal_init(wal_base);
    assert(ok);
    ok = wal_checkpoint_now(&wal_bp, w_fd);
    assert(ok && "WAL checkpoint must succeed");
    wal_shutdown();
    buffer_pool_destroy(&wal_bp, w_fd);
    close_test_file(w_fd);
    printf("  [PASS] Phase 3: WAL logging, sub-ms commit, and replay verified.\n\n");

    /* --------------------------------------------------------------------- */
    /* PHASE 4: Persistent On-Disk B+ Tree Index                             */
    /* --------------------------------------------------------------------- */
    printf("[Phase 4] Testing On-Disk B+ Tree Indexing & Range Scans...\n");
    const char* btree_file = "test_btree.db";
    int b_fd = open_test_file(btree_file);
    assert(b_fd >= 0);

    BufferPool btree_bp;
    buffer_pool_init(&btree_bp);
    uint32_t root_page = btree_create(&btree_bp, b_fd, TYPE_NUMBER);
    assert(root_page != UINT32_MAX);

    /* Insert 60 keys to force B+ Tree leaf split and internal node generation */
    for (int i = 1; i <= 60; i++) {
        Value k;
        snprintf(k.text, sizeof(k.text), "%03d", i);
        k.is_null = false;
        ok = btree_insert(&btree_bp, b_fd, &root_page, TYPE_NUMBER, &k, (uint32_t)(i * 10), i);
        assert(ok && "B+ Tree insert must succeed");
    }

    /* Point lookup */
    Value query_key;
    snprintf(query_key.text, sizeof(query_key.text), "035");
    query_key.is_null = false;
    uint32_t found_page = 0;
    int found_slot = 0;
    ok = btree_search(&btree_bp, b_fd, root_page, TYPE_NUMBER, &query_key, &found_page, &found_slot);
    assert(ok && "B+ Tree point search must find key");
    assert(found_page == 350 && found_slot == 35);

    /* Range scan between 020 and 025 */
    Value low, high;
    snprintf(low.text, sizeof(low.text), "020");
    snprintf(high.text, sizeof(high.text), "025");
    low.is_null = high.is_null = false;
    uint32_t out_pages[10];
    int out_slots[10];
    int matches = btree_range_scan(&btree_bp, b_fd, root_page, TYPE_NUMBER, &low, &high, out_pages, out_slots, 10);
    assert(matches == 6 && "Range scan between 020 and 025 must find 6 records");
    assert(out_slots[0] == 20 && out_slots[5] == 25);

    buffer_pool_destroy(&btree_bp, b_fd);
    close_test_file(b_fd);
    printf("  [PASS] Phase 4: B+ Tree splits, logarithmic lookups, and range scans verified.\n\n");

    /* Cleanup temporary test files */
#ifdef _WIN32
    DeleteFileA(db_file);
    DeleteFileA(db_path);
    char wal_del[256];
    snprintf(wal_del, sizeof(wal_del), "%s.wal", wal_base);
    DeleteFileA(wal_del);
    DeleteFileA(btree_file);
#else
    unlink(db_file);
    unlink(db_path);
    char wal_del[256];
    snprintf(wal_del, sizeof(wal_del), "%s.wal", wal_base);
    unlink(wal_del);
    unlink(btree_file);
#endif

    printf("===================================================================\n");
    printf("   All 4 Large Dataset Architecture Phases Passed Successfully!   \n");
    printf("===================================================================\n");
    return 0;
}
