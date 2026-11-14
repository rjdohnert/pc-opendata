/*
 * PC/OpenData - Embedded Relational Database Engine
 * Zero external dependencies. C11 compatible.
 */

#ifndef OPENDATA_H
#define OPENDATA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the database engine, open or create the database file at db_path,
 * configure off-machine backup publication at backup_dir, and start the periodic
 * backup worker thread.
 *
 * If restore_path is non-NULL, validates and restores the specified backup snapshot
 * to db_path (which must not already exist).
 *
 * Returns true if the database was opened and ready for commands.
 */
bool opendata_open(const char* db_path, const char* backup_dir, unsigned backup_interval, const char* restore_path);

/*
 * Execute a single SQL statement or console command (e.g. CREATE, INSERT, SELECT,
 * UPDATE, DELETE, COMMIT, ROLLBACK, STATUS, BACKUP, HELP, DESCRIBE).
 *
 * Results and console messages are written to stdout; errors to stderr.
 * Returns true on statement success, false on failure.
 */
bool opendata_exec(const char* sql);

/*
 * Durably commit pending transaction changes to disk.
 * Atomically replaces the database file with CRC32 verification and directory sync.
 * Returns true if the commit succeeded.
 */
bool opendata_commit(void);

/*
 * Rollback the current uncommitted transaction, discarding pending changes
 * and restoring the database to the last committed snapshot state.
 */
bool opendata_rollback(void);

/*
 * Savepoint and partial rollback support
 */
bool opendata_savepoint(const char* name);
bool opendata_rollback_to(const char* name);
bool opendata_release_savepoint(const char* name);

/*
 * Force an immediate verified committed-state backup publication to backup_dir.
 */
bool opendata_backup(void);

/*
 * Run Multi-Version Garbage Collection (VACUUM) on a specific table (or all tables if table_name is NULL).
 */
bool opendata_vacuum(const char* table_name);

/*
 * Return the current durable database generation number.
 */
uint64_t opendata_get_generation(void);

/*
 * Check whether uncommitted modifications are currently pending.
 */
bool opendata_has_pending_changes(void);

/*
 * Return the cumulative count of errors encountered since startup.
 */
int opendata_get_error_count(void);
const char* opendata_errmsg(void);

/*
 * Open the database in read-only mode for Single-Writer / Multi-Reader (SWMR) concurrency.
 * Acquires a shared lock allowing concurrent reader processes to inspect data
 * without write conflicts or requiring backup directories.
 */
bool opendata_open_readonly(const char* db_path);

/*
 * Prepared Statements API:
 * Parameterized queries with positional '?' placeholders.
 */
typedef struct OpendataStmt OpendataStmt;

OpendataStmt* opendata_prepare(const char* sql);
bool opendata_bind_int(OpendataStmt* stmt, int param_idx, int64_t val);
bool opendata_bind_double(OpendataStmt* stmt, int param_idx, double val);
bool opendata_bind_text(OpendataStmt* stmt, int param_idx, const char* str);
bool opendata_bind_vector(OpendataStmt* stmt, int param_idx, const float* vec, int dim);
bool opendata_bind_null(OpendataStmt* stmt, int param_idx);
bool opendata_step(OpendataStmt* stmt);
int opendata_column_count(OpendataStmt* stmt);
const char* opendata_column_name(OpendataStmt* stmt, int col_idx);
const char* opendata_column_text(OpendataStmt* stmt, int col_idx);
int64_t opendata_column_int64(OpendataStmt* stmt, int col_idx);
double opendata_column_double(OpendataStmt* stmt, int col_idx);
int opendata_column_vector(OpendataStmt* stmt, int col_idx, float* out_vec, int max_dim);
bool opendata_column_is_null(OpendataStmt* stmt, int col_idx);
bool opendata_reset(OpendataStmt* stmt);
void opendata_finalize(OpendataStmt* stmt);

/*
 * JSON and CSV Data Interchange API
 */
