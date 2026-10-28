/*
 * PC/OpenData SQL Server
 * Copyright (c) 2026, PC/OpenSystems LLC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef DATABASE_ENGINE_H
#define DATABASE_ENGINE_H
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#endif
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#ifdef _WIN32
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#define PATH_SEPARATOR "\\"
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <pthread.h>
#define PATH_SEPARATOR "/"
#endif

#define SERVER_VERSION "27.0 SQL Server"
#define MAX_TABLES 128
#define MAX_COLUMNS 32
#define MAX_ROWS 65536
#define MAX_TOTAL_ROWS 1048576
#define MAX_NAME_LEN 64
#define MAX_VAL_LEN 256
#define MAX_USERS 64
#define USER_SALT_SIZE 16
#define USER_VERIFIER_SIZE 32
#define USER_KDF_ITERATIONS 210000u
#define USER_KDF_PBKDF2_SHA256 1u
#define MAX_QUERY_LEN 65536
#define MAX_RESULTS 65536
#define MAX_PAIRS 1000000
#define MAX_IMAGE (256u * 1024u * 1024u)
#define PATH_CAP 4096

typedef enum { TYPE_NUMBER, TYPE_VARCHAR2, TYPE_DATE, TYPE_VECTOR } DataType;
typedef struct { char text[MAX_VAL_LEN]; bool is_null; } Value;

typedef enum {
   FK_ACTION_RESTRICT = 0,
   FK_ACTION_CASCADE,
   FK_ACTION_SET_NULL
} ForeignKeyAction;

typedef struct {
   char name[MAX_NAME_LEN];
   DataType type;
   int size;
   bool nullable, unique, primary;
   char ref_table[MAX_NAME_LEN], ref_column[MAX_NAME_LEN];
   ForeignKeyAction fk_on_delete;
   ForeignKeyAction fk_on_update;
   char constraint_name[MAX_NAME_LEN];
} ColumnDef;
typedef struct { Value values[MAX_COLUMNS]; } Row;

#define CBO_HISTOGRAM_BUCKETS 8

typedef struct {
   int null_count;
   int distinct_count;
   long double min_numeric;
   long double max_numeric;
   bool has_numeric_bounds;
   int histogram[CBO_HISTOGRAM_BUCKETS];
} ColumnStats;

typedef struct {
   bool is_analyzed;
   int sample_rows;
   ColumnStats col_stats[MAX_COLUMNS];
} TableStats;

/* Copy-on-Write row storage shared across database snapshots until modified. */
typedef struct RowStorage RowStorage;
struct RowStorage {
   atomic_int refcount;
   int capacity;
   Row* rows;
};

typedef enum { ENGINE_MEMORY = 0, ENGINE_DISK = 1, ENGINE_COLUMNAR = 2 } StorageEngineType;

typedef enum {
   PART_NONE = 0,
   PART_HASH = 1,
   PART_RANGE = 2
} PartitionType;

#define MAX_PARTITIONS 8

typedef struct {
   char name[MAX_NAME_LEN];
   char upper_bound[64];
   bool is_maxvalue;
} PartitionDef;

typedef struct {
   char name[MAX_NAME_LEN];
   int col_count, row_count, capacity;
   ColumnDef columns[MAX_COLUMNS];
   Row* rows;
   RowStorage* storage;
   StorageEngineType engine_type;
   uint32_t btree_root_page;
   uint32_t first_data_page;
   int pk_cols[MAX_COLUMNS];
   int pk_col_count;
   char pk_name[MAX_NAME_LEN];
   int uq_cols[MAX_COLUMNS][MAX_COLUMNS];
   int uq_col_counts[MAX_COLUMNS];
   char uq_names[MAX_COLUMNS][MAX_NAME_LEN];
   int uq_count;
   TableStats stats;
   /* Multi-Version Garbage Collection (Vacuum) tracking */
   int dead_row_count;
   uint64_t last_vacuum_gen;
   uint64_t total_vacuumed_rows;
   /* Table Partitioning (HASH / RANGE) */
   PartitionType part_type;
   int part_col_idx;
   int part_count;
   PartitionDef partitions[MAX_PARTITIONS];
} Table;

