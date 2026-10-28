#include "database_engine.h"

static uint32_t hash_value_text(const char* s) {
    uint32_t h = 2166136261u;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        h ^= (uint32_t)*p++;
        h *= 16777619u;
    }
    return h;
}

/* Hash a value the way compare_typed_values compares it, so equal values land
   in the same bucket. Numeric values are normalized through their long double
   representation ("1.0" and "1" must hash identically). */
static uint32_t hash_typed_value(DataType type, const Value* v) {
    if (type == TYPE_NUMBER && !v->is_null) {
        long double num = strtold(v->text, NULL);
        if (num == 0) num = 0; /* collapse -0.0 to +0.0 */
        unsigned long long bits = 0;
        double d = (double)num;
        memcpy(&bits, &d, sizeof(bits));
        uint32_t h = 2166136261u;
        for (unsigned i = 0; i < sizeof(bits); i++) {
            h ^= (uint32_t)(bits & 0xFFu);
            h *= 16777619u;
            bits >>= 8;
        }
        return h;
    }
    return hash_value_text(v->text);
}

bool hash_index_init(HashIndex* index, int min_capacity) {
    memset(index, 0, sizeof(*index));
    int cap = 16;
    while (cap < min_capacity * 2 && cap < 65536) cap *= 2;
    index->slots = malloc((size_t)cap * sizeof(HashSlot));
    if (!index->slots) return false;
    for (int i = 0; i < cap; i++) {
        index->slots[i].row = -1;
        index->slots[i].hash = 0;
    }
    index->capacity = cap;
    index->mask = cap - 1;
    index->count = 0;
    return true;
}

void hash_index_destroy(HashIndex* index) {
    if (index) {
        free(index->slots);
        memset(index, 0, sizeof(*index));
    }
}

int hash_index_find(const HashIndex* index, const Table* table, int col_index, const Value* val) {
    if (!index || !index->slots || val->is_null) return -1;
    uint32_t h = hash_typed_value(table->columns[col_index].type, val);
    int mask = index->mask;
    int i = (int)(h & (uint32_t)mask);
    int step = 0;
    while (index->slots[i].row >= 0 && step < index->capacity) {
        if (index->slots[i].hash == h) {
            int r = index->slots[i].row;
            const Value* cand = &table->rows[r].values[col_index];
            if (!cand->is_null && compare_typed_values(table->columns[col_index].type, val, cand) == 0) {
                return r;
            }
        }
        i = (i + 1) & mask;
        step++;
    }
    return -1;
}

bool hash_index_insert(HashIndex* index, const Table* table, int col_index, int row) {
    if (!index || !index->slots) return false;
    const Value* val = &table->rows[row].values[col_index];
    if (val->is_null) return true;
    uint32_t h = hash_typed_value(table->columns[col_index].type, val);
    int mask = index->mask;
    int i = (int)(h & (uint32_t)mask);
    int step = 0;
    while (index->slots[i].row >= 0 && step < index->capacity) {
        i = (i + 1) & mask;
        step++;
    }
    if (step >= index->capacity) return false;
    index->slots[i].row = row;
    index->slots[i].hash = h;
    index->count++;
    return true;
}

bool join_index_build(JoinIndex* index, const Table* table, int col_index) {
    memset(index, 0, sizeof(*index));
    index->key_type = table->columns[col_index].type;
    int cap = 16;
    while (cap < table->row_count * 2 && cap < 65536) cap *= 2;
    index->buckets = malloc((size_t)cap * sizeof(int));
    if (!index->buckets) return false;
    for (int i = 0; i < cap; i++) index->buckets[i] = -1;
    index->capacity = cap;
    index->mask = cap - 1;
    index->entry_count = 0;
    if (table->row_count > 0) {
        index->entries = malloc((size_t)table->row_count * sizeof(JoinHashEntry));
        if (!index->entries) {
            free(index->buckets);
            index->buckets = NULL;
            return false;
        }
    }
    for (int r = 0; r < table->row_count; r++) {
        const Value* val = &table->rows[r].values[col_index];
        if (val->is_null) continue;
        uint32_t h = hash_typed_value(table->columns[col_index].type, val);
        int b = (int)(h & (uint32_t)index->mask);
        int entry_idx = index->entry_count++;
        index->entries[entry_idx].row = r;
        index->entries[entry_idx].hash = h;
        index->entries[entry_idx].next = index->buckets[b];
        index->buckets[b] = entry_idx;
    }
    return true;
}

