#include "database_engine.h"

struct OpendataStmt {
    char sql_template[MAX_QUERY_LEN];
    Value bound_params[16];
    bool is_bound[16];
    int param_count;

    bool executed;
    int current_row;
    QueryResultCapture capture;
};

bool opendata_open(const char* db_path, const char* backup_dir, unsigned backup_interval, const char* restore_path) {
    if (!db_path || !*db_path) return false;
    if (!resolve_absolute_path(db_path, g_path)) return false;
    if (backup_dir && *backup_dir) {
        if (!resolve_absolute_path(backup_dir, g_backup_dir)) return false;
        g_backup_interval = backup_interval;
    } else {
        g_backup_dir[0] = '\0';
        g_backup_interval = 0;
    }
    g_readonly = false;
    initialize_snapshot_io();
    if (!open_or_restore_database(restore_path)) {
        shutdown_snapshot_io();
        return false;
    }
    if (g_backup_interval > 0 && g_backup_dir[0] != '\0') {
        if (!start_periodic_backups()) {
            destroy_database(g_db); g_db = NULL;
            destroy_database(g_committed); g_committed = NULL;
            release_database_file_lock();
            shutdown_snapshot_io();
            return false;
        }
    }
    return true;
}

bool opendata_open_readonly(const char* db_path) {
    if (!db_path || !*db_path) return false;
    if (!resolve_absolute_path(db_path, g_path)) return false;
    g_readonly = true;
    g_backup_dir[0] = '\0';
    g_backup_interval = 0;
    initialize_snapshot_io();
    if (!open_or_restore_database(NULL)) {
        shutdown_snapshot_io();
        return false;
    }
    return true;
}

bool opendata_exec(const char* sql) {
    if (!sql) return false;
    bool quit = false;
    return execute_console_statement(sql, &quit);
}

bool opendata_commit(void) {
    return commit_transaction();
}

bool opendata_rollback(void) {
    return rollback_transaction();
}

bool opendata_savepoint(const char* name) {
    return execute_savepoint(name);
}

bool opendata_rollback_to(const char* name) {
    return execute_rollback_savepoint(name);
}

bool opendata_release_savepoint(const char* name) {
    return execute_release_savepoint(name);
}

bool opendata_backup(void) {
    return backup_committed_snapshot();
}

bool opendata_vacuum(const char* table_name) {
    return execute_vacuum_command(table_name, true, false);
}

uint64_t opendata_get_generation(void) {
    return g_generation;
}

bool opendata_has_pending_changes(void) {
    return g_pending;
}

int opendata_get_error_count(void) {
    return atomic_load(&g_errors);
}

void opendata_close(void) {
    if (!g_db && !g_committed) return;
    if (!g_readonly) {
        if (g_backup_interval > 0 && g_backup_dir[0] != '\0') {
            stop_periodic_backups();
        }
        backup_committed_snapshot();
    }
    if (g_pending) fprintf(stderr, "NOTICE: uncommitted transaction discarded at exit.\n");
    destroy_database(g_db); g_db = NULL;
    destroy_database(g_committed); g_committed = NULL;
    clear_all_savepoints();
    release_database_file_lock();
    shutdown_snapshot_io();
    g_readonly = false;
    g_path[0] = '\0';
    g_backup_dir[0] = '\0';
    g_backup_interval = 0;
    g_pending = false;
    g_generation = 0;
}

OpendataStmt* opendata_prepare(const char* sql) {
    if (!sql || strlen(sql) >= MAX_QUERY_LEN) return NULL;
    OpendataStmt* stmt = calloc(1, sizeof(*stmt));
    if (!stmt) return NULL;
    memcpy(stmt->sql_template, sql, strlen(sql) + 1);

    bool in_quote = false;
    for (const char* s = sql; *s; s++) {
        if (*s == '\'') in_quote = !in_quote;
        else if (!in_quote && *s == '?') {
            if (stmt->param_count >= 16) { free(stmt); return NULL; }
            stmt->param_count++;
        }
    }
    return stmt;
}

