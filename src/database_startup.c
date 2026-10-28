#include "database_engine.h"

bool resolve_absolute_path(const char* input, char* output) {
#ifdef _WIN32
    DWORD length = GetFullPathNameA(input, PATH_CAP, output, NULL);
    return length > 0 && length < PATH_CAP;
#else
    if (input[0] == '/') return snprintf(output, PATH_CAP, "%s", input) < PATH_CAP;
    char cwd[PATH_CAP];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    return snprintf(output, PATH_CAP, "%s/%s", cwd, input) < PATH_CAP;
#endif
}

bool open_or_restore_database(const char* restore) {
    if (g_hotpatch_mode) {
        /* The predecessor releases the database lock during its exit teardown,
           moments after handing off the listener; wait for it here. */
        double deadline = monotonic_seconds() + 30.0;
        while (!acquire_database_file_lock()) {
            if (monotonic_seconds() >= deadline)
                return report_database_error("timed out waiting for the predecessor to release the database lock");
#ifdef _WIN32
            Sleep(50);
#else
            struct timespec pause = { 0, 50 * 1000 * 1000 };
            nanosleep(&pause, NULL);
#endif
        }
    } else if (!acquire_database_file_lock()) return report_database_error("cannot acquire database lock (already open or inaccessible)");
    if (restore && snapshot_path_exists(g_path)) { release_database_file_lock(); return report_database_error("restore requires a NEW database path; existing data will not be overwritten"); }
    if (restore) {
        Image image;
        if (!read_snapshot_file(restore, &image)) { release_database_file_lock(); return report_database_error("cannot read restore image"); }
        g_committed = decode_database_snapshot(&image, &g_identity, &g_generation);
        bool uncertain = false;
        bool ok = g_committed && publish_durable_snapshot(g_path, &image, false, &uncertain);
        free(image.data);
        if (!ok) {
            if (g_committed) { destroy_database(g_committed); g_committed = NULL; }
            release_database_file_lock();
            return report_database_error("restore validation/publication failed");
        }
    } else if (snapshot_path_exists(g_path)) {
        Image image;
        if (!read_snapshot_file(g_path, &image)) { release_database_file_lock(); return report_database_error("cannot read database; refusing to initialize over existing data"); }
        g_committed = decode_database_snapshot(&image, &g_identity, &g_generation);
        free(image.data);
        if (!g_committed) { release_database_file_lock(); return report_database_error("database corrupt, incompatible or invalid; restore a verified backup to a NEW path"); }
    } else {
        g_committed = calloc(1, sizeof(*g_committed));
        if (!g_committed) { release_database_file_lock(); return report_database_error("out of memory"); }
#ifdef _WIN32
        FILETIME now; GetSystemTimeAsFileTime(&now);
        g_identity = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
        g_identity ^= (uint64_t)GetCurrentProcessId() << 32;
#else
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        g_identity = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
        g_identity ^= (uint64_t)getpid() << 32;
#endif
        if (!g_identity) g_identity = 1;
        Image image;
        bool ok = encode_database_snapshot(g_committed, g_identity, 0, &image);
        bool uncertain = false;
        if (ok) ok = publish_durable_snapshot(g_path, &image, false, &uncertain);
        free(image.data);
        if (!ok) {
            destroy_database(g_committed); g_committed = NULL;
            release_database_file_lock();
            return report_database_error("cannot durably initialize database");
        }
    }
    g_db = copy_database(g_committed);
    if (!g_db) {
        destroy_database(g_committed); g_committed = NULL;
        release_database_file_lock();
        return report_database_error("cannot allocate working database");
    }
    if (!backup_committed_snapshot()) {
        destroy_database(g_db); g_db = NULL;
        destroy_database(g_committed); g_committed = NULL;
        release_database_file_lock();
        return false;
    }
    return true;
}