#define MAX_SECONDARY_INDEXES 32

typedef struct {
   Value key;
   int row_index;
} IndexEntry;

typedef struct {
   char name[MAX_NAME_LEN];
   char table_name[MAX_NAME_LEN];
   int table_idx;
   int col_idx;
   bool is_unique;
   int entry_count;
   int capacity;
   IndexEntry* entries;
} SecondaryIndex;

typedef enum { USER_ROLE_ADMIN = 1, USER_ROLE_READ_ONLY = 2 } UserRole;
typedef struct {
   char name[MAX_NAME_LEN];
   unsigned char salt[USER_SALT_SIZE];
   unsigned char verifier[USER_VERIFIER_SIZE];
   uint32_t kdf_id;
   uint32_t iterations;
   UserRole role;
   bool disabled;
} UserAccount;

/* Per-table privileges for network-authenticated users. Bit flags; a user
   with no grant row for a table has no access to it over the network. */
#define PRIV_SELECT 1u
#define PRIV_INSERT 2u
#define PRIV_UPDATE 4u
#define PRIV_DELETE 8u
#define MAX_GRANTS 256
typedef struct {
   char user[MAX_NAME_LEN];
   char table[MAX_NAME_LEN];
   uint32_t privileges;
} TableGrant;

typedef struct {
   int table_count;
   Table tables[MAX_TABLES];
   int index_count;
   SecondaryIndex indexes[MAX_SECONDARY_INDEXES];
   int user_count;
   UserAccount users[MAX_USERS];
   int grant_count;
   TableGrant grants[MAX_GRANTS];
} Database;

typedef enum { TK_END, TK_ID, TK_STRING, TK_NUMBER, TK_LP, TK_RP, TK_COMMA,
   TK_STAR, TK_DOT, TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_SEMI, TK_HELP,
   TK_PLUS, TK_MINUS, TK_SLASH, TK_COSINE_DIST, TK_L2_DIST, TK_BAD } TokenKind;
typedef struct { const char* cursor; TokenKind kind; char text[MAX_VAL_LEN]; bool failed; } Parser;
typedef struct { char table[MAX_NAME_LEN], column[MAX_NAME_LEN]; } ColumnName;
typedef struct { int table, column; } Binding;
#define MAX_SCOPE_TABLES 8
typedef struct {
   Table* tables[MAX_SCOPE_TABLES]; const Row* rows[MAX_SCOPE_TABLES]; char aliases[MAX_SCOPE_TABLES][MAX_NAME_LEN]; int count;
} Scope;
typedef enum {
   PRED_EQ = TK_EQ, PRED_NE = TK_NE, PRED_LT = TK_LT, PRED_LE = TK_LE,
   PRED_GT = TK_GT, PRED_GE = TK_GE,
   PRED_LIKE = 100, PRED_IN, PRED_NOT_IN, PRED_BETWEEN,
   PRED_EXISTS, PRED_NOT_EXISTS
} PredicateOp;
#define MAX_SUBQUERY_LEN 512
typedef struct {
   Binding left, right; bool right_column, is_null_test, negate;
   int op; Value value; Value value2;
   Value in_values[16]; int in_count;
   char expr_op; Value expr_operand;
   char subquery_sql[MAX_SUBQUERY_LEN];
} Predicate;
typedef enum {
   FILTER_NODE_PRED, FILTER_NODE_AND, FILTER_NODE_OR, FILTER_NODE_NOT
} FilterNodeType;
typedef struct {
   FilterNodeType type;
   int left_child, right_child;
   Predicate pred;
} FilterNode;
typedef struct { FilterNode nodes[64]; int node_count, root; } Filter;
typedef struct {
   int col_count;
   char col_names[MAX_COLUMNS][MAX_NAME_LEN];
   int row_count;
   int capacity;
   Value (*rows)[MAX_COLUMNS];
   bool silent;
} QueryResultCapture;
extern QueryResultCapture* g_result_capture;
/* Versioned, little-endian snapshots contain no pointers or compiler struct padding. */
typedef struct { unsigned char* data; size_t size, capacity, offset; bool ok; } Image;

