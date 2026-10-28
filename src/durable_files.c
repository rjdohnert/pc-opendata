#include "database_engine.h"

#ifdef _WIN32
static HANDLE g_file_lock = INVALID_HANDLE_VALUE;
#else
static int g_file_lock = -1;
#endif

static FILE* open_snapshot_file(const char* path, const char* mode) {
#ifdef _WIN32
    FILE* file = NULL;
    if (fopen_s(&file, path, mode)) return NULL;
    return file;
#else
    return fopen(path, mode);
#endif
}

/* Create a brand-new file, failing if it already exists (O_EXCL semantics). */
static FILE* open_snapshot_file_exclusive(const char* path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    int fd = _open_osfhandle((intptr_t)h, _O_WRONLY);
    if (fd < 0) { CloseHandle(h); return NULL; }
    FILE* file = _fdopen(fd, "wb");
    if (!file) { _close(fd); return NULL; }
    return file;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return NULL;
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); return NULL; }
    return file;
#endif
}

bool read_snapshot_file(const char* path, Image* image) {
    memset(image, 0, sizeof(*image)); image->ok = true;
    FILE* file = open_snapshot_file(path, "rb");
    if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long length = ok ? ftell(file) : -1;
    if (length < 32 || (unsigned long)length > MAX_IMAGE) ok = false;
    if (ok && fseek(file, 0, SEEK_SET) != 0) ok = false;
    if (ok) {
        image->size = image->capacity = (size_t)length;
        image->data = malloc(image->size);
        if (!image->data || fread(image->data, 1, image->size, file) != image->size) ok = false;
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) { free(image->data); memset(image, 0, sizeof(*image)); }
    return ok;
}

static bool flush_snapshot_file(FILE* file) {
    if (fflush(file)) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

#ifndef _WIN32
static bool get_parent_directory(const char* path, char* parent) {
    size_t length = strlen(path);
    if (length >= PATH_CAP) return false;
    memcpy(parent, path, length + 1);
    char* last = NULL;
    for (char* p = parent; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) { memcpy(parent, ".", 2); return true; }
    if (last == parent) last[1] = '\0';
#ifdef _WIN32
    else if (last == parent + 2 && parent[1] == ':') last[1] = '\0';
#endif
    else *last = '\0';
    return true;
}
#endif

static bool flush_parent_directory(const char* path) {
#ifdef _WIN32
    (void)path;
    return true; /* MoveFileEx WRITE_THROUGH is used at publication. */
#else
    char parent[PATH_CAP];
    if (!get_parent_directory(path, parent)) return false;
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
#endif
}

bool snapshot_file_matches(const Image* expected, const char* path) {
    Image actual;
    if (!read_snapshot_file(path, &actual)) return false;
    bool ok = actual.size == expected->size && !memcmp(actual.data, expected->data, actual.size);
    free(actual.data); return ok;
}

#ifdef OPENDATA_TESTING
static void inject_commit_crash(const char* stage) {
    char value[64] = {0};
#ifdef _WIN32
    size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), "OPENDATA_TEST_CRASH") || !length) return;
#else
    const char* environment = getenv("OPENDATA_TEST_CRASH");
    if (!environment || strlen(environment) >= sizeof(value)) return;
    memcpy(value, environment, strlen(environment) + 1);
#endif
    if (!strcmp(value, stage)) _Exit(86);
}
#endif

/* Publication is the commit point. A failed directory sync makes the outcome uncertain. */
bool publish_durable_snapshot(const char* path, const Image* image, bool replace, bool* uncertain) {
    char temp[PATH_CAP];
    if (snprintf(temp, sizeof(temp), "%s.pending", path) >= (int)sizeof(temp)) return false;
    /* Create exclusively so a pre-planted .pending file cannot be swapped in.
       A stale .pending left by a crashed commit is safe to reclaim. */
    FILE* file = open_snapshot_file_exclusive(temp);
    if (!file) {
        remove(temp);
        file = open_snapshot_file_exclusive(temp);
    }
    if (!file) return false;
    bool ok = fwrite(image->data, 1, image->size, file) == image->size && flush_snapshot_file(file);
    if (fclose(file) != 0) ok = false;
    if (!ok || !snapshot_file_matches(image, temp)) {
        remove(temp);
        return false;
    }
#ifdef OPENDATA_TESTING
    if (replace) inject_commit_crash("before-publish");
#endif
#ifdef _WIN32
    DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0u);
    if (!MoveFileExA(temp, path, flags)) return false;
#else
    if (replace) {
        if (rename(temp, path)) return false;
    } else {
        if (link(temp, path)) return false;
        if (unlink(temp)) { *uncertain = true; return false; }
    }
#endif
    if (!flush_parent_directory(path)) { *uncertain = true; return false; }
#ifdef OPENDATA_TESTING
    if (replace) inject_commit_crash("after-publish");
#endif
    return true;
}

bool snapshot_path_exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st; return stat(path, &st) == 0;
#endif
}

bool acquire_database_file_lock(void) {
    char path[PATH_CAP];
    if (snprintf(path, sizeof(path), "%s.lock", g_path) >= (int)sizeof(path)) return false;
#ifdef _WIN32
    DWORD access = g_readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD share = g_readonly ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : 0;
    g_file_lock = CreateFileA(path, access, share, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    return g_file_lock != INVALID_HANDLE_VALUE;
#else
    g_file_lock = open(path, O_CREAT | (g_readonly ? O_RDONLY : O_RDWR), 0666);
    if (g_file_lock < 0) return false;
    int op = g_readonly ? (LOCK_SH | LOCK_NB) : (LOCK_EX | LOCK_NB);
    if (flock(g_file_lock, op) != 0) {
        close(g_file_lock);
        g_file_lock = -1;
        return false;
    }
    return true;
#endif
}

void release_database_file_lock(void) {
#ifdef _WIN32
    if (g_file_lock != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file_lock);
        g_file_lock = INVALID_HANDLE_VALUE;
    }
#else
    if (g_file_lock >= 0) {
        close(g_file_lock);
        g_file_lock = -1;
    }
#endif
}

#ifdef _WIN32
HANDLE get_database_file_lock(void) {
    return g_file_lock;
}
void set_database_file_lock(HANDLE h) {
    g_file_lock = h;
}
#else
int get_database_file_lock(void) {
    return g_file_lock;
}
void set_database_file_lock(int fd) {
    g_file_lock = fd;
}
#endif