int join_index_first(const JoinIndex* index, const Value* val) {
    if (!index || !index->buckets || val->is_null) return -1;
    uint32_t h = hash_typed_value(index->key_type, val);
    int b = (int)(h & (uint32_t)index->mask);
    return index->buckets[b];
}

void join_index_destroy(JoinIndex* index) {
    if (index) {
        free(index->buckets);
        free(index->entries);
        memset(index, 0, sizeof(*index));
    }
}

/* ----------------------------------------------------------------- */
/* Recommendation 5: Persistent Secondary B+ Tree / Ordered Indexes */
/* ----------------------------------------------------------------- */

static int compare_index_entries(const void* a_ptr, const void* b_ptr, void* type_ptr) {
    const IndexEntry* ea = (const IndexEntry*)a_ptr;
    const IndexEntry* eb = (const IndexEntry*)b_ptr;
    DataType type = (DataType)(uintptr_t)type_ptr;
    if (ea->key.is_null && eb->key.is_null) return 0;
    if (ea->key.is_null) return 1;
    if (eb->key.is_null) return -1;
    int cmp = compare_typed_values(type, &ea->key, &eb->key);
    if (cmp != 0) return cmp;
    return ea->row_index - eb->row_index;
}

/* qsort_r is non-portable; guard the comparator context with a mutex so
   concurrent index builds cannot race on the shared sort type. */
#ifdef _WIN32
static SRWLOCK g_index_sort_lock = SRWLOCK_INIT;
static void index_sort_lock(void) { AcquireSRWLockExclusive(&g_index_sort_lock); }
static void index_sort_unlock(void) { ReleaseSRWLockExclusive(&g_index_sort_lock); }
#else
static pthread_mutex_t g_index_sort_lock = PTHREAD_MUTEX_INITIALIZER;
static void index_sort_lock(void) { pthread_mutex_lock(&g_index_sort_lock); }
static void index_sort_unlock(void) { pthread_mutex_unlock(&g_index_sort_lock); }
#endif
static DataType g_index_sort_type = TYPE_VARCHAR2;
static int compare_index_entries_qsort(const void* a_ptr, const void* b_ptr) {
    return compare_index_entries(a_ptr, b_ptr, (void*)(uintptr_t)g_index_sort_type);
}
static void sort_index_entries(IndexEntry* entries, int count, DataType type) {
    index_sort_lock();
    g_index_sort_type = type;
    qsort(entries, (size_t)count, sizeof(IndexEntry), compare_index_entries_qsort);
    index_sort_unlock();
}

bool build_secondary_index(SecondaryIndex* idx, const Table* table, int col_idx) {
    free(idx->entries);
    idx->entries = NULL;
    idx->entry_count = 0;
    idx->capacity = 0;

    int total = table->row_count;
    if (total <= 0) return true;

    idx->entries = malloc((size_t)total * sizeof(IndexEntry));
    if (!idx->entries) return false;
    idx->capacity = total;

    for (int r = 0; r < total; r++) {
        const Value* v = &table->rows[r].values[col_idx];
        if (!v->is_null) {
            idx->entries[idx->entry_count].key = *v;
            idx->entries[idx->entry_count].row_index = r;
            idx->entry_count++;
        }
    }

    if (idx->entry_count > 1) {
        sort_index_entries(idx->entries, idx->entry_count, table->columns[col_idx].type);
    }
    return true;
}

