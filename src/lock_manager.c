#include "database_engine.h"

LockManager g_lock_manager = {0};

void lock_manager_init(void) {
    memset(&g_lock_manager, 0, sizeof(g_lock_manager));
}

bool lock_manager_detect_deadlock(int session_id, int target_session_id) {
    if (session_id <= 0 || target_session_id <= 0) return false;
    if (session_id == target_session_id) return true;

    int visited[MAX_CLIENT_SESSIONS + 1];
    memset(visited, 0, sizeof(visited));
    int curr = target_session_id;

    while (curr > 0 && curr <= MAX_CLIENT_SESSIONS) {
        if (curr == session_id) return true;
        if (visited[curr]) break;
        visited[curr] = 1;
        curr = g_lock_manager.wait_for[curr];
    }
    return false;
}

int lock_manager_get_holder(const char* table_name, LockMode mode) {
    if (!table_name || !*table_name) return 0;
    for (int i = 0; i < g_lock_manager.lock_count; i++) {
        TableLockEntry* e = &g_lock_manager.locks[i];
        if (strcmp(e->table_name, table_name) == 0 && e->row_id == 0) {
            if (mode == LOCK_MODE_EXCLUSIVE || e->mode == LOCK_MODE_EXCLUSIVE) return e->session_id;
        }
    }
    return 0;
}

bool lock_manager_has_conflict(int session_id, const char* table_name, LockMode mode) {
    if (!table_name || !*table_name) return false;
    for (int i = 0; i < g_lock_manager.lock_count; i++) {
        TableLockEntry* e = &g_lock_manager.locks[i];
        if (strcmp(e->table_name, table_name) == 0 && e->session_id != session_id) {
            if (mode == LOCK_MODE_EXCLUSIVE || e->mode == LOCK_MODE_EXCLUSIVE) {
                return true;
            }
        }
    }
    return false;
}

bool lock_manager_has_row_conflict(int session_id, const char* table_name, uint32_t row_id, LockMode mode) {
    if (!table_name || !*table_name) return false;
    for (int i = 0; i < g_lock_manager.lock_count; i++) {
        TableLockEntry* e = &g_lock_manager.locks[i];
        if (strcmp(e->table_name, table_name) == 0 && e->session_id != session_id) {
            if (e->row_id == 0 || e->row_id == row_id) {
                if (mode == LOCK_MODE_EXCLUSIVE || e->mode == LOCK_MODE_EXCLUSIVE) {
                    return true;
                }
            }
        }
    }
    return false;
}

static int lock_manager_get_row_holder(const char* table_name, uint32_t row_id, LockMode mode) {
    if (!table_name || !*table_name) return 0;
    for (int i = 0; i < g_lock_manager.lock_count; i++) {
        TableLockEntry* e = &g_lock_manager.locks[i];
        if (strcmp(e->table_name, table_name) == 0) {
            if (e->row_id == 0 || e->row_id == row_id) {
                if (mode == LOCK_MODE_EXCLUSIVE || e->mode == LOCK_MODE_EXCLUSIVE) {
                    return e->session_id;
                }
            }
        }
    }
    return 0;
}