/* Fast in-memory hash indexes for uniqueness, foreign keys, and equi-joins. */
typedef struct { int row; uint32_t hash; } HashSlot;
typedef struct { int capacity, mask, count; HashSlot* slots; } HashIndex;
typedef struct { int row, next; uint32_t hash; } JoinHashEntry;
typedef struct { int capacity, mask; int* buckets; JoinHashEntry* entries; int entry_count; DataType key_type; } JoinIndex;

/* Phase 2: Paged storage definitions */
#define PAGE_SIZE 4096
#define PAGE_TYPE_FREE 0
#define PAGE_TYPE_DATA 1
#define PAGE_TYPE_BTREE_INTERNAL 2
#define PAGE_TYPE_BTREE_LEAF 3
#define BUFFER_POOL_FRAMES 64

typedef struct {
    uint32_t page_id;
    uint16_t page_type;
    uint16_t item_count;
    uint16_t free_start;
    uint16_t free_end;
    uint32_t next_page;
    uint32_t prev_page;
    uint32_t reserved;
} SlottedPageHeader;

typedef struct {
    uint8_t data[PAGE_SIZE];
    uint32_t page_id;
    int pin_count;
    bool is_dirty;
    bool ref_bit;
    bool is_valid;
} BufferFrame;

typedef struct {
    BufferFrame frames[BUFFER_POOL_FRAMES];
    uint32_t clock_hand;
    uint32_t page_count;
} BufferPool;

/* Phase 3: WAL definitions */
#define WAL_MAGIC 0x57414C31
#define WAL_OP_PAGE_DELTA 1
#define WAL_OP_COMMIT     2
#define WAL_OP_CHECKPOINT 3

typedef struct {
    uint32_t magic;
    uint32_t lsn;
    uint32_t tx_id;
    uint16_t op;
    uint16_t length;
    uint32_t page_id;
    uint16_t page_offset;
    uint32_t reserved;
    uint32_t crc32;
} WalRecordHeader;

extern Database *g_db, *g_committed;
extern bool g_pending, g_timing, g_io_uncertain, g_readonly, g_autocommit;
extern bool g_interactive_shell;
extern bool g_semicolon_optional;
extern char g_null_str[32];
extern int g_max_rows;
extern FILE* g_spool_file;
extern char g_spool_path[PATH_CAP];
extern atomic_int g_errors;
extern uint64_t g_generation, g_identity;
extern char g_path[PATH_CAP], g_backup_dir[PATH_CAP];
extern unsigned g_backup_interval;
extern atomic_bool g_stop, g_backup_failed;
extern atomic_ullong g_backup_generation, g_backup_time;
extern const Scope* g_outer_scope;
extern char g_network_user[MAX_NAME_LEN];
extern bool g_network_tls_active;
extern char g_last_error[MAX_VAL_LEN];

/* User configuration and error reporting */
extern char g_config_path[PATH_CAP];
extern char g_error_report_path[PATH_CAP];
extern bool g_config_loaded;
extern char g_default_database[PATH_CAP];
extern bool g_show_tutorial;
extern bool g_autocommit_configured;
extern bool g_semicolon_optional_configured;
extern bool g_audit_log_enabled;
extern char g_audit_log_path[PATH_CAP];

