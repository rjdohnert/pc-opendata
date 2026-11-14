#include "database_engine.h"

#ifdef _WIN32
static HANDLE g_worker;
#else
static pthread_t g_worker;
static bool g_worker_valid;
#endif

typedef struct {
    char database_path[PATH_CAP];
    char backup_dir[PATH_CAP];
    unsigned interval;
    atomic_bool* stop_flag;
    atomic_bool* failed_flag;
} BackupWorkerArgs;

static atomic_bool* g_worker_stop_flag;

static bool backup_committed_snapshot_locked(const char* database_path, const char* backup_dir) {
    Image image;
    if (!read_snapshot_file(database_path, &image)) return false;
    uint64_t identity = 0, generation = 0;
    Database* check = decode_database_snapshot(&image, &identity, &generation);
    if (!check) { free(image.data); return false; }
    destroy_database(check);
    char path[PATH_CAP];
    int n = snprintf(path, sizeof(path), "%s%sopendata-%016llx-%020llu.pcod",
                     backup_dir, PATH_SEPARATOR, (unsigned long long)identity,
                     (unsigned long long)generation);
    bool ok = n > 0 && n < (int)sizeof(path);
    bool uncertain = false;
    if (ok) {
        if (snapshot_path_exists(path)) ok = snapshot_file_matches(&image, path);
        else ok = publish_durable_snapshot(path, &image, false, &uncertain);
    }
    if (ok && !uncertain) {
        atomic_store(&g_backup_generation, (unsigned long long)generation);
        atomic_store(&g_backup_time, (unsigned long long)time(NULL));
    }
    free(image.data);
    return ok && !uncertain;
}

bool backup_committed_snapshot(void) {
    if (g_readonly || g_backup_dir[0] == '\0') return true;
    lock_snapshot_io();
    bool ok = backup_committed_snapshot_locked(g_path, g_backup_dir);
    atomic_store(&g_backup_failed, !ok);
    unlock_snapshot_io();
    if (!ok) {
        g_errors++;
        fprintf(stderr, "ERROR: backup failed; writes are blocked until a backup succeeds. Check the remote destination.\n");
    }
    return ok;
}

#ifdef _WIN32
static DWORD WINAPI run_periodic_backups(LPVOID context) {
#else
static void* run_periodic_backups(void* context) {
#endif
    BackupWorkerArgs* args = context;
    double last = monotonic_seconds();
    while (!atomic_load(args->stop_flag)) {
#ifdef _WIN32
        Sleep(100);
#else
        struct timespec wait = {0, 100000000};
        nanosleep(&wait, NULL);
#endif
        if (monotonic_seconds() - last >= args->interval) {
            lock_snapshot_io();
            if (g_db) run_autovacuum_sweep(g_db);
            bool ok = backup_committed_snapshot_locked(args->database_path, args->backup_dir);
            atomic_store(args->failed_flag, !ok);
            unlock_snapshot_io();
            if (!ok) {
                g_errors++;
                fprintf(stderr, "ERROR: backup failed; writes are blocked until a backup succeeds. Check the remote destination.\n");
            }
            last = monotonic_seconds();
        }
    }
    free(args);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static bool start_periodic_backups_with_stop(atomic_bool* stop_flag, atomic_bool* failed_flag) {
    if (g_readonly || g_backup_dir[0] == '\0' || g_backup_interval == 0) return true;
    if (g_worker_stop_flag != NULL) {
        stop_periodic_backups();
    }
    BackupWorkerArgs* args = calloc(1, sizeof(*args));
    if (!args) return false;
    snprintf(args->database_path, sizeof(args->database_path), "%s", g_path);
    snprintf(args->backup_dir, sizeof(args->backup_dir), "%s", g_backup_dir);
    args->interval = g_backup_interval;
    args->stop_flag = stop_flag;
    args->failed_flag = failed_flag;
    atomic_store(stop_flag, false);
#ifdef _WIN32
    g_worker = CreateThread(NULL, 0, run_periodic_backups, args, 0, NULL);
    if (!g_worker) {
        free(args);
        return false;
    }
#else
    if (pthread_create(&g_worker, NULL, run_periodic_backups, args) != 0) {
        free(args);
        return false;
    }
    g_worker_valid = true;
#endif
    g_worker_stop_flag = stop_flag;
    return true;
}

bool start_periodic_backups(void) {
    return start_periodic_backups_with_stop(&g_stop, &g_backup_failed);
}

bool start_periodic_backups_for_context(atomic_bool* stop_flag, atomic_bool* failed_flag) {
    return stop_flag && failed_flag && start_periodic_backups_with_stop(stop_flag, failed_flag);
}

void stop_periodic_backups(void) {
    if (!g_worker_stop_flag) return;
    atomic_store(g_worker_stop_flag, true);
#ifdef _WIN32
    if (g_worker) {
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = NULL;
    }
#else
    /* pthread_t is opaque and may be a struct; track validity separately. */
    if (g_worker_valid) {
        pthread_join(g_worker, NULL);
        g_worker_valid = false;
    }
#endif
    g_worker_stop_flag = NULL;
}

atomic_bool* get_backup_worker_stop_flag(void) {
    return g_worker_stop_flag;
}

void set_backup_worker_stop_flag(atomic_bool* stop_flag) {
    g_worker_stop_flag = stop_flag;
}

void clear_backup_worker_handle(void) {
#ifdef _WIN32
    g_worker = NULL;
#else
    memset(&g_worker, 0, sizeof(g_worker));
    g_worker_valid = false;
#endif
}

#ifdef _WIN32
HANDLE get_backup_worker_handle(void) {
    return g_worker;
}
void set_backup_worker_handle(HANDLE h) {
    g_worker = h;
}
#else
pthread_t get_backup_worker_handle(void) {
    return g_worker;
}
void set_backup_worker_handle(pthread_t t) {
    g_worker = t;
    /* A zeroed handle from a fresh context means "no worker", not a joinable
       thread; only a real started worker marks the handle valid. */
    static const pthread_t zeroed;
    g_worker_valid = memcmp(&t, &zeroed, sizeof(pthread_t)) != 0;
}
#endif
