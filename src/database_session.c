#include "database_engine.h"

Database *g_db, *g_committed;
bool g_pending, g_timing, g_io_uncertain, g_readonly;
bool g_autocommit = false;
bool g_interactive_shell = false;
bool g_semicolon_optional = false;
char g_null_str[32] = "NULL";
int g_max_rows = 0;
FILE* g_spool_file = NULL;
char g_spool_path[PATH_CAP] = {0};
OutputFormat g_output_format = FORMAT_TABLE;
atomic_int g_errors;
uint64_t g_generation, g_identity;
char g_path[PATH_CAP], g_backup_dir[PATH_CAP];
char g_network_user[MAX_NAME_LEN];
bool g_network_tls_active;
char g_last_error[MAX_VAL_LEN];
unsigned g_backup_interval = 60;
atomic_bool g_stop, g_backup_failed;
atomic_bool g_restart_requested;
bool g_server_mode, g_cluster_active, g_hotpatch_mode;
atomic_ullong g_backup_generation, g_backup_time;
#ifdef _WIN32
static CRITICAL_SECTION g_io_lock;
/* Negative values mean "initializing"; zero means uninitialized; positive is
   the number of active users. The transition from initializing to usable is
   atomic so no caller can observe a half-initialized critical section. */
static atomic_int g_io_lock_ref = 0;
static SRWLOCK g_engine_state_lock = SRWLOCK_INIT;
void lock_snapshot_io(void) { EnterCriticalSection(&g_io_lock); }
void unlock_snapshot_io(void) { LeaveCriticalSection(&g_io_lock); }
void lock_engine_state(void) { AcquireSRWLockExclusive(&g_engine_state_lock); }
void unlock_engine_state(void) { ReleaseSRWLockExclusive(&g_engine_state_lock); }
#else
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_engine_state_lock = PTHREAD_MUTEX_INITIALIZER;
void lock_snapshot_io(void) { pthread_mutex_lock(&g_io_lock); }
void unlock_snapshot_io(void) { pthread_mutex_unlock(&g_io_lock); }
void lock_engine_state(void) { pthread_mutex_lock(&g_engine_state_lock); }
void unlock_engine_state(void) { pthread_mutex_unlock(&g_engine_state_lock); }
#endif

void initialize_snapshot_io(void) {
#ifdef _WIN32
    while (true) {
        int state = atomic_load(&g_io_lock_ref);
        if (state > 0) {
            /* Already initialized: add a reference. */
            if (atomic_compare_exchange_weak(&g_io_lock_ref, &state, state + 1)) return;
            continue;
        }
        if (state == 0 && atomic_compare_exchange_weak(&g_io_lock_ref, &state, -1)) {
            InitializeCriticalSection(&g_io_lock);
            atomic_store(&g_io_lock_ref, 1); /* publish: initialized, one user */
            return;
        }
        /* state == -1: another thread is initializing; wait for it. */
        Sleep(0);
    }
#endif
}

void shutdown_snapshot_io(void) {
#ifdef _WIN32
    while (true) {
        int state = atomic_load(&g_io_lock_ref);
        if (state <= 0) return; /* not initialized or mid-init; nothing to release */
        if (!atomic_compare_exchange_weak(&g_io_lock_ref, &state, state - 1)) continue;
        if (state == 1) {
            DeleteCriticalSection(&g_io_lock);
        }
        return;
    }
#endif
}

bool report_database_error(const char* format, ...) {
    va_list args;
    fputs("ERROR: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    if (g_spool_file) {
        va_list args2;
        fputs("ERROR: ", g_spool_file);
        va_start(args2, format);
        vfprintf(g_spool_file, format, args2);
        va_end(args2);
        fputc('\n', g_spool_file);
        fflush(g_spool_file);
    }
    /* Mirror to the user error report for post-mortem debugging. */
    {
        char buffer[2048];
        va_list args3;
        va_start(args3, format);
        vsnprintf(buffer, sizeof(buffer), format, args3);
        va_end(args3);
        log_error_report("%s", buffer);
        snprintf(g_last_error, sizeof(g_last_error), "%s", buffer);
    }
    g_errors++;
    return false;
}

double monotonic_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}