bool get_home_directory(char* output, size_t capacity);
bool ensure_user_config_exists(void);
bool load_user_config(void);
bool validate_user_config(void);
void log_error_report(const char* format, ...);
void log_error_report_event(const char* event);
void audit_log_statement(const char* command, const char* detail, bool success);
bool verify_audit_log(const char* path, char* error_out, size_t error_cap);
bool execute_verify_audit_log(void);
extern char g_tde_key[65];
extern bool g_tde_enabled;
bool opendata_set_tde_key(const char* key_hex);

bool acquire_database_file_lock(void);
bool allocate_table_storage(Table* table, int capacity);
bool backup_committed_snapshot(void);
bool commit_transaction(void);
int compare_typed_values(DataType type, const Value* a, const Value* b);
bool consume_keyword(Parser* p, const char* word);
bool consume_statement_end(Parser* p);
bool consume_token(Parser* p, TokenKind kind);
Database* copy_database(const Database* source);
Database* decode_database_snapshot(Image* image, uint64_t* identity, uint64_t* generation);
void destroy_database(Database* db);
bool encode_database_snapshot(const Database* db, uint64_t identity, uint64_t generation, Image* image);
bool ensure_table_writable(Table* table);
bool evaluate_comparison_predicate(const Scope* scope, const Predicate* pred);
bool evaluate_where_clause(const Scope* scope, const Filter* filter);
bool execute_alter_table(Parser* p, Database* db);
bool execute_atomic_mutation(Parser* p, const char* command);
bool execute_console_statement(const char* text, bool* quit);
bool execute_create_table(Parser* p, Database* db);
bool execute_drop_table(Parser* p, Database* db);
bool execute_insert_row(Parser* p, Database* db, int* inserted);
bool execute_select_query(Parser* p);
bool execute_with_query(Parser* p);
bool execute_truncate_table(Parser* p, Database* db);
bool execute_update_or_delete(Parser* p, Database* db, bool deleting, int* affected);
int find_column_index(const Table* table, const char* name);
int find_table_index(const Database* db, const char* name);
void hash_index_destroy(HashIndex* index);
int hash_index_find(const HashIndex* index, const Table* table, int col_index, const Value* val);
bool hash_index_init(HashIndex* index, int min_capacity);
bool hash_index_insert(HashIndex* index, const Table* table, int col_index, int row);
bool is_ascii_digit(char c);
bool is_identifier_start(char c);
bool is_sql_whitespace(char c);
bool is_valid_column_value(const ColumnDef* column, const Value* value);
bool is_valid_datetime(const char* text);
bool is_valid_identifier(const char* text);
bool join_index_build(JoinIndex* index, const Table* table, int col_index);
void join_index_destroy(JoinIndex* index);
int join_index_first(const JoinIndex* index, const Value* val);
void lock_snapshot_io(void);
void lock_engine_state(void);
double monotonic_seconds(void);
bool normalize_decimal(const char* text, char* output);
bool open_or_restore_database(const char* restore);
bool parse_column_reference(Parser* p, ColumnName* name);
bool parse_comparison_predicate(Parser* p, const Scope* scope, Predicate* pred);
bool parse_identifier(Parser* p, char* output);
bool parse_typed_literal(Parser* p, const ColumnDef* column, Value* value);
bool parse_where_clause(Parser* p, const Scope* scope, Filter* filter);
void print_command_help(void);
bool publish_durable_snapshot(const char* path, const Image* image, bool replace, bool* uncertain);
bool read_snapshot_file(const char* path, Image* image);
void release_table_storage(Table* table);
bool report_database_error(const char* format, ...);
bool reserve_insert_capacity(Table* table);
bool resolve_absolute_path(const char* input, char* output);
bool resolve_column_reference(const Scope* scope, const ColumnName* name, Binding* binding);
Table* resolve_statement_table(Parser* p, Database* db);
bool rollback_transaction(void);
void scan_next_token(Parser* p);
bool snapshot_file_matches(const Image* expected, const char* path);
bool snapshot_path_exists(const char* path);
bool token_is_keyword(Parser* p, const char* word);
void unlock_snapshot_io(void);
void unlock_engine_state(void);
char uppercase_ascii(char c);
bool validate_relational_integrity(const Database* db);
bool execute_user_catalog_statement(Parser* p, const char* command);
bool execute_grant_statement(Parser* p, bool granting);
bool execute_show_grants(void);
bool user_has_table_privilege(const Database* db, const char* user, const char* table, uint32_t privilege);
int find_user_index(const Database* db, const char* name);
bool verify_user_password(const UserAccount* account, const char* password);
bool authenticate_database_user(const Database* db, const char* name, const char* password,
                               UserRole* role);