bool opendata_bind_int(OpendataStmt* stmt, int param_idx, int64_t val) {
    if (!stmt || param_idx < 1 || param_idx > 16) return false;
    int idx = param_idx - 1;
    snprintf(stmt->bound_params[idx].text, sizeof(stmt->bound_params[idx].text), "%lld", (long long)val);
    stmt->bound_params[idx].is_null = false;
    stmt->is_bound[idx] = true;
    return true;
}

bool opendata_bind_text(OpendataStmt* stmt, int param_idx, const char* str) {
    if (!stmt || param_idx < 1 || param_idx > 16 || !str) return false;
    int idx = param_idx - 1;
    snprintf(stmt->bound_params[idx].text, sizeof(stmt->bound_params[idx].text), "%s", str);
    stmt->bound_params[idx].is_null = false;
    stmt->is_bound[idx] = true;
    return true;
}

bool opendata_bind_null(OpendataStmt* stmt, int param_idx) {
    if (!stmt || param_idx < 1 || param_idx > 16) return false;
    int idx = param_idx - 1;
    stmt->bound_params[idx].is_null = true;
    stmt->bound_params[idx].text[0] = '\0';
    stmt->is_bound[idx] = true;
    return true;
}

bool opendata_step(OpendataStmt* stmt) {
    if (!stmt) return false;
    if (!stmt->executed) {
        char concrete_sql[MAX_QUERY_LEN * 2];
        size_t out_len = 0;
        int p_idx = 0;
        bool in_quote = false;

        for (const char* s = stmt->sql_template; *s; s++) {
            if (*s == '\'') {
                in_quote = !in_quote;
                if (out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = *s;
            } else if (!in_quote && *s == '?') {
                if (p_idx < stmt->param_count && stmt->is_bound[p_idx]) {
                    const Value* v = &stmt->bound_params[p_idx];
                    if (v->is_null) {
                        const char* n = "NULL";
                        while (*n && out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = *n++;
                    } else {
                        /* Only splice unquoted text that is a well-formed numeric literal.
                           A loose character allowlist would let sequences like "1--"
                           comment out the remainder of the statement. */
                        bool is_num = false;
                        {
                            char* end = NULL;
                            (void)strtold(v->text, &end);
                            if (end && end != v->text && *end == '\0') {
                                bool syntax_ok = true;
                                for (const char* c = v->text; *c; c++) {
                                    if (!is_ascii_digit(*c) && *c != '-' && *c != '+' && *c != '.') { syntax_ok = false; break; }
                                }
                                is_num = syntax_ok;
                            }
                        }
                        if (is_num && v->text[0] != '\0') {
                            const char* num = v->text;
                            while (*num && out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = *num++;
                        } else {
                            if (out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = '\'';
                            for (const char* c = v->text; *c && out_len + 2 < sizeof(concrete_sql); c++) {
                                if (*c == '\'') concrete_sql[out_len++] = '\'';
                                concrete_sql[out_len++] = *c;
                            }
                            if (out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = '\'';
                        }
                    }
                } else {
                    const char* n = "NULL";
                    while (*n && out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = *n++;
                }
                p_idx++;
            } else {
                if (out_len + 1 < sizeof(concrete_sql)) concrete_sql[out_len++] = *s;
            }
        }
        concrete_sql[out_len] = '\0';

        free(stmt->capture.rows);
        memset(&stmt->capture, 0, sizeof(stmt->capture));
        stmt->capture.silent = true;
        QueryResultCapture* old_cap = g_result_capture;
        g_result_capture = &stmt->capture;

        bool quit = false;
        bool ok = execute_console_statement(concrete_sql, &quit);
        g_result_capture = old_cap;

        stmt->executed = true;
        stmt->current_row = -1;
        if (!ok) return false;
    }

    stmt->current_row++;
    return stmt->current_row < stmt->capture.row_count;
}

int opendata_column_count(OpendataStmt* stmt) {
    return stmt ? stmt->capture.col_count : 0;
}

const char* opendata_column_name(OpendataStmt* stmt, int col_idx) {
    if (!stmt || col_idx < 0 || col_idx >= stmt->capture.col_count) return NULL;
    return stmt->capture.col_names[col_idx];
}

const char* opendata_column_text(OpendataStmt* stmt, int col_idx) {
    if (!stmt || stmt->current_row < 0 || stmt->current_row >= stmt->capture.row_count ||
        col_idx < 0 || col_idx >= stmt->capture.col_count) return NULL;
    return stmt->capture.rows[stmt->current_row][col_idx].text;
}

bool opendata_column_is_null(OpendataStmt* stmt, int col_idx) {
    if (!stmt || stmt->current_row < 0 || stmt->current_row >= stmt->capture.row_count ||
        col_idx < 0 || col_idx >= stmt->capture.col_count) return true;
    return stmt->capture.rows[stmt->current_row][col_idx].is_null;
}

bool opendata_reset(OpendataStmt* stmt) {
    if (!stmt) return false;
    free(stmt->capture.rows);
    memset(&stmt->capture, 0, sizeof(stmt->capture));
    stmt->executed = false;
    stmt->current_row = 0;
    return true;
}

void opendata_finalize(OpendataStmt* stmt) {
    if (stmt) {
        free(stmt->capture.rows);
        free(stmt);
    }
}

bool opendata_export_csv(const char* table_name, const char* filepath) {
    return export_table_csv(g_db, table_name, filepath);
}

bool opendata_import_csv(const char* table_name, const char* filepath, bool has_header) {
    return import_table_csv(g_db, table_name, filepath, has_header);
}

bool opendata_export_json(const char* table_name, const char* filepath) {
    return export_table_json(g_db, table_name, filepath);
}

bool opendata_import_json(const char* table_name, const char* filepath) {
    return import_table_json(g_db, table_name, filepath);
}

bool opendata_export_mssql(const char* table_name, const char* filepath) {
    return export_database_mssql(g_db, table_name, filepath);
}

bool opendata_import_mssql(const char* table_name, const char* filepath) {
    return import_database_mssql(g_db, table_name, filepath);
}

bool opendata_export_oracle(const char* table_name, const char* filepath) {
    return export_database_oracle(g_db, table_name, filepath);
}

bool opendata_import_oracle(const char* table_name, const char* filepath) {
    return import_database_oracle(g_db, table_name, filepath);
}

bool opendata_extract_json(const char* json, const char* path, char* out_val, size_t out_cap) {
    return extract_json_value(json, path, out_val, out_cap);
}

void opendata_set_format(const char* format) {
    if (!format) return;
    if (!strcmp(format, "csv") || !strcmp(format, "CSV")) g_output_format = FORMAT_CSV;
    else if (!strcmp(format, "json") || !strcmp(format, "JSON")) g_output_format = FORMAT_JSON;
    else g_output_format = FORMAT_TABLE;
}

struct OpendataContext {
    Database *db;
    Database *committed;
    bool pending;
    bool timing;
    bool io_uncertain;
    bool readonly;
    OutputFormat output_format;
    char path[PATH_CAP];
    char backup_dir[PATH_CAP];
    unsigned backup_interval;
    uint64_t generation;
    uint64_t identity;
#ifdef _WIN32
    HANDLE file_lock;
    HANDLE backup_worker;
#else
    int file_lock;
    pthread_t backup_worker;
#endif
    atomic_bool backup_stop;
    atomic_bool backup_failed;
    atomic_bool* backup_worker_stop;
};

static void context_save(OpendataCtx* dest) {
    if (!dest) return;
    dest->db = g_db;
    dest->committed = g_committed;
    dest->pending = g_pending;
    dest->timing = g_timing;
    dest->io_uncertain = g_io_uncertain;
    dest->readonly = g_readonly;
    dest->output_format = g_output_format;
    dest->generation = g_generation;
    dest->identity = g_identity;
    memcpy(dest->path, g_path, sizeof(g_path));
    memcpy(dest->backup_dir, g_backup_dir, sizeof(g_backup_dir));
    dest->backup_interval = g_backup_interval;
    atomic_store(&dest->backup_failed, atomic_load(&g_backup_failed));
    dest->file_lock = get_database_file_lock();
    dest->backup_worker = get_backup_worker_handle();
    dest->backup_worker_stop = get_backup_worker_stop_flag();
}

static void context_load(const OpendataCtx* src) {
    if (!src) return;
    g_db = src->db;
    g_committed = src->committed;
    g_pending = src->pending;
    g_timing = src->timing;
    g_io_uncertain = src->io_uncertain;
    g_readonly = src->readonly;
    g_output_format = src->output_format;
    g_generation = src->generation;
    g_identity = src->identity;
    memcpy(g_path, src->path, sizeof(g_path));
    memcpy(g_backup_dir, src->backup_dir, sizeof(g_backup_dir));
    g_backup_interval = src->backup_interval;
    atomic_store(&g_backup_failed, atomic_load(&src->backup_failed));
    set_database_file_lock(src->file_lock);
    set_backup_worker_handle(src->backup_worker);
    set_backup_worker_stop_flag(src->backup_worker_stop);
}

OpendataCtx* opendata_ctx_create(void) {
    OpendataCtx* ctx = calloc(1, sizeof(OpendataCtx));
    if (ctx) {
        ctx->backup_interval = 60;
        atomic_init(&ctx->backup_stop, false);
        atomic_init(&ctx->backup_failed, false);
#ifdef _WIN32
        ctx->file_lock = INVALID_HANDLE_VALUE;
        ctx->backup_worker = NULL;
#else
        ctx->file_lock = -1;
#endif
    }
    return ctx;
}

bool opendata_ctx_open(OpendataCtx* ctx, const char* db_path, const char* backup_dir, unsigned backup_interval, const char* restore_path) {
    if (!ctx || ctx->db || ctx->committed) return false;
    OpendataCtx saved = {0};
    context_save(&saved);
    clear_backup_worker_handle();
    set_backup_worker_stop_flag(NULL);
    bool ok = opendata_open(db_path, backup_dir, 0, restore_path);
    if (ok) {
        g_backup_interval = backup_interval;
        context_save(ctx);
        if (g_backup_interval > 0 && g_backup_dir[0] != '\0') {
            if (!start_periodic_backups_for_context(&ctx->backup_stop, &ctx->backup_failed)) {
                g_backup_interval = 0;
                opendata_close();
                context_save(ctx);
                ok = false;
            } else {
                context_save(ctx);
            }
        }
    }
    context_load(&saved);
    return ok;
}

bool opendata_ctx_exec(OpendataCtx* ctx, const char* sql) {
    if (!ctx) return false;
    OpendataCtx saved = {0};
    context_save(&saved);
    context_load(ctx);
    bool ok = opendata_exec(sql);
    context_save(ctx);
    context_load(&saved);
    return ok;
}

bool opendata_ctx_commit(OpendataCtx* ctx) {
    if (!ctx) return false;
    OpendataCtx saved = {0};
    context_save(&saved);
    context_load(ctx);
    bool ok = opendata_commit();
    context_save(ctx);
    context_load(&saved);
    return ok;
}

bool opendata_ctx_rollback(OpendataCtx* ctx) {
    if (!ctx) return false;
    OpendataCtx saved = {0};
    context_save(&saved);
    context_load(ctx);
    bool ok = opendata_rollback();
    context_save(ctx);
    context_load(&saved);
    return ok;
}

void opendata_ctx_close(OpendataCtx* ctx) {
    if (!ctx || (!ctx->db && !ctx->committed)) return;
    OpendataCtx saved = {0};
    context_save(&saved);
    context_load(ctx);
    opendata_close();
    context_save(ctx);
    context_load(&saved);
}

void opendata_ctx_destroy(OpendataCtx* ctx) {
    if (!ctx) return;
    opendata_ctx_close(ctx);
    free(ctx);
}