bool opendata_export_csv(const char* table_name, const char* filepath);
bool opendata_import_csv(const char* table_name, const char* filepath, bool has_header);
bool opendata_export_json(const char* table_name, const char* filepath);
bool opendata_import_json(const char* table_name, const char* filepath);
bool opendata_export_mssql(const char* table_name, const char* filepath);
bool opendata_import_mssql(const char* table_name, const char* filepath);
bool opendata_export_oracle(const char* table_name, const char* filepath);
bool opendata_import_oracle(const char* table_name, const char* filepath);
bool opendata_extract_json(const char* json, const char* path, char* out_val, size_t out_cap);
void opendata_set_format(const char* format);

/*
 * Multi-Instance Engine Context API:
 * Allows multiple isolated database instances within the same process.
 */
typedef struct OpendataContext OpendataContext;
typedef struct OpendataContext OpendataCtx;

OpendataCtx* opendata_ctx_create(void);
bool opendata_ctx_open(OpendataCtx* ctx, const char* db_path, const char* backup_dir, unsigned backup_interval, const char* restore_path);
bool opendata_ctx_exec(OpendataCtx* ctx, const char* sql);
bool opendata_ctx_commit(OpendataCtx* ctx);
bool opendata_ctx_rollback(OpendataCtx* ctx);
void opendata_ctx_close(OpendataCtx* ctx);
void opendata_ctx_destroy(OpendataCtx* ctx);
const char* opendata_ctx_errmsg(OpendataCtx* ctx);

/*
 * Stop periodic backup worker, publish clean shutdown backup, release database lock,
 * and destroy all in-memory database copies.
 */
void opendata_close(void);

/*
 * IRIS Internal Text Editor API (GNU Nano-Styled SQL & Script Editor)
 */
#ifndef IRIS_EDITOR_TYPES_DEFINED
#define IRIS_EDITOR_TYPES_DEFINED
typedef struct IrisEditor IrisEditor;

typedef enum {
    IRIS_KEY_CTRL_C    = 3,
    IRIS_KEY_CTRL_G    = 7,
    IRIS_KEY_BACKSPACE = 8,
    IRIS_KEY_TAB       = 9,
    IRIS_KEY_CTRL_J    = 10,
    IRIS_KEY_CTRL_K    = 11,
    IRIS_KEY_ENTER     = 13,
    IRIS_KEY_CTRL_O    = 15,
    IRIS_KEY_CTRL_R    = 18,
    IRIS_KEY_CTRL_T    = 20,
    IRIS_KEY_CTRL_U    = 21,
    IRIS_KEY_CTRL_V    = 22,
    IRIS_KEY_CTRL_W    = 23,
    IRIS_KEY_CTRL_X    = 24,
    IRIS_KEY_CTRL_Y    = 25,
    IRIS_KEY_UP        = 1001,
    IRIS_KEY_DOWN      = 1002,
    IRIS_KEY_LEFT      = 1003,
    IRIS_KEY_RIGHT     = 1004,
    IRIS_KEY_HOME      = 1005,
    IRIS_KEY_END       = 1006,
    IRIS_KEY_DELETE    = 1007,
    IRIS_KEY_PAGE_UP   = 1008,
    IRIS_KEY_PAGE_DOWN = 1009
} IrisKey;
#endif

IrisEditor* iris_create(void);
void iris_destroy(IrisEditor* ed);
void iris_set_filename(IrisEditor* ed, const char* filename);
bool iris_load_text(IrisEditor* ed, const char* text);
bool iris_load_file(IrisEditor* ed, const char* filepath);
bool iris_insert_file(IrisEditor* ed, const char* filepath);
bool iris_save_file(IrisEditor* ed, const char* filepath);
size_t iris_export_text(const IrisEditor* ed, char* out_buf, size_t out_cap);
bool iris_search(IrisEditor* ed, const char* query);
void iris_justify_sql(IrisEditor* ed);
bool iris_feed_key(IrisEditor* ed, int key);
int iris_get_line_count(const IrisEditor* ed);
const char* iris_get_line(const IrisEditor* ed, int line_idx);
void iris_get_cursor(const IrisEditor* ed, int* out_row, int* out_col);
const char* iris_get_status(const IrisEditor* ed);
void iris_render_to_buffer(const IrisEditor* ed, char* out_buf, size_t out_cap);
bool iris_run_interactive(const char* filepath, char* sql_buffer, size_t sql_buffer_cap, bool* out_execute_sql);

#ifdef __cplusplus
}
#endif

#endif /* OPENDATA_H */
