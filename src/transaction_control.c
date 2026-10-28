#include "database_engine.h"

#define MAX_SAVEPOINTS 32
typedef struct {
    char name[MAX_NAME_LEN];
    Database* snapshot;
} Savepoint;

static Savepoint g_savepoints[MAX_SAVEPOINTS];
static int g_savepoint_count = 0;

void clear_all_savepoints(void) {
    for (int i = 0; i < g_savepoint_count; i++) {
        destroy_database(g_savepoints[i].snapshot);
        g_savepoints[i].snapshot = NULL;
    }
    g_savepoint_count = 0;
}

bool execute_savepoint(const char* name) {
    if (!g_db) return report_database_error("not connected to any database");
    if (g_readonly) return report_database_error("cannot create savepoint in read-only mode");
    if (!name || !name[0]) return report_database_error("savepoint name required");

    int idx = -1;
    for (int i = 0; i < g_savepoint_count; i++) {
        if (!strcmp(g_savepoints[i].name, name)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (g_savepoint_count >= MAX_SAVEPOINTS) {
            return report_database_error("savepoint limit reached (%d)", MAX_SAVEPOINTS);
        }
        idx = g_savepoint_count;
    }
    /* Allocate the replacement before destroying the old snapshot so an OOM
       cannot leave a dangling pointer behind. */
    Database* copy = copy_database(g_db);
    if (!copy) return report_database_error("cannot allocate savepoint snapshot");
    if (idx == g_savepoint_count) g_savepoint_count++;
    destroy_database(g_savepoints[idx].snapshot);
    snprintf(g_savepoints[idx].name, sizeof(g_savepoints[idx].name), "%s", name);
    g_savepoints[idx].snapshot = copy;
    printf("Savepoint %s created.\n", name);
    return true;
}

bool execute_rollback_savepoint(const char* name) {
    if (!g_db) return report_database_error("not connected to any database");
    if (g_readonly) return report_database_error("cannot rollback in read-only mode");
    if (!name || !name[0]) return report_database_error("savepoint name required");

    int idx = -1;
    for (int i = g_savepoint_count - 1; i >= 0; i--) {
        if (!strcmp(g_savepoints[i].name, name)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return report_database_error("savepoint does not exist: %s", name);

    Database* restored = copy_database(g_savepoints[idx].snapshot);
    if (!restored) return report_database_error("cannot allocate rollback snapshot");
    destroy_database(g_db);
    g_db = restored;

    for (int i = idx + 1; i < g_savepoint_count; i++) {
        destroy_database(g_savepoints[i].snapshot);
        g_savepoints[i].snapshot = NULL;
    }
    g_savepoint_count = idx + 1;
    printf("Rollback to savepoint %s complete.\n", name);
    return true;
}

bool execute_release_savepoint(const char* name) {
    if (!name || !name[0]) return report_database_error("savepoint name required");
    int idx = -1;
    for (int i = g_savepoint_count - 1; i >= 0; i--) {
        if (!strcmp(g_savepoints[i].name, name)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return report_database_error("savepoint does not exist: %s", name);

    for (int i = idx; i < g_savepoint_count; i++) {
        destroy_database(g_savepoints[i].snapshot);
        g_savepoints[i].snapshot = NULL;
    }
    g_savepoint_count = idx;
    printf("Savepoint %s released.\n", name);
    return true;
}

bool commit_transaction(void) {
    if (g_raft_role == RAFT_FOLLOWER) return report_database_error("follower nodes cannot commit transactions");
    if (g_readonly) return report_database_error("cannot commit in read-only mode");
    if (g_io_uncertain) return report_database_error("commit outcome uncertain; restart and inspect the database before writing");
    if (atomic_load(&g_backup_failed)) return report_database_error("backup unhealthy; run BACKUP before committing");
    if (!g_pending) { puts("Commit complete. No pending changes."); return true; }
    if (g_generation == UINT64_MAX) return report_database_error("generation exhausted");
    Database* committed = copy_database(g_db);
    if (!committed) return report_database_error("cannot allocate committed state");
    Image image, previous_image;
    if (!encode_database_snapshot(g_db, g_identity, g_generation + 1, &image)) {
        free(image.data); destroy_database(committed); return report_database_error("snapshot allocation or size limit failed");
    }
    memset(&previous_image, 0, sizeof(previous_image));
    if (!encode_database_snapshot(g_committed, g_identity, g_generation, &previous_image)) {
        free(image.data);
        free(previous_image.data);
        destroy_database(committed);
        return report_database_error("cannot preserve previous committed snapshot");
    }
    lock_snapshot_io();
    bool ok = !atomic_load(&g_backup_failed) &&
              publish_durable_snapshot(g_path, &image, true, &g_io_uncertain);
    if (ok && g_sync_replication) {
        ok = raft_ship_wal_sync(image.data, image.size);
        if (!ok) {
            bool rollback_uncertain = false;
            bool restored = publish_durable_snapshot(g_path, &previous_image, true, &rollback_uncertain);
            if (!restored || rollback_uncertain) g_io_uncertain = true;
        }
    }
    unlock_snapshot_io();
    free(image.data);
    free(previous_image.data);
    if (!ok) {
        destroy_database(committed);
        return report_database_error(g_io_uncertain ? "commit outcome uncertain; restart required; no success acknowledged" :
                      "commit failed; previous committed snapshot preserved, transaction remains pending");
    }
    destroy_database(g_committed); g_committed = committed;
    g_generation++; g_pending = false;
    clear_all_savepoints();
    printf("Commit complete. Durable generation %llu.\n", (unsigned long long)g_generation);
    return true;
}

bool rollback_transaction(void) {
    if (g_readonly) return report_database_error("cannot rollback in read-only mode");
    if (g_io_uncertain) return report_database_error("commit outcome uncertain; restart required");
    if (!g_committed) return report_database_error("not connected to any database");
    Database* restored = copy_database(g_committed);
    if (!restored) return report_database_error("cannot allocate rollback state");
    destroy_database(g_db); g_db = restored; g_pending = false;
    clear_all_savepoints();
    puts("Rollback complete.");
    return true;
}

bool execute_atomic_mutation(Parser* p, const char* command) {
    if (g_readonly) return report_database_error("database opened in read-only mode; mutations not allowed");
    if (g_io_uncertain || atomic_load(&g_backup_failed)) return report_database_error("writes blocked by storage/backup failure");
    Database* candidate = copy_database(g_db);
    if (!candidate) return report_database_error("unable to allocate statement snapshot");
    int before_errors = g_errors, affected = 0;
    bool ok = false;
    bool is_index_create = false;
    bool is_index_drop = false;

    if (!strcmp(command, "CREATE")) {
        if (token_is_keyword(p, "INDEX") || token_is_keyword(p, "UNIQUE")) {
            is_index_create = true;
            bool unique = consume_keyword(p, "UNIQUE");
            char idx_name[MAX_NAME_LEN], tbl_name[MAX_NAME_LEN], col_name[MAX_NAME_LEN];
            if (!consume_keyword(p, "INDEX") ||
                !parse_identifier(p, idx_name) || !consume_keyword(p, "ON") ||
                !parse_identifier(p, tbl_name) || !consume_token(p, TK_LP) ||
                !parse_identifier(p, col_name) || !consume_token(p, TK_RP) ||
                !consume_statement_end(p)) {
                ok = false;
            } else {
                ok = execute_create_index_in_db(candidate, idx_name, tbl_name, col_name, unique);
            }
        } else {
            ok = execute_create_table(p, candidate);
        }
    } else if (!strcmp(command, "DROP")) {
        if (token_is_keyword(p, "INDEX")) {
            is_index_drop = true;
            consume_keyword(p, "INDEX");
            char idx_name[MAX_NAME_LEN];
            if (!parse_identifier(p, idx_name) || !consume_statement_end(p)) {
                ok = false;
            } else {
                ok = execute_drop_index_in_db(candidate, idx_name);
            }
        } else {
            ok = execute_drop_table(p, candidate);
        }
    } else if (!strcmp(command, "ALTER")) ok = execute_alter_table(p, candidate);
    else if (!strcmp(command, "TRUNCATE")) ok = execute_truncate_table(p, candidate);
    else if (!strcmp(command, "INSERT")) ok = execute_insert_row(p, candidate, &affected);
    else ok = execute_update_or_delete(p, candidate, !strcmp(command, "DELETE"), &affected);

    if (ok) ok = validate_relational_integrity(candidate);
    if (!ok) {
        destroy_database(candidate);
        if (g_errors == before_errors) report_database_error("invalid or unsupported %s syntax/value; statement unchanged", command);
        return false;
    }
    destroy_database(g_db); g_db = candidate; g_pending = true;
    if (is_index_create) puts("Index created.");
    else if (is_index_drop) puts("Index dropped.");
    else if (!strcmp(command, "CREATE")) puts("Table created.");
    else if (!strcmp(command, "DROP")) puts("Table dropped.");
    else if (!strcmp(command, "ALTER")) puts("Table altered.");
    else if (!strcmp(command, "TRUNCATE")) puts("Table truncated.");
    else if (!strcmp(command, "INSERT")) printf("%d row%s created.\n", affected, affected == 1 ? "" : "s");
    else printf("%d row%s %s.\n", affected, affected == 1 ? "" : "s",
                !strcmp(command, "DELETE") ? "deleted" : "updated");
    if (g_autocommit) return commit_transaction();
    return true;
}