bool lock_manager_acquire_row(int session_id, const char* table_name, uint32_t row_id, LockMode mode, uint32_t timeout_ms) {
    if (!table_name || !*table_name || mode == LOCK_MODE_NONE) return true;

    for (int i = 0; i < g_lock_manager.lock_count; i++) {
        TableLockEntry* e = &g_lock_manager.locks[i];
        if (strcmp(e->table_name, table_name) == 0 && e->session_id == session_id) {
            if (e->row_id == row_id || e->row_id == 0) {
                if (e->mode == mode || e->mode == LOCK_MODE_EXCLUSIVE) return true;
                if (mode == LOCK_MODE_EXCLUSIVE && !lock_manager_has_row_conflict(session_id, table_name, row_id, mode)) {
                    e->mode = LOCK_MODE_EXCLUSIVE;
                    return true;
                }
            }
        }
    }

    int holder = lock_manager_get_row_holder(table_name, row_id, mode);
    if (holder > 0 && holder != session_id) {
        if (lock_manager_detect_deadlock(session_id, holder)) {
            snprintf(g_last_error, sizeof(g_last_error), "ERROR 40P01: deadlock detected between session %d and session %d", session_id, holder);
            return false;
        }
        if (session_id > 0 && session_id <= MAX_CLIENT_SESSIONS) {
            g_lock_manager.wait_for[session_id] = holder;
        }
    }

    uint64_t start_ms = (uint64_t)(monotonic_seconds() * 1000.0);
    while (lock_manager_has_row_conflict(session_id, table_name, row_id, mode)) {
        int cur_holder = lock_manager_get_row_holder(table_name, row_id, mode);
        if (cur_holder > 0 && cur_holder != session_id) {
            if (lock_manager_detect_deadlock(session_id, cur_holder)) {
                if (session_id > 0 && session_id <= MAX_CLIENT_SESSIONS) {
                    g_lock_manager.wait_for[session_id] = 0;
                }
                snprintf(g_last_error, sizeof(g_last_error), "ERROR 40P01: deadlock detected between session %d and session %d", session_id, cur_holder);
                return false;
            }
            if (session_id > 0 && session_id <= MAX_CLIENT_SESSIONS) {
                g_lock_manager.wait_for[session_id] = cur_holder;
            }
        }

        uint64_t now_ms = (uint64_t)(monotonic_seconds() * 1000.0);
        if (now_ms - start_ms >= (uint64_t)timeout_ms) {
            if (session_id > 0 && session_id <= MAX_CLIENT_SESSIONS) {
                g_lock_manager.wait_for[session_id] = 0;
            }
            return false;
        }
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L };
        nanosleep(&ts, NULL);
#endif
    }

    if (session_id > 0 && session_id <= MAX_CLIENT_SESSIONS) {
        g_lock_manager.wait_for[session_id] = 0;
    }

    if (g_lock_manager.lock_count < MAX_TABLE_LOCKS) {
        TableLockEntry* entry = &g_lock_manager.locks[g_lock_manager.lock_count++];
        snprintf(entry->table_name, sizeof(entry->table_name), "%s", table_name);
        entry->row_id = row_id;
        entry->mode = mode;
        entry->session_id = session_id;
        entry->acquired_at_ms = (uint64_t)(monotonic_seconds() * 1000.0);
        return true;
    }
    return false;
}

bool lock_manager_acquire(int session_id, const char* table_name, LockMode mode, uint32_t timeout_ms) {
    return lock_manager_acquire_row(session_id, table_name, 0, mode, timeout_ms);
}

void lock_manager_release_session(int session_id) {
    if (session_id > 0 && session_id <= MAX_CLIENT_SESSIONS) {
        g_lock_manager.wait_for[session_id] = 0;
    }
    int w = 0;
    for (int r = 0; r < g_lock_manager.lock_count; r++) {
        if (g_lock_manager.locks[r].session_id != session_id) {
            if (w != r) g_lock_manager.locks[w] = g_lock_manager.locks[r];
            w++;
        }
    }
    g_lock_manager.lock_count = w;
}

void lock_manager_release_table(int session_id, const char* table_name) {
    if (!table_name) return;
    int w = 0;
    for (int r = 0; r < g_lock_manager.lock_count; r++) {
        if (g_lock_manager.locks[r].session_id == session_id &&
            strcmp(g_lock_manager.locks[r].table_name, table_name) == 0) {
            continue;
        }
        if (w != r) g_lock_manager.locks[w] = g_lock_manager.locks[r];
        w++;
    }
    g_lock_manager.lock_count = w;
}

void lock_manager_release_row(int session_id, const char* table_name, uint32_t row_id) {
    if (!table_name) return;
    int w = 0;
    for (int r = 0; r < g_lock_manager.lock_count; r++) {
        if (g_lock_manager.locks[r].session_id == session_id &&
            g_lock_manager.locks[r].row_id == row_id &&
            strcmp(g_lock_manager.locks[r].table_name, table_name) == 0) {
            continue;
        }
        if (w != r) g_lock_manager.locks[w] = g_lock_manager.locks[r];
        w++;
    }
    g_lock_manager.lock_count = w;
}