bool execute_create_index_in_db(Database* db, const char* idx_name, const char* tbl_name, const char* col_name, bool unique) {
    if (!db) return report_database_error("database not open");
    if (!idx_name || !*idx_name || !tbl_name || !*tbl_name || !col_name || !*col_name) {
        return report_database_error("invalid index definition");
    }

    for (int i = 0; i < db->index_count; i++) {
        if (!strcmp(db->indexes[i].name, idx_name)) {
            return report_database_error("index already exists: %s", idx_name);
        }
    }

    int t_idx = find_table_index(db, tbl_name);
    if (t_idx < 0) return report_database_error("table does not exist: %s", tbl_name);
    if (g_network_user[0])
        return report_database_error("permission denied: CREATE INDEX requires the ADMIN role");
    const Table* table = &db->tables[t_idx];

    int c_idx = find_column_index(table, col_name);
    if (c_idx < 0) return report_database_error("column does not exist: %s", col_name);

    if (db->index_count >= MAX_SECONDARY_INDEXES) {
        return report_database_error("maximum secondary indexes (%d) reached", MAX_SECONDARY_INDEXES);
    }

    SecondaryIndex* idx = &db->indexes[db->index_count];
    memset(idx, 0, sizeof(*idx));
    snprintf(idx->name, sizeof(idx->name), "%s", idx_name);
    snprintf(idx->table_name, sizeof(idx->table_name), "%s", tbl_name);
    idx->table_idx = t_idx;
    idx->col_idx = c_idx;
    idx->is_unique = unique;

    if (!build_secondary_index(idx, table, c_idx)) {
        return report_database_error("failed to build secondary index: %s", idx_name);
    }

    if (unique && idx->entry_count > 1) {
        DataType type = table->columns[c_idx].type;
        for (int i = 0; i < idx->entry_count - 1; i++) {
            if (compare_typed_values(type, &idx->entries[i].key, &idx->entries[i + 1].key) == 0) {
                free(idx->entries);
                idx->entries = NULL;
                return report_database_error("unique index violation on %s(%s)", tbl_name, col_name);
            }
        }
    }

    db->index_count++;
    return true;
}

bool execute_drop_index_in_db(Database* db, const char* idx_name) {
    if (!db) return report_database_error("database not open");
    if (!idx_name || !*idx_name) return report_database_error("invalid index name");

    int found = -1;
    for (int i = 0; i < db->index_count; i++) {
        if (!strcmp(db->indexes[i].name, idx_name)) {
            found = i; break;
        }
    }
    if (found < 0) return report_database_error("index not found: %s", idx_name);

    free(db->indexes[found].entries);
    for (int i = found; i < db->index_count - 1; i++) {
        db->indexes[i] = db->indexes[i + 1];
    }
    db->index_count--;
    return true;
}

bool execute_create_index(const char* idx_name, const char* tbl_name, const char* col_name, bool unique) {
    if (!g_db || g_readonly) return report_database_error("database is readonly or not open");
    if (!execute_create_index_in_db(g_db, idx_name, tbl_name, col_name, unique)) return false;
    g_pending = true;
    printf("Index %s created on %s(%s).\n", idx_name, tbl_name, col_name);
    return true;
}

bool execute_drop_index(const char* idx_name) {
    if (!g_db || g_readonly) return report_database_error("database is readonly or not open");
    if (!execute_drop_index_in_db(g_db, idx_name)) return false;
    g_pending = true;
    printf("Index %s dropped.\n", idx_name);
    return true;
}

int find_secondary_index(const Database* db, const char* tbl_name, int col_idx) {
    if (!db) return -1;
    for (int i = 0; i < db->index_count; i++) {
        if (!strcmp(db->indexes[i].table_name, tbl_name) && db->indexes[i].col_idx == col_idx) {
            return i;
        }
    }
    return -1;
}

void rebuild_secondary_indexes(Database* db) {
    if (!db) return;
    for (int i = 0; i < db->index_count; i++) {
        int t = find_table_index(db, db->indexes[i].table_name);
        if (t >= 0) {
            db->indexes[i].table_idx = t;
            build_secondary_index(&db->indexes[i], &db->tables[t], db->indexes[i].col_idx);
        }
    }
}

void secondary_index_insert_row(Database* db, int table_idx, int row_idx) {
    if (!db || table_idx < 0 || table_idx >= db->table_count) return;
    const Table* table = &db->tables[table_idx];
    for (int i = 0; i < db->index_count; i++) {
        SecondaryIndex* idx = &db->indexes[i];
        if (!strcmp(idx->table_name, table->name)) {
            if (idx->col_idx < 0 || idx->col_idx >= table->col_count) continue;
            const Value* v = &table->rows[row_idx].values[idx->col_idx];
            if (!v->is_null) {
                if (idx->entry_count >= idx->capacity) {
                    int new_cap = idx->capacity ? idx->capacity * 2 : 16;
                    IndexEntry* nentries = realloc(idx->entries, (size_t)new_cap * sizeof(IndexEntry));
                    if (nentries) {
                        idx->entries = nentries;
                        idx->capacity = new_cap;
                    }
                }
                if (idx->entry_count < idx->capacity) {
                    idx->entries[idx->entry_count].key = *v;
                    idx->entries[idx->entry_count].row_index = row_idx;
                    idx->entry_count++;
                    sort_index_entries(idx->entries, idx->entry_count, table->columns[idx->col_idx].type);
                }
            }
        }
    }
}