bool set_user_password(UserAccount* account, const char* password);
bool create_bootstrap_admin(const char* name, const char* password);
bool validate_user_name(const char* name);

/* Embedded C library API */
bool opendata_open(const char* db_path, const char* backup_dir, unsigned backup_interval, const char* restore_path);
bool opendata_open_readonly(const char* db_path);
bool opendata_exec(const char* sql);
bool opendata_commit(void);
bool opendata_rollback(void);
bool opendata_savepoint(const char* name);
bool opendata_rollback_to(const char* name);
bool opendata_release_savepoint(const char* name);
bool opendata_backup(void);
uint64_t opendata_get_generation(void);
bool opendata_has_pending_changes(void);
int opendata_get_error_count(void);
void opendata_close(void);

typedef struct OpendataStmt OpendataStmt;
OpendataStmt* opendata_prepare(const char* sql);
bool opendata_bind_int(OpendataStmt* stmt, int param_idx, int64_t val);
bool opendata_bind_text(OpendataStmt* stmt, int param_idx, const char* str);
bool opendata_bind_null(OpendataStmt* stmt, int param_idx);
bool opendata_step(OpendataStmt* stmt);
int opendata_column_count(OpendataStmt* stmt);
const char* opendata_column_name(OpendataStmt* stmt, int col_idx);
const char* opendata_column_text(OpendataStmt* stmt, int col_idx);
bool opendata_column_is_null(OpendataStmt* stmt, int col_idx);
bool opendata_reset(OpendataStmt* stmt);
void opendata_finalize(OpendataStmt* stmt);
bool opendata_export_csv(const char* table_name, const char* filepath);
bool opendata_import_csv(const char* table_name, const char* filepath, bool has_header);
bool opendata_export_json(const char* table_name, const char* filepath);
bool opendata_import_json(const char* table_name, const char* filepath);
void opendata_set_format(const char* format);

/* Priority 2: Multi-Instance Engine Context */
typedef struct OpendataContext OpendataContext;
typedef struct OpendataContext OpendataCtx;

OpendataCtx* opendata_ctx_create(void);
bool opendata_ctx_open(OpendataCtx* ctx, const char* db_path, const char* backup_dir, unsigned backup_interval, const char* restore_path);
bool opendata_ctx_exec(OpendataCtx* ctx, const char* sql);
bool opendata_ctx_commit(OpendataCtx* ctx);
bool opendata_ctx_rollback(OpendataCtx* ctx);
void opendata_ctx_close(OpendataCtx* ctx);
void opendata_ctx_destroy(OpendataCtx* ctx);

/* Phase 1: Slotted Binary Rows */
size_t encode_slotted_row(const ColumnDef* cols, int col_count, const Row* row, uint8_t* dest, size_t max_size);
bool decode_slotted_row(const ColumnDef* cols, int col_count, const uint8_t* src, size_t src_size, Row* row);
bool read_slotted_cell(const ColumnDef* col, const uint8_t* src, size_t src_size, int col_idx, int col_count, Value* val);

