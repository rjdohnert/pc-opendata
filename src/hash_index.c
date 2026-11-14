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
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
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
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: DROP INDEX requires the ADMIN role");

    int found = -1;
    for (int i = 0; i < db->index_count; i++) {
        if (!strcmp(db->indexes[i].name, idx_name)) {
            found = i; break;
        }
    }
    if (found < 0) return report_database_error("index not found: %s", idx_name);

    if (db->indexes[found].is_hnsw) {
        hnsw_index_destroy(&db->indexes[found]);
    } else {
        free(db->indexes[found].entries);
    }
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
            if (db->indexes[i].is_hnsw) {
                hnsw_index_build(&db->indexes[i], &db->tables[t], db->indexes[i].col_idx);
            } else {
                build_secondary_index(&db->indexes[i], &db->tables[t], db->indexes[i].col_idx);
            }
        }
    }
}

void secondary_index_insert_row(Database* db, int table_idx, int row_idx) {
    if (!db || table_idx < 0 || table_idx >= db->table_count) return;
    const Table* table = &db->tables[table_idx];
    for (int i = 0; i < db->index_count; i++) {
        SecondaryIndex* idx = &db->indexes[i];
        if (!strcmp(idx->table_name, table->name)) {
            if (idx->is_hnsw) {
                hnsw_index_insert_row(idx, table, row_idx);
                continue;
            }
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
            if (idx->is_hnsw) {
                hnsw_index_build(idx, table, idx->col_idx);
                continue;
            }
            if (!idx->entries) continue;
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
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: ANALYZE requires the ADMIN role");
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
        int max_distinct_sample = tbl->row_count > 256 ? (tbl->row_count < 4096 ? tbl->row_count : 4096) : 256;
        Value* distinct_vals = calloc((size_t)max_distinct_sample, sizeof(Value));
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
                if (distinct_cnt < max_distinct_sample) {
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

        if (type == TYPE_NUMBER && has_num) {
            long double span = max_num - min_num;
            for (int r = 0; r < tbl->row_count; r++) {
                const Value* v = &tbl->rows[r].values[c];
                if (v->is_null) continue;
                int bucket = 0;
                if (span > 0.0L) {
                    long double num = strtold(v->text, NULL);
                    long double norm = (num - min_num) / span;
                    bucket = (int)(norm * (long double)CBO_HISTOGRAM_BUCKETS);
                    if (bucket < 0) bucket = 0;
                    if (bucket >= CBO_HISTOGRAM_BUCKETS) bucket = CBO_HISTOGRAM_BUCKETS - 1;
                }
                cs->histogram[bucket]++;
            }
        }
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

double cbo_estimate_predicate_selectivity(const Table* tbl, const Predicate* pred) {
    if (!tbl || !pred) return 1.0;
    int col_idx = pred->left.column;
    if (col_idx < 0 || col_idx >= tbl->col_count) return 0.5;

    if (!tbl->stats.is_analyzed || tbl->stats.sample_rows <= 0) {
        if (pred->is_null_test) return pred->negate ? 0.95 : 0.05;
        if (pred->op == TK_EQ) return tbl->columns[col_idx].unique ? (1.0 / (double)(tbl->row_count > 0 ? tbl->row_count : 1)) : 0.10;
        if (pred->op == PRED_BETWEEN) return 0.25;
        if (pred->op == TK_LT || pred->op == TK_LE || pred->op == TK_GT || pred->op == TK_GE) return 0.33;
        return 0.50;
    }

    const ColumnStats* cs = &tbl->stats.col_stats[col_idx];
    double total = (double)tbl->stats.sample_rows;
    double non_null = total - (double)cs->null_count;
    if (non_null <= 0.0) {
        if (pred->is_null_test) return pred->negate ? 0.0 : 1.0;
        return 0.0;
    }
    if (pred->is_null_test) {
        double null_frac = (double)cs->null_count / total;
        return pred->negate ? (1.0 - null_frac) : null_frac;
    }

    if (pred->op == TK_EQ && !pred->right_column) {
        if (cs->has_numeric_bounds && !pred->value.is_null) {
            long double qv = strtold(pred->value.text, NULL);
            if (qv < cs->min_numeric || qv > cs->max_numeric) return 0.0;
        }
        int ndv = cs->distinct_count > 0 ? cs->distinct_count : 1;
        return (non_null / total) / (double)ndv;
    }

    if (cs->has_numeric_bounds && cs->max_numeric > cs->min_numeric && !pred->right_column) {
        long double low = cs->min_numeric;
        long double high = cs->max_numeric;
        bool is_range = false;
        if (pred->op == PRED_BETWEEN && !pred->value.is_null && !pred->value2.is_null) {
            low = strtold(pred->value.text, NULL);
            high = strtold(pred->value2.text, NULL);
            is_range = true;
        } else if ((pred->op == TK_GT || pred->op == TK_GE) && !pred->value.is_null) {
            low = strtold(pred->value.text, NULL);
            is_range = true;
        } else if ((pred->op == TK_LT || pred->op == TK_LE) && !pred->value.is_null) {
            high = strtold(pred->value.text, NULL);
            is_range = true;
        }
        if (is_range) {
            if (high < cs->min_numeric || low > cs->max_numeric || low > high) return 0.0;
            if (low < cs->min_numeric) low = cs->min_numeric;
            if (high > cs->max_numeric) high = cs->max_numeric;
            long double span = cs->max_numeric - cs->min_numeric;
            long double bucket_width = span / (long double)CBO_HISTOGRAM_BUCKETS;
            double matched_rows = 0.0;
            for (int b = 0; b < CBO_HISTOGRAM_BUCKETS; b++) {
                long double b_low = cs->min_numeric + (long double)b * bucket_width;
                long double b_high = (b == CBO_HISTOGRAM_BUCKETS - 1) ? cs->max_numeric : (b_low + bucket_width);
                if (high < b_low || low > b_high) continue;
                long double o_low = low > b_low ? low : b_low;
                long double o_high = high < b_high ? high : b_high;
                double frac = (bucket_width > 0.0L) ? (double)((o_high - o_low) / bucket_width) : 1.0;
                if (frac <= 0.0 && o_low == o_high) frac = 1.0 / (double)(cs->distinct_count > 0 ? cs->distinct_count : 1);
                if (frac > 1.0) frac = 1.0;
                matched_rows += (double)cs->histogram[b] * frac;
            }
            double sel = matched_rows / total;
            if (sel < 0.0) sel = 0.0;
            if (sel > 1.0) sel = 1.0;
            return sel;
        }
    }

    return 0.33;
}

static double cbo_eval_filter_node_depth(const Table* tbl, int table_scope_idx, const Filter* filter, int node_idx, int depth) {
    if (!filter || node_idx < 0 || node_idx >= filter->node_count || depth > 128) return 1.0;
    const FilterNode* fn = &filter->nodes[node_idx];
    if (fn->type == FILTER_NODE_PRED) {
        if (fn->pred.left.table != table_scope_idx) return 1.0;
        return cbo_estimate_predicate_selectivity(tbl, &fn->pred);
    }
    if (fn->type == FILTER_NODE_AND) {
        double sl = cbo_eval_filter_node_depth(tbl, table_scope_idx, filter, fn->left_child, depth + 1);
        double sr = cbo_eval_filter_node_depth(tbl, table_scope_idx, filter, fn->right_child, depth + 1);
        return sl * sr;
    }
    if (fn->type == FILTER_NODE_OR) {
        double sl = cbo_eval_filter_node_depth(tbl, table_scope_idx, filter, fn->left_child, depth + 1);
        double sr = cbo_eval_filter_node_depth(tbl, table_scope_idx, filter, fn->right_child, depth + 1);
        return sl + sr - (sl * sr);
    }
    if (fn->type == FILTER_NODE_NOT) {
        double sl = cbo_eval_filter_node_depth(tbl, table_scope_idx, filter, fn->left_child, depth + 1);
        return 1.0 - sl;
    }
    return 1.0;
}

double cbo_estimate_filter_selectivity(const Table* tbl, int table_scope_idx, const Filter* filter) {
    if (!tbl || !filter || filter->node_count <= 0 || filter->root < 0) return 1.0;
    return cbo_eval_filter_node_depth(tbl, table_scope_idx, filter, filter->root, 0);
}

double cbo_estimate_query_cost(const Database* db, const Table* tbl, int table_scope_idx, const Filter* filter, bool* out_use_index, int* out_index_id, int* out_est_rows) {
    if (out_use_index) *out_use_index = false;
    if (out_index_id) *out_index_id = -1;
    if (!tbl) {
        if (out_est_rows) *out_est_rows = 0;
        return 0.0;
    }
    double sel = cbo_estimate_filter_selectivity(tbl, table_scope_idx, filter);
    int total_rows = tbl->row_count;
    int est_rows = (int)round((double)total_rows * sel);
    if (est_rows < 1 && sel > 0.0 && total_rows > 0) est_rows = 1;
    if (out_est_rows) *out_est_rows = est_rows;

    double seq_cost = (double)total_rows * (tbl->engine_type == ENGINE_COLUMNAR ? 0.4 : 1.0) + 1.0;

    if (db && filter && filter->node_count > 0 && filter->root >= 0) {
        const FilterNode* root = &filter->nodes[filter->root];
        const Predicate* index_pred = NULL;
        if (root->type == FILTER_NODE_PRED) {
            index_pred = &root->pred;
        } else if (root->type == FILTER_NODE_AND && root->left_child >= 0 && filter->nodes[root->left_child].type == FILTER_NODE_PRED) {
            index_pred = &filter->nodes[root->left_child].pred;
        }
        if (index_pred && !index_pred->right_column && index_pred->left.table == table_scope_idx) {
            int op = index_pred->op;
            if (op == TK_EQ || op == PRED_BETWEEN || op == TK_GT || op == TK_GE || op == TK_LT || op == TK_LE) {
                int sec_idx = find_secondary_index(db, tbl->name, index_pred->left.column);
                if (sec_idx >= 0 && db->indexes[sec_idx].entry_count > 0) {
                    double pred_sel = cbo_estimate_predicate_selectivity(tbl, index_pred);
                    double idx_rows = (double)total_rows * pred_sel;
                    double idx_cost = log2((double)(total_rows > 1 ? total_rows : 2)) + idx_rows * 1.4;
                    if (!tbl->stats.is_analyzed || idx_cost < seq_cost || pred_sel <= 0.45) {
                        if (out_use_index) *out_use_index = true;
                        if (out_index_id) *out_index_id = sec_idx;
                        return idx_cost;
                    }
                }
            }
        }
    }
    return seq_cost;
}

int get_row_partition(const Table* tbl, const Row* row) {
    if (!tbl || !row || tbl->part_type == PART_NONE || tbl->part_count <= 0) return 0;
    int col_idx = tbl->part_col_idx;
    if (col_idx < 0 || col_idx >= tbl->col_count) return 0;
    const Value* val = &row->values[col_idx];
    DataType type = tbl->columns[col_idx].type;

    if (tbl->part_type == PART_HASH) {
        if (val->is_null) return 0;
        uint32_t h = hash_typed_value(type, val);
        return (int)(h % (uint32_t)tbl->part_count);
    }

    if (tbl->part_type == PART_RANGE) {
        if (val->is_null) return tbl->part_count - 1;
        for (int p = 0; p < tbl->part_count; p++) {
            if (tbl->partitions[p].is_maxvalue) return p;
            Value ub = {0};
            snprintf(ub.text, sizeof(ub.text), "%s", tbl->partitions[p].upper_bound);
            ub.is_null = false;
            if (compare_typed_values(type, val, &ub) < 0) return p;
        }
        return -1;
    }
    return 0;
}

static uint32_t prune_partitions_for_pred(const Table* tbl, int table_scope_idx, const Predicate* pred) {
    uint32_t all_mask = (tbl->part_count >= 32) ? 0xFFFFFFFFu : ((1u << (unsigned)tbl->part_count) - 1u);
    if (!pred || pred->right_column || pred->left.table != table_scope_idx || pred->left.column != tbl->part_col_idx) {
        return all_mask;
    }
    if (pred->is_null_test) return all_mask;
    DataType type = tbl->columns[tbl->part_col_idx].type;

    if (tbl->part_type == PART_HASH) {
        if (pred->op == TK_EQ && !pred->value.is_null) {
            uint32_t h = hash_typed_value(type, &pred->value);
            int p = (int)(h % (uint32_t)tbl->part_count);
            return (1u << (unsigned)p);
        }
        return all_mask;
    }

    if (tbl->part_type == PART_RANGE) {
        uint32_t mask = 0;
        for (int p = 0; p < tbl->part_count; p++) {
            bool has_low = (p > 0);
            Value p_low = {0};
            if (has_low) {
                snprintf(p_low.text, sizeof(p_low.text), "%s", tbl->partitions[p - 1].upper_bound);
                p_low.is_null = false;
            }
            bool has_high = !tbl->partitions[p].is_maxvalue;
            Value p_high = {0};
            if (has_high) {
                snprintf(p_high.text, sizeof(p_high.text), "%s", tbl->partitions[p].upper_bound);
                p_high.is_null = false;
            }

            bool overlap = true;
            if (pred->op == TK_EQ && !pred->value.is_null) {
                if (has_low && compare_typed_values(type, &pred->value, &p_low) < 0) overlap = false;
                if (has_high && compare_typed_values(type, &pred->value, &p_high) >= 0) overlap = false;
            } else if ((pred->op == TK_LT || pred->op == TK_LE) && !pred->value.is_null) {
                if (has_low) {
                    int cmp = compare_typed_values(type, &pred->value, &p_low);
                    if (pred->op == TK_LT ? (cmp <= 0) : (cmp < 0)) overlap = false;
                }
            } else if ((pred->op == TK_GT || pred->op == TK_GE) && !pred->value.is_null) {
                if (has_high && compare_typed_values(type, &pred->value, &p_high) >= 0) overlap = false;
            } else if (pred->op == PRED_BETWEEN && !pred->value.is_null && !pred->value2.is_null) {
                if (has_low && compare_typed_values(type, &pred->value2, &p_low) < 0) overlap = false;
                if (has_high && compare_typed_values(type, &pred->value, &p_high) >= 0) overlap = false;
            }
            if (overlap) mask |= (1u << (unsigned)p);
        }
        return mask;
    }

    return all_mask;
}

static uint32_t prune_partitions_node(const Table* tbl, int table_scope_idx, const Filter* filter, int node_idx, int depth) {
    uint32_t all_mask = (tbl->part_count >= 32) ? 0xFFFFFFFFu : ((1u << (unsigned)tbl->part_count) - 1u);
    if (!filter || node_idx < 0 || node_idx >= filter->node_count || depth > 128) return all_mask;
    const FilterNode* fn = &filter->nodes[node_idx];
    if (fn->type == FILTER_NODE_PRED) {
        return prune_partitions_for_pred(tbl, table_scope_idx, &fn->pred);
    }
    if (fn->type == FILTER_NODE_AND) {
        return prune_partitions_node(tbl, table_scope_idx, filter, fn->left_child, depth + 1) &
               prune_partitions_node(tbl, table_scope_idx, filter, fn->right_child, depth + 1);
    }
    if (fn->type == FILTER_NODE_OR) {
        return prune_partitions_node(tbl, table_scope_idx, filter, fn->left_child, depth + 1) |
               prune_partitions_node(tbl, table_scope_idx, filter, fn->right_child, depth + 1);
    }
    return all_mask;
}

uint32_t prune_partitions_for_filter(const Table* tbl, int table_scope_idx, const Filter* filter) {
    if (!tbl || tbl->part_type == PART_NONE || tbl->part_count <= 0) return 0xFFFFFFFFu;
    uint32_t all_mask = (tbl->part_count >= 32) ? 0xFFFFFFFFu : ((1u << (unsigned)tbl->part_count) - 1u);
    if (!filter || filter->node_count <= 0 || filter->root < 0) return all_mask;
    return prune_partitions_node(tbl, table_scope_idx, filter, filter->root, 0);
}

void vector_aggregate_numeric_partitioned(const Table* table, int col_idx, const Filter* filter, uint32_t active_part_mask, long double* out_sum, long double* out_min, long double* out_max, int* out_count) {
    long double sum = 0.0L;
    long double min_val = 1e30L;
    long double max_val = -1e30L;
    int count = 0;
    bool has_any = false;

    /* Columnar spillover: materialize a contiguous stride-1 vector when scanning
       large or ENGINE_COLUMNAR tables, skipping pruned partitions upfront. */
    int total_rows = table->row_count;
    bool check_parts = (table->part_type != PART_NONE && table->part_count > 0);
    long double* col_spill = NULL;
    if ((table->engine_type == ENGINE_COLUMNAR || total_rows > VECTOR_CHUNK_SIZE) && total_rows > 0) {
        col_spill = malloc((size_t)total_rows * sizeof(long double));
    }

    if (col_spill) {
        int active_len = 0;
        for (int r = 0; r < total_rows; r++) {
            if (check_parts) {
                int p = get_row_partition(table, &table->rows[r]);
                if (p < 0 || !(active_part_mask & (1u << (unsigned)p))) continue;
            }
            if (filter && filter->node_count > 0 && filter->root >= 0) {
                Scope sc = { .tables = {(Table*)table}, .rows = {&table->rows[r]}, .count = 1 };
                if (!evaluate_where_clause(&sc, filter)) continue;
            }
            const Value* v = &table->rows[r].values[col_idx];
            if (!v->is_null) {
                col_spill[active_len++] = strtold(v->text, NULL);
            }
        }
        for (int i = 0; i < active_len; i++) {
            long double v = col_spill[i];
            sum += v;
            if (!has_any || v < min_val) min_val = v;
            if (!has_any || v > max_val) max_val = v;
            has_any = true;
            count++;
        }
        free(col_spill);
    } else {
        long double chunk[VECTOR_CHUNK_SIZE];
        for (int r = 0; r < total_rows; r += VECTOR_CHUNK_SIZE) {
            int end = (r + VECTOR_CHUNK_SIZE <= total_rows) ? r + VECTOR_CHUNK_SIZE : total_rows;
            int chunk_len = end - r;
            int active_len = 0;

            for (int i = 0; i < chunk_len; i++) {
                int row_num = r + i;
                if (check_parts) {
                    int p = get_row_partition(table, &table->rows[row_num]);
                    if (p < 0 || !(active_part_mask & (1u << (unsigned)p))) continue;
                }
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
    }

    if (out_sum) *out_sum = sum;
    if (out_min) *out_min = has_any ? min_val : 0.0L;
    if (out_max) *out_max = has_any ? max_val : 0.0L;
    if (out_count) *out_count = count;
}

void vector_aggregate_numeric(const Table* table, int col_idx, const Filter* filter, long double* out_sum, long double* out_min, long double* out_max, int* out_count) {
    uint32_t mask = prune_partitions_for_filter(table, 0, filter);
    vector_aggregate_numeric_partitioned(table, col_idx, filter, mask, out_sum, out_min, out_max, out_count);
}
