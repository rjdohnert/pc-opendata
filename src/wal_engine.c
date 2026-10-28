#include "database_engine.h"

static FILE* g_wal_file = NULL;
static char g_wal_path[PATH_CAP] = {0};
static uint32_t g_current_lsn = 1;

static uint32_t wal_crc32_update(uint32_t crc, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

bool wal_init(const char* base_path) {
    if (!base_path || !*base_path) return false;
    if (snprintf(g_wal_path, sizeof(g_wal_path), "%s.wal", base_path) >= (int)sizeof(g_wal_path)) return false;
#ifdef _WIN32
    if (fopen_s(&g_wal_file, g_wal_path, "a+b") != 0) g_wal_file = NULL;
#else
    g_wal_file = fopen(g_wal_path, "a+b");
#endif
    return g_wal_file != NULL;
}

static bool flush_wal(void) {
    if (!g_wal_file) return false;
    if (fflush(g_wal_file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(g_wal_file)) == 0;
#else
    return fsync(fileno(g_wal_file)) == 0;
#endif
}

static uint32_t wal_record_crc32(const WalRecordHeader* hdr, const void* data, uint16_t len) {
    /* Cover the header fields (excluding the crc32 member itself) plus payload. */
    uint32_t crc = wal_crc32_update(UINT32_MAX, hdr, offsetof(WalRecordHeader, crc32));
    crc = wal_crc32_update(crc, data, len);
    return ~crc;
}

bool wal_write_page_delta(uint32_t page_id, uint16_t offset, const void* data, uint16_t len) {
    if (!g_wal_file || !data || len == 0) return false;
    WalRecordHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WAL_MAGIC;
    hdr.lsn = g_current_lsn++;
    hdr.op = WAL_OP_PAGE_DELTA;
    hdr.length = len;
    hdr.page_id = page_id;
    hdr.page_offset = offset;
    hdr.reserved = (uint32_t)time(NULL);
    hdr.crc32 = wal_record_crc32(&hdr, data, len);

    if (fwrite(&hdr, sizeof(hdr), 1, g_wal_file) != 1) return false;
    if (fwrite(data, 1, len, g_wal_file) != len) return false;
    return true;
}

bool wal_commit_tx(uint32_t tx_id) {
    if (!g_wal_file) return false;
    WalRecordHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WAL_MAGIC;
    hdr.lsn = g_current_lsn++;
    hdr.tx_id = tx_id;
    hdr.op = WAL_OP_COMMIT;
    hdr.length = 0;
    hdr.reserved = (uint32_t)time(NULL);
    hdr.crc32 = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, g_wal_file) != 1) return false;
    return flush_wal();
}

bool wal_checkpoint_now(BufferPool* bp, int db_fd) {
    if (!g_wal_file || db_fd < 0) return false;
    if (!buffer_pool_flush_all(bp, db_fd)) return false;
    /* Make the flushed pages durable before discarding the WAL that could
       otherwise redo them. */
#ifdef _WIN32
    if (_commit(db_fd) != 0) return false;
#else
    if (fsync(db_fd) != 0) return false;
#endif
    fclose(g_wal_file);
    g_wal_file = NULL;

    /* Truncate WAL file after dirty pages are durably flushed */
#ifdef _WIN32
    if (fopen_s(&g_wal_file, g_wal_path, "w+b") != 0) g_wal_file = NULL;
#else
    g_wal_file = fopen(g_wal_path, "w+b");
#endif
    return g_wal_file != NULL;
}

bool wal_recover_and_replay_to_timestamp(int db_fd, BufferPool* bp, uint32_t target_timestamp) {
    if (!g_wal_path[0] || db_fd < 0) return false;
    FILE* file = NULL;
#ifdef _WIN32
    if (fopen_s(&file, g_wal_path, "rb") != 0) return true; /* No WAL file to replay */
#else
    file = fopen(g_wal_path, "rb");
    if (!file) return true;
#endif
    WalRecordHeader hdr;
    uint8_t buffer[PAGE_SIZE];
    bool ok = true;

    while (fread(&hdr, sizeof(hdr), 1, file) == 1) {
        if (hdr.magic != WAL_MAGIC) { ok = false; break; }
        if (hdr.length > PAGE_SIZE) { ok = false; break; }
        if (target_timestamp > 0 && hdr.reserved > target_timestamp) {
            /* Point-in-time recovery limit reached */
            break;
        }
        if (hdr.length > 0) {
            if (fread(buffer, 1, hdr.length, file) != hdr.length) { ok = false; break; }
            if (wal_record_crc32(&hdr, buffer, hdr.length) != hdr.crc32) { ok = false; break; }
        }
        if (hdr.op == WAL_OP_PAGE_DELTA) {
            uint8_t* page = buffer_pool_fetch(bp, db_fd, hdr.page_id, true);
            if (!page) { ok = false; break; }
            if ((size_t)hdr.page_offset + hdr.length <= PAGE_SIZE) {
                memcpy(page + hdr.page_offset, buffer, hdr.length);
                buffer_pool_unpin(bp, hdr.page_id, true);
            } else {
                buffer_pool_unpin(bp, hdr.page_id, false);
            }
        }
    }
    fclose(file);
    if (ok) buffer_pool_flush_all(bp, db_fd);
    return ok;
}

bool wal_recover_and_replay(int db_fd, BufferPool* bp) {
    return wal_recover_and_replay_to_timestamp(db_fd, bp, 0);
}

void wal_shutdown(void) {
    if (g_wal_file) {
        flush_wal();
        fclose(g_wal_file);
        g_wal_file = NULL;
    }
}