/* Phase 2: Paged Storage & Buffer Pool */
bool slotted_page_init(uint8_t* page, uint32_t page_id, uint16_t page_type);
int slotted_page_insert_record(uint8_t* page, const uint8_t* record, size_t record_len);
bool slotted_page_get_record(const uint8_t* page, int slot_idx, const uint8_t** out_data, size_t* out_len);
size_t slotted_page_free_space(const uint8_t* page);

void buffer_pool_init(BufferPool* bp);
uint8_t* buffer_pool_fetch(BufferPool* bp, int fd, uint32_t page_id, bool is_write);
void buffer_pool_unpin(BufferPool* bp, uint32_t page_id, bool is_dirty);
bool buffer_pool_flush_all(BufferPool* bp, int fd);
uint32_t buffer_pool_alloc_page(BufferPool* bp, int fd, uint16_t page_type);
void buffer_pool_destroy(BufferPool* bp, int fd);

/* Phase 3: Write-Ahead Logging (WAL) */
bool wal_init(const char* base_path);
bool wal_write_page_delta(uint32_t page_id, uint16_t offset, const void* data, uint16_t len);
bool wal_commit_tx(uint32_t tx_id);
bool wal_checkpoint_now(BufferPool* bp, int db_fd);
bool wal_recover_and_replay(int db_fd, BufferPool* bp);
bool wal_recover_and_replay_to_timestamp(int db_fd, BufferPool* bp, uint32_t target_timestamp);
void wal_shutdown(void);

/* Phase 4: Persistent On-Disk B+ Tree Index */
uint32_t btree_create(BufferPool* bp, int fd, DataType key_type);
bool btree_search(BufferPool* bp, int fd, uint32_t root_page, DataType key_type, const Value* key, uint32_t* out_page, int* out_slot);
bool btree_insert(BufferPool* bp, int fd, uint32_t* root_page, DataType key_type, const Value* key, uint32_t data_page, int slot_idx);
int btree_range_scan(BufferPool* bp, int fd, uint32_t root_page, DataType key_type, const Value* low, const Value* high, uint32_t* out_pages, int* out_slots, int max_results);

void initialize_snapshot_io(void);
void shutdown_snapshot_io(void);
bool start_periodic_backups(void);
bool start_periodic_backups_for_context(atomic_bool* stop_flag, atomic_bool* failed_flag);
void stop_periodic_backups(void);
atomic_bool* get_backup_worker_stop_flag(void);
void set_backup_worker_stop_flag(atomic_bool* stop_flag);
void clear_backup_worker_handle(void);
void release_database_file_lock(void);
#ifdef _WIN32
HANDLE get_database_file_lock(void);
void set_database_file_lock(HANDLE h);
HANDLE get_backup_worker_handle(void);
void set_backup_worker_handle(HANDLE h);
#else
int get_database_file_lock(void);
void set_database_file_lock(int fd);
pthread_t get_backup_worker_handle(void);
void set_backup_worker_handle(pthread_t t);
#endif

/* Output Formatting and JSON/CSV Engine */
typedef enum {
    FORMAT_TABLE = 0,
    FORMAT_CSV = 1,
    FORMAT_JSON = 2
} OutputFormat;

extern OutputFormat g_output_format;

bool export_table_csv(const Database* db, const char* table_name, const char* filepath);
bool import_table_csv(Database* db, const char* table_name, const char* filepath, bool has_header);
bool export_table_json(const Database* db, const char* table_name, const char* filepath);
bool import_table_json(Database* db, const char* table_name, const char* filepath);
bool extract_json_value(const char* json, const char* path, char* out_val, size_t out_cap);
bool opendata_extract_json(const char* json, const char* path, char* out_val, size_t out_cap);
bool execute_export_statement(Parser* p);
bool execute_import_statement(Parser* p);
bool run_csv_import_wizard(Database* db);

/* Headless Server and Network */
typedef struct {
    int port;
    const char* bind_addr;
    bool tcps;
    const char* cert_path;
    const char* key_path;
} ServerConfig;

