#include "database_engine.h"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

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
    g_wal_file = _fsopen(g_wal_path, "a+b", _SH_DENYNO);
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

bool wal_archive_segment(const char* archive_dir) {
    if (!archive_dir || !*archive_dir || !g_wal_path[0]) return false;
    if (g_wal_file) flush_wal();

    FILE* src = NULL;
#ifdef _WIN32
    src = _fsopen(g_wal_path, "rb", _SH_DENYNO);
    if (!src) return false;
#else
    src = fopen(g_wal_path, "rb");
    if (!src) return false;
#endif

    fseek(src, 0, SEEK_END);
    long sz = ftell(src);
    fseek(src, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(src);
        return true;
    }

    char arch_path[PATH_CAP];
    uint32_t now = (uint32_t)time(NULL);
    snprintf(arch_path, sizeof(arch_path), "%s/wal_%u.wal", archive_dir, now);

    FILE* dst = NULL;
#ifdef _WIN32
    if (fopen_s(&dst, arch_path, "wb") != 0) { fclose(src); return false; }
#else
    dst = fopen(arch_path, "wb");
    if (!dst) { fclose(src); return false; }
#endif

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            return false;
        }
    }
    fclose(src);
    fclose(dst);
    return true;
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
    if (g_backup_dir[0] != '\0') {
        wal_archive_segment(g_backup_dir);
    }
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

static int compare_wal_filenames(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    uint32_t ta = 0, tb = 0;
    const char* pa = strstr(sa, "wal_");
    const char* pb = strstr(sb, "wal_");
    if (pa) sscanf(pa, "wal_%u", &ta);
    if (pb) sscanf(pb, "wal_%u", &tb);
    if (ta != tb) return ta < tb ? -1 : 1;
    return strcmp(sa, sb);
}

bool wal_pitr_restore(const char* db_path, const char* archive_dir, uint32_t target_ts) {
    if (!db_path || !*db_path) return false;

    int db_fd = -1;
#ifdef _WIN32
    _sopen_s(&db_fd, db_path, _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
#else
    db_fd = open(db_path, O_RDWR);
#endif
    if (db_fd < 0) return false;

    BufferPool bp;
    buffer_pool_init(&bp);

    bool ok = true;
    if (archive_dir && *archive_dir) {
        char** seg_list = (char**)calloc(1024, sizeof(char*));
        if (!seg_list) {
            buffer_pool_destroy(&bp, db_fd);
#ifdef _WIN32
            _close(db_fd);
#else
            close(db_fd);
#endif
            return false;
        }
        int seg_count = 0;
#ifdef _WIN32
        char search_pattern[PATH_CAP];
        snprintf(search_pattern, sizeof(search_pattern), "%s\\wal_*.wal", archive_dir);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search_pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (seg_count < 1024) {
                    char seg_path[PATH_CAP];
                    snprintf(seg_path, sizeof(seg_path), "%s\\%s", archive_dir, fd.cFileName);
                    size_t slen = strlen(seg_path) + 1;
                    seg_list[seg_count] = (char*)malloc(slen);
                    if (seg_list[seg_count]) {
                        memcpy(seg_list[seg_count], seg_path, slen);
                        seg_count++;
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        DIR* d = opendir(archive_dir);
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, "wal_", 4) == 0 && strstr(de->d_name, ".wal")) {
                    if (seg_count < 1024) {
                        char seg_path[PATH_CAP];
                        snprintf(seg_path, sizeof(seg_path), "%s/%s", archive_dir, de->d_name);
                        size_t slen = strlen(seg_path) + 1;
                        seg_list[seg_count] = (char*)malloc(slen);
                        if (seg_list[seg_count]) {
                            memcpy(seg_list[seg_count], seg_path, slen);
                            seg_count++;
                        }
                    }
                }
            }
            closedir(d);
        }
#endif
        if (seg_count > 1) {
            qsort(seg_list, (size_t)seg_count, sizeof(char*), compare_wal_filenames);
        }
        for (int i = 0; i < seg_count; i++) {
            char old_wal[PATH_CAP];
            memcpy(old_wal, g_wal_path, sizeof(old_wal));
            snprintf(g_wal_path, sizeof(g_wal_path), "%s", seg_list[i]);
            wal_recover_and_replay_to_timestamp(db_fd, &bp, target_ts);
            memcpy(g_wal_path, old_wal, sizeof(old_wal));
            free(seg_list[i]);
        }
        free(seg_list);
    }

    if (g_wal_path[0]) {
        ok = wal_recover_and_replay_to_timestamp(db_fd, &bp, target_ts);
    }

    buffer_pool_flush_all(&bp, db_fd);
    buffer_pool_destroy(&bp, db_fd);
#ifdef _WIN32
    _close(db_fd);
#else
    close(db_fd);
#endif
    return ok;
}

void wal_shutdown(void) {
    if (g_wal_file) {
        flush_wal();
        fclose(g_wal_file);
        g_wal_file = NULL;
    }
}