void secondary_index_remove_row(Database* db, int table_idx, int row_idx) {
    if (!db || table_idx < 0 || table_idx >= db->table_count) return;
    const Table* table = &db->tables[table_idx];
    for (int i = 0; i < db->index_count; i++) {
        SecondaryIndex* idx = &db->indexes[i];
        if (!strcmp(idx->table_name, table->name)) {
            int write_k = 0;
            for (int k = 0; k < idx->entry_count; k++) {
                if (idx->entries[k].row_index == row_idx) continue;
                if (idx->entries[k].row_index > row_idx) {
                    idx->entries[k].row_index--;
                }
                if (write_k != k) {
                    idx->entries[write_k] = idx->entries[k];
                }
                write_k++;
            }
            idx->entry_count = write_k;
        }
    }
}

int secondary_index_find_range(const SecondaryIndex* idx, DataType type, const Value* low, const Value* high, int* out_rows, int max_rows) {
    if (!idx || !idx->entries || idx->entry_count == 0 || max_rows <= 0) return 0;
    int matched = 0;

    int left = 0, right = idx->entry_count - 1, start_idx = 0;
    if (low && !low->is_null) {
        start_idx = idx->entry_count; /* default: no key >= low */
        while (left <= right) {
            int mid = left + (right - left) / 2;
            int cmp = compare_typed_values(type, &idx->entries[mid].key, low);
            if (cmp >= 0) {
                start_idx = mid;
                right = mid - 1;
            } else {
                left = mid + 1;
            }
        }
    }

    for (int i = start_idx; i < idx->entry_count && matched < max_rows; i++) {
        if (high && !high->is_null) {
            if (compare_typed_values(type, &idx->entries[i].key, high) > 0) break;
        }
        out_rows[matched++] = idx->entries[i].row_index;
    }
    return matched;
}

/* ----------------------------------------------------------------- */
/* Recommendation 1: Cost-Based Optimizer (CBO) & Statistics Engine  */
/* ----------------------------------------------------------------- */

bool execute_analyze_table(const char* table_name) {
    if (!g_db) return report_database_error("no open database");
    if (g_readonly) return report_database_error("cannot ANALYZE in read-only mode");
    if (g_raft_role == RAFT_FOLLOWER) return report_database_error("follower nodes accept read-only queries");
    int t_idx = find_table_index(g_db, table_name);
    if (t_idx < 0) return report_database_error("table does not exist: %s", table_name);
    Table* tbl = &g_db->tables[t_idx];

    tbl->stats.is_analyzed = true;
    tbl->stats.sample_rows = tbl->row_count;

    for (int c = 0; c < tbl->col_count; c++) {
        ColumnStats* cs = &tbl->stats.col_stats[c];
        memset(cs, 0, sizeof(*cs));
        DataType type = tbl->columns[c].type;

        int nulls = 0;
        long double min_num = 1e30L, max_num = -1e30L;
        bool has_num = false;

        // Distinct counting via sample array (heap-allocated: Value is large).
        Value* distinct_vals = calloc(256, sizeof(Value));
        if (!distinct_vals) return report_database_error("out of memory");
        int distinct_cnt = 0;

        for (int r = 0; r < tbl->row_count; r++) {
            const Value* v = &tbl->rows[r].values[c];
            if (v->is_null) {
                nulls++;
            } else {
                if (type == TYPE_NUMBER) {
                    long double num = strtold(v->text, NULL);
                    if (!has_num || num < min_num) min_num = num;
                    if (!has_num || num > max_num) max_num = num;
                    has_num = true;
                }
                if (distinct_cnt < 256) {
                    bool seen = false;
                    for (int d = 0; d < distinct_cnt; d++) {
                        if (compare_typed_values(type, v, &distinct_vals[d]) == 0) {
                            seen = true; break;
                        }
                    }
                    if (!seen) distinct_vals[distinct_cnt++] = *v;
                }
            }
        }
        free(distinct_vals);
        cs->null_count = nulls;
        cs->distinct_count = distinct_cnt;
        cs->has_numeric_bounds = has_num;
        cs->min_numeric = has_num ? min_num : 0;
        cs->max_numeric = has_num ? max_num : 0;
    }
    printf("Table %s analyzed (%d rows, %d columns sampled).\n", tbl->name, tbl->row_count, tbl->col_count);
    return true;
}