bool run_headless_server(const ServerConfig* config);
bool bootstrap_server_accounts(void);
void reset_auth_rate_limit(void);
bool admit_auth_attempt(uint32_t source, const char* user, uint64_t now_ms);
typedef struct {
    uint64_t admitted, credential_failures;
    uint64_t source_throttled, pair_throttled, global_throttled;
    uint64_t source_saturated, pair_saturated;
} AuthRateLimitStats;
AuthRateLimitStats auth_rate_limit_stats(void);
void record_auth_credential_failure(void);

typedef struct {
    char server_cert[PATH_CAP];
    char server_key[PATH_CAP];
    char cluster_cert[PATH_CAP];
    char cluster_key[PATH_CAP];
    char cluster_ca[PATH_CAP];
    char server_thumbprint[41];
    char cluster_thumbprint[41];
    char server_store[32];
    char cluster_store[32];
    char cluster_trust_store[32];
    char directory[PATH_CAP];
} TlsWallet;

bool load_tls_wallet(const char* directory, TlsWallet* wallet);
bool inspect_tls_wallet(const char* directory);

/* Raft Consensus and Synchronous WAL Shipping */
typedef enum {
    RAFT_FOLLOWER = 0,
    RAFT_CANDIDATE = 1,
    RAFT_LEADER = 2
} RaftRole;

typedef struct {
    int node_id;
    int port;
    int peer_count;
    char peer_addrs[4][64];
    bool is_leader;
    bool sync_replication;
    const char* cert_path;
    const char* key_path;
    const char* ca_path;
} ClusterConfig;

typedef struct ClusterTlsContext ClusterTlsContext;
typedef struct ClusterTlsStream ClusterTlsStream;

extern bool g_sync_replication;
/* Written by the cluster worker thread, read by the network/main threads. */
extern _Atomic RaftRole g_raft_role;
extern int g_raft_node_id;

bool cluster_init(const ClusterConfig* config);
bool raft_ship_wal_sync(const void* data, size_t len);
void cluster_shutdown(void);
void print_cluster_status(void);
uint32_t raft_get_current_term(void);
bool raft_add_peer(const char* peer_addr);
bool raft_remove_peer(int node_id);
ClusterTlsContext* cluster_tls_create(const char* cert_path, const char* key_path,
                                      const char* ca_path, int local_node_id);
void cluster_tls_destroy(ClusterTlsContext* context);
ClusterTlsStream* cluster_tls_accept(ClusterTlsContext* context, intptr_t socket);
ClusterTlsStream* cluster_tls_connect(ClusterTlsContext* context, intptr_t socket,
                                      const char* peer_ip, int peer_node_id);
int cluster_tls_peer_node_id(ClusterTlsStream* stream);
bool cluster_tls_send_all(ClusterTlsStream* stream, const void* data, size_t len);
bool cluster_tls_recv_all(ClusterTlsStream* stream, void* data, size_t len);
void cluster_tls_close(ClusterTlsStream* stream);

/* IRIS Internal Text Editor (GNU Nano-Styled SQL & Script Editor) */
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

bool execute_savepoint(const char* name);
bool execute_rollback_savepoint(const char* name);
bool execute_release_savepoint(const char* name);
void clear_all_savepoints(void);

/* Secondary In-Memory Indexes */
bool execute_create_index(const char* idx_name, const char* tbl_name, const char* col_name, bool unique);
bool execute_drop_index(const char* idx_name);
bool execute_create_index_in_db(Database* db, const char* idx_name, const char* tbl_name, const char* col_name, bool unique);
bool execute_drop_index_in_db(Database* db, const char* idx_name);
int find_secondary_index(const Database* db, const char* tbl_name, int col_idx);
bool build_secondary_index(SecondaryIndex* idx, const Table* table, int col_idx);
void rebuild_secondary_indexes(Database* db);
void secondary_index_insert_row(Database* db, int table_idx, int row_idx);
void secondary_index_remove_row(Database* db, int table_idx, int row_idx);
int secondary_index_find_range(const SecondaryIndex* idx, DataType type, const Value* low, const Value* high, int* out_rows, int max_rows);

/* Cost-Based Optimizer (CBO) & Statistics */
bool execute_analyze_table(const char* table_name);
bool execute_analyze_all(void);
void print_table_stats(const char* table_name);
double cbo_estimate_predicate_selectivity(const Table* tbl, const Predicate* pred);
double cbo_estimate_filter_selectivity(const Table* tbl, int table_scope_idx, const Filter* filter);
double cbo_estimate_query_cost(const Database* db, const Table* tbl, int table_scope_idx, const Filter* filter, bool* out_use_index, int* out_index_id, int* out_est_rows);

/* Multi-Version Garbage Collection (MVCC Vacuum) */
bool vacuum_table_storage(Database* db, int table_idx, bool full, size_t* out_freed_bytes, int* out_reclaimed_versions);
bool execute_vacuum_command(const char* table_name, bool full, bool analyze);
bool opendata_vacuum(const char* table_name);

/* Partitioning & Vectorized Columnar Spillover Engine */
int get_row_partition(const Table* tbl, const Row* row);
uint32_t prune_partitions_for_filter(const Table* tbl, int table_scope_idx, const Filter* filter);
void vector_aggregate_numeric(const Table* table, int col_idx, const Filter* filter, long double* out_sum, long double* out_min, long double* out_max, int* out_count);
void vector_aggregate_numeric_partitioned(const Table* table, int col_idx, const Filter* filter, uint32_t active_part_mask, long double* out_sum, long double* out_min, long double* out_max, int* out_count);

/* Non-blocking Async Event-Multiplexed Wire Server */
bool run_nonblocking_wire_server(const ServerConfig* config);

/* Zero-Downtime Hot-Patch Restart: the running server spawns its successor,
   hands the listening socket to it, drains in-flight replies, and exits so the
   new binary resumes service without refusing connections. */
extern atomic_bool g_restart_requested;
extern bool g_server_mode, g_cluster_active, g_hotpatch_mode;

void hotpatch_capture_launch(int argc, char** argv);
bool hotpatch_child_bootstrap(void);
bool request_server_restart(void);
void install_server_signal_handlers(void);
bool hotpatch_spawn_successor(intptr_t listener, char* error, size_t error_cap);
intptr_t hotpatch_adopt_listener(int port, const char* bind_addr);

/* Phase 1 & 5: Session Registry & PostgreSQL v3 Wire Protocol */
int register_client_session(const char* addr);
void unregister_client_session(int session_id);
void update_client_session_query(int session_id, const char* state, const char* query);
bool is_client_session_killed(int session_id);
void print_active_sessions(void);
bool kill_client_session(int session_id);
int get_active_session_count(void);

/* Phase 4: Native VECTOR(dim) SIMD Distance Functions */
int parse_vector_floats(const char* text, float* out_vals, int max_dim);
double vector_cosine_distance(const char* a, const char* b);
double vector_l2_distance(const char* a, const char* b);

/* Microsoft SQL Server & Oracle Database File Compatibility & Schema Diagnostics */
bool export_database_mssql(const Database* db, const char* table_name, const char* filepath);
bool import_database_mssql(Database* db, const char* table_name, const char* filepath);
bool export_database_oracle(const Database* db, const char* table_name, const char* filepath);
bool import_database_oracle(Database* db, const char* table_name, const char* filepath);
bool opendata_export_mssql(const char* table_name, const char* filepath);
bool opendata_import_mssql(const char* table_name, const char* filepath);
bool opendata_export_oracle(const char* table_name, const char* filepath);
bool opendata_import_oracle(const char* table_name, const char* filepath);
bool dump_database_sql(const Database* db, const char* filepath);
bool diff_database_schema(const Database* db, const char* other_db_path);

#endif