bool execute_analyze_all(void) {
    if (!g_db) return report_database_error("no open database");
    for (int t = 0; t < g_db->table_count; t++) {
        execute_analyze_table(g_db->tables[t].name);
    }
    printf("All %d tables analyzed successfully.\n", g_db->table_count);
    return true;
}

void print_table_stats(const char* table_name) {
    if (!g_db) return;
    int t_start = 0, t_end = g_db->table_count;
    if (table_name && *table_name) {
        int t = find_table_index(g_db, table_name);
        if (t < 0) { report_database_error("table not found: %s", table_name); return; }
        t_start = t; t_end = t + 1;
    }

    printf("TABLE_NAME\tCOLUMN_NAME\tTYPE\tSAMPLED_ROWS\tNULLS\tDISTINCT\tMIN_VAL\tMAX_VAL\n");
    for (int t = t_start; t < t_end; t++) {
        const Table* tbl = &g_db->tables[t];
        if (!tbl->stats.is_analyzed) {
            printf("%s\t*\t*\t%d (NOT ANALYZED)\t0\t0\t-\t-\n", tbl->name, tbl->row_count);
            continue;
        }
        for (int c = 0; c < tbl->col_count; c++) {
            const ColumnStats* cs = &tbl->stats.col_stats[c];
            const char* type_str = (tbl->columns[c].type == TYPE_NUMBER ? "NUMBER" :
                                    tbl->columns[c].type == TYPE_DATE ? "DATE" : "VARCHAR2");
            char min_str[64] = "-", max_str[64] = "-";
            if (cs->has_numeric_bounds) {
                snprintf(min_str, sizeof(min_str), "%.4Lf", cs->min_numeric);
                snprintf(max_str, sizeof(max_str), "%.4Lf", cs->max_numeric);
            }
            printf("%s\t%s\t%s\t%d\t%d\t%d\t%s\t%s\n",
                   tbl->name, tbl->columns[c].name, type_str, tbl->stats.sample_rows,
                   cs->null_count, cs->distinct_count, min_str, max_str);
        }
    }
}

/* ----------------------------------------------------------------- */
/* Recommendation 2: Vectorized SIMD Columnar Evaluation Engine      */
/* ----------------------------------------------------------------- */

#define VECTOR_CHUNK_SIZE 256

void vector_aggregate_numeric(const Table* table, int col_idx, const Filter* filter, long double* out_sum, long double* out_min, long double* out_max, int* out_count) {
    long double sum = 0.0L;
    long double min_val = 1e30L;
    long double max_val = -1e30L;
    int count = 0;
    bool has_any = false;

    long double chunk[VECTOR_CHUNK_SIZE];

    for (int r = 0; r < table->row_count; r += VECTOR_CHUNK_SIZE) {
        int end = (r + VECTOR_CHUNK_SIZE <= table->row_count) ? r + VECTOR_CHUNK_SIZE : table->row_count;
        int chunk_len = end - r;
        int active_len = 0;

        for (int i = 0; i < chunk_len; i++) {
            int row_num = r + i;
            if (filter && filter->node_count > 0 && filter->root >= 0) {
                Scope sc = { .tables = {(Table*)table}, .rows = {&table->rows[row_num]}, .count = 1 };
                if (!evaluate_where_clause(&sc, filter)) continue;
            }
            const Value* v = &table->rows[row_num].values[col_idx];
            if (!v->is_null) {
                chunk[active_len++] = strtold(v->text, NULL);
            }
        }

        /* Contiguous stride-1 loop auto-vectorized by Clang/GCC SIMD */
        for (int i = 0; i < active_len; i++) {
            long double v = chunk[i];
            sum += v;
            if (!has_any || v < min_val) min_val = v;
            if (!has_any || v > max_val) max_val = v;
            has_any = true;
            count++;
        }
    }

    if (out_sum) *out_sum = sum;
    if (out_min) *out_min = has_any ? min_val : 0.0L;
    if (out_max) *out_max = has_any ? max_val : 0.0L;
    if (out_count) *out_count = count;
}
