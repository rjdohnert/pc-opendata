#include "database_engine.h"

THREAD_LOCAL QueryResultCapture* g_result_capture = NULL;

static void print_escaped_cell_to(FILE* out, const char* text) {
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '\\') fputs("\\\\", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c < 32 || c == 127) fprintf(out, "\\x%02X", (unsigned)c);
        else fputc(c, out);
    }
}

static void print_escaped_cell(const char* text) {
    print_escaped_cell_to(stdout, text);
    if (g_spool_file) print_escaped_cell_to(g_spool_file, text);
}

typedef enum {
    PROJ_COL,
    PROJ_COUNT_STAR,
    PROJ_COUNT_COL,
    PROJ_SUM,
    PROJ_AVG,
    PROJ_MIN,
    PROJ_MAX,
    PROJ_JSON_EXTRACT,
    PROJ_ROW_NUMBER,
    PROJ_RANK,
    PROJ_DENSE_RANK,
    PROJ_UPPER,
    PROJ_LOWER,
    PROJ_LENGTH,
    PROJ_TRIM,
    PROJ_SUBSTR,
    PROJ_CONCAT,
    PROJ_ABS,
    PROJ_ROUND,
    PROJ_CEIL,
    PROJ_FLOOR,
    PROJ_MOD,
    PROJ_COALESCE,
    PROJ_NVL,
    PROJ_NVL2,
    PROJ_IDENTITY,
    PROJ_CASE
} ProjType;

typedef struct {
    ColumnName raw_name;
    Binding binding;
    TokenKind op;
    int is_null_check;
    Value when_val;
    Value then_val;
} CaseBranch;

#define MAX_CASE_BRANCHES 8

typedef struct {
    ProjType type;
    Binding binding;
    ColumnName raw_name;
    char alias[MAX_NAME_LEN];
    char display_name[MAX_NAME_LEN * 2 + 32];
    DataType data_type;
    bool is_hidden;
    char expr_op;
    Value expr_operand;
    char json_path[MAX_VAL_LEN];
    bool is_window;
    ColumnName partition_name;
    Binding partition_binding;
    bool has_partition;

    int substr_start;
    int substr_len;
    int round_decimals;
    long double mod_divisor;
    ColumnName raw_name2;
    Binding binding2;
    bool has_binding2;
    Value literal1;
    bool has_literal1;
    Value literal2;
    bool has_literal2;
    Value literal3;
    bool has_literal3;
    ColumnName raw_name3;
    Binding binding3;
    bool has_binding3;

    CaseBranch case_branches[MAX_CASE_BRANCHES];
    int case_branch_count;
    Value case_else_val;
    bool case_has_else;
} ProjItem;

typedef struct {
    Value cells[MAX_COLUMNS];
} RowData;

typedef struct {
    int proj_idx;
    bool desc;
    DataType type;
} SortKey;

static int compare_rows_with_keys(const RowData* a, const RowData* b, const SortKey* keys, int key_count) {
    for (int k = 0; k < key_count; k++) {
        int idx = keys[k].proj_idx;
        const Value* va = &a->cells[idx];
        const Value* vb = &b->cells[idx];
        if (va->is_null && vb->is_null) continue;
        if (va->is_null) return keys[k].desc ? -1 : 1;
        if (vb->is_null) return keys[k].desc ? 1 : -1;
        int cmp = compare_typed_values(keys[k].type, va, vb);
        if (cmp != 0) return keys[k].desc ? -cmp : cmp;
    }
    return 0;
}

static void sort_row_data(RowData* rows, int count, const SortKey* keys, int key_count) {
    if (count <= 1 || key_count <= 0) return;
    for (int gap = count / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < count; i++) {
            RowData temp = rows[i];
            int j;
            for (j = i; j >= gap && compare_rows_with_keys(&rows[j - gap], &temp, keys, key_count) > 0; j -= gap) {
                rows[j] = rows[j - gap];
            }
            rows[j] = temp;
        }
    }
}

typedef struct {
    Value group_keys[MAX_COLUMNS];
    int count_star;
    int col_counts[MAX_COLUMNS];
    long double sums[MAX_COLUMNS];
    Value mins[MAX_COLUMNS];
    Value maxs[MAX_COLUMNS];
    bool has_min[MAX_COLUMNS];
    bool has_max[MAX_COLUMNS];
} GroupAccumulator;

typedef struct {
    ProjType agg_type;
    ColumnName col_ref;
    TokenKind op;
    long double target_val;
    int proj_idx;
} HavingCond;

#define MAX_HAVING_CONDS 8

typedef struct {
    HavingCond conds[MAX_HAVING_CONDS];
    int count;
} HavingClause;

typedef enum {
    JOIN_INNER,
    JOIN_LEFT,
    JOIN_FULL,
    JOIN_CROSS
} JoinType;

typedef enum {
    SET_OP_NONE,
    SET_OP_UNION,
    SET_OP_UNION_ALL,
    SET_OP_INTERSECT,
    SET_OP_MINUS
} SetOperation;

static void format_decimal_val(long double val, char* out, size_t out_cap) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%.10Lf", val);
    char* dot = strchr(buf, '.');
    if (dot) {
        char* end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';
    }
    if (!normalize_decimal(buf, out)) {
        snprintf(out, out_cap, "%s", buf);
    }
}

static void evaluate_scalar_item(const ProjItem* item, const Value* src, const Value* src2, Value* out) {
    memset(out, 0, sizeof(*out));
    switch (item->type) {
    case PROJ_UPPER: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        out->is_null = false;
        size_t len = strlen(in->text);
        if (len >= MAX_VAL_LEN) len = MAX_VAL_LEN - 1;
        for (size_t i = 0; i < len; i++) out->text[i] = (char)toupper((unsigned char)in->text[i]);
        out->text[len] = '\0';
        break;
    }
    case PROJ_LOWER: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        out->is_null = false;
        size_t len = strlen(in->text);
        if (len >= MAX_VAL_LEN) len = MAX_VAL_LEN - 1;
        for (size_t i = 0; i < len; i++) out->text[i] = (char)tolower((unsigned char)in->text[i]);
        out->text[len] = '\0';
        break;
    }
    case PROJ_LENGTH: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        out->is_null = false;
        snprintf(out->text, sizeof(out->text), "%zu", strlen(in->text));
        break;
    }
    case PROJ_TRIM: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        const char* start = in->text;
        while (*start && isspace((unsigned char)*start)) start++;
        const char* end = in->text + strlen(in->text);
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
        size_t len = (size_t)(end - start);
        if (len >= MAX_VAL_LEN) len = MAX_VAL_LEN - 1;
        memcpy(out->text, start, len);
        out->text[len] = '\0';
        out->is_null = false;
        break;
    }
    case PROJ_SUBSTR: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        int slen = (int)strlen(in->text);
        int start_idx = 0;
        if (item->substr_start > 0) start_idx = item->substr_start - 1;
        else if (item->substr_start < 0) start_idx = slen + item->substr_start;
        if (start_idx < 0) start_idx = 0;
        if (start_idx >= slen) {
            out->is_null = false;
            out->text[0] = '\0';
            return;
        }
        int sub_len = (item->substr_len >= 0) ? item->substr_len : (slen - start_idx);
        if (start_idx + sub_len > slen) sub_len = slen - start_idx;
        if (sub_len < 0) sub_len = 0;
        if (sub_len >= MAX_VAL_LEN) sub_len = MAX_VAL_LEN - 1;
        memcpy(out->text, in->text + start_idx, (size_t)sub_len);
        out->text[sub_len] = '\0';
        out->is_null = false;
        break;
    }
    case PROJ_CONCAT: {
        const char* t1 = item->has_literal1 ? item->literal1.text : (src && !src->is_null ? src->text : "");
        const char* t2 = item->has_binding2 ? (src2 && !src2->is_null ? src2->text : "") : (item->literal2.is_null ? "" : item->literal2.text);
        snprintf(out->text, sizeof(out->text), "%s%s", t1, t2);
        out->is_null = false;
        break;
    }
    case PROJ_ABS: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        long double v = fabsl(strtold(in->text, NULL));
        format_decimal_val(v, out->text, sizeof(out->text));
        out->is_null = false;
        break;
    }
    case PROJ_ROUND: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        long double v = strtold(in->text, NULL);
        long double factor = powl(10.0L, (long double)item->round_decimals);
        long double rounded = roundl(v * factor) / factor;
        format_decimal_val(rounded, out->text, sizeof(out->text));
        out->is_null = false;
        break;
    }
    case PROJ_CEIL: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        long double v = ceill(strtold(in->text, NULL));
        format_decimal_val(v, out->text, sizeof(out->text));
        out->is_null = false;
        break;
    }
    case PROJ_FLOOR: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        long double v = floorl(strtold(in->text, NULL));
        format_decimal_val(v, out->text, sizeof(out->text));
        out->is_null = false;
        break;
    }
    case PROJ_MOD: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null || item->mod_divisor == 0.0L) { out->is_null = true; return; }
        long double v = fmodl(strtold(in->text, NULL), item->mod_divisor);
        format_decimal_val(v, out->text, sizeof(out->text));
        out->is_null = false;
        break;
    }
    case PROJ_COALESCE:
    case PROJ_NVL: {
        const Value* in1 = item->has_literal1 ? &item->literal1 : src;
        const Value* in2 = item->has_binding2 ? src2 : &item->literal2;
        if (in1 && !in1->is_null) {
            *out = *in1;
        } else if (in2 && !in2->is_null) {
            *out = *in2;
        } else {
            out->is_null = true;
        }
        break;
    }
    case PROJ_NVL2: {
        const Value* in1 = item->has_literal1 ? &item->literal1 : src;
        const Value* in2 = item->has_binding2 ? src2 : &item->literal2;
        const Value* in3 = &item->literal3;
        if (in1 && !in1->is_null) {
            if (in2 && !in2->is_null) *out = *in2;
            else out->is_null = true;
        } else {
            if (in3 && !in3->is_null) *out = *in3;
            else out->is_null = true;
        }
        break;
    }
    case PROJ_IDENTITY: {
        const Value* in = item->has_literal1 ? &item->literal1 : src;
        if (!in || in->is_null) { out->is_null = true; return; }
        *out = *in;
        break;
    }
    default:
        out->is_null = true;
        break;
    }
}

static void evaluate_case_item(const ProjItem* item, const Scope* scope, Value* out) {
    memset(out, 0, sizeof(*out));
    for (int b = 0; b < item->case_branch_count; b++) {
        const CaseBranch* cb = &item->case_branches[b];
        /* Outer-scope bindings carry the 0x1000 tag; resolve them against
           g_outer_scope instead of indexing the local scope arrays. */
        const Scope* bind_scope = (cb->binding.table & 0x1000) ? g_outer_scope : scope;
        int t = cb->binding.table & ~0x1000;
        if (!bind_scope || t < 0 || t >= bind_scope->count) { out->is_null = true; return; }
        const Value* val = (bind_scope->rows[t] == NULL) ? NULL : &bind_scope->rows[t]->values[cb->binding.column];
        if (cb->is_null_check == 1) {
            if (!val || val->is_null) { *out = cb->then_val; return; }
        } else if (cb->is_null_check == 2) {
            if (val && !val->is_null) { *out = cb->then_val; return; }
        } else {
            if (!val || val->is_null) continue;
            DataType type = bind_scope->tables[t]->columns[cb->binding.column].type;
            int cmp = compare_typed_values(type, val, &cb->when_val);
            bool match = false;
            switch (cb->op) {
            case TK_EQ: match = (cmp == 0); break;
            case TK_NE: match = (cmp != 0); break;
            case TK_LT: match = (cmp < 0); break;
            case TK_LE: match = (cmp <= 0); break;
            case TK_GT: match = (cmp > 0); break;
            case TK_GE: match = (cmp >= 0); break;
            default: break;
            }
            if (match) { *out = cb->then_val; return; }
        }
    }
    if (item->case_has_else) {
        *out = item->case_else_val;
    } else {
        out->is_null = true;
    }
}

typedef struct { int indices[MAX_SCOPE_TABLES]; } ResultTuple;
static int g_select_max_needed = MAX_RESULTS;

static void emit_tuples(
    const Scope* scope,
    const Predicate* joins,
    const JoinType* join_types,
    const bool* use_hash,
    const JoinIndex* join_indexes,
    const int* left_cols,
    const Filter* filter,
    int table_idx,
    int* current_tuple,
    ResultTuple* results,
    int* selected,
    int* pairs,
    bool* work_limit_reached,
    bool* right_matched)
{
    if (*selected >= g_select_max_needed || *work_limit_reached) return;
    if (is_query_timed_out()) {
        *work_limit_reached = true;
        report_database_error("query canceled due to statement timeout (%u ms)", g_statement_timeout_ms);
        return;
    }

    if (table_idx == scope->count) {
        Scope sc = *scope;
        for (int t = 0; t < sc.count; t++) {
            sc.rows[t] = current_tuple[t] >= 0 ? &sc.tables[t]->rows[current_tuple[t]] : NULL;
        }
        if (evaluate_where_clause(&sc, filter)) {
            for (int t = 0; t < sc.count; t++) {
                results[*selected].indices[t] = current_tuple[t];
            }
            (*selected)++;
        }
        return;
    }

    if (table_idx == 0) {
        int* candidate_rows = NULL;
        int candidate_count = 0;
        bool used_secondary_index = false;
        const Table* t0 = scope->tables[0];
        bool has_parts = (t0->part_type != PART_NONE && t0->part_count > 0);
        uint32_t part_mask = has_parts ? prune_partitions_for_filter(t0, 0, filter) : 0xFFFFFFFFu;
        if (has_parts && part_mask == 0) return;

        bool cbo_use_idx = false;
        int sec_idx = -1;
        int est_rows = 0;
        cbo_estimate_query_cost(g_db, t0, 0, filter, &cbo_use_idx, &sec_idx, &est_rows);

        if (cbo_use_idx && sec_idx >= 0 && g_db && g_db->indexes[sec_idx].entry_count > 0) {
            const FilterNode* root = &filter->nodes[filter->root];
            const Predicate* pred = (root->type == FILTER_NODE_PRED) ? &root->pred :
                ((root->type == FILTER_NODE_AND && root->left_child >= 0) ? &filter->nodes[root->left_child].pred : NULL);
            if (pred && !pred->right_column && pred->left.table == 0) {
                candidate_rows = (int*)malloc((size_t)g_db->indexes[sec_idx].entry_count * sizeof(int));
                if (candidate_rows) {
                    DataType col_type = t0->columns[pred->left.column].type;
                    if (pred->op == TK_EQ) {
                        candidate_count = secondary_index_find_range(&g_db->indexes[sec_idx], col_type, &pred->value, &pred->value, candidate_rows, g_db->indexes[sec_idx].entry_count);
                        used_secondary_index = true;
                    } else if (pred->op == PRED_BETWEEN) {
                        candidate_count = secondary_index_find_range(&g_db->indexes[sec_idx], col_type, &pred->value, &pred->value2, candidate_rows, g_db->indexes[sec_idx].entry_count);
                        used_secondary_index = true;
                    } else if (pred->op == TK_GT || pred->op == TK_GE) {
                        candidate_count = secondary_index_find_range(&g_db->indexes[sec_idx], col_type, &pred->value, NULL, candidate_rows, g_db->indexes[sec_idx].entry_count);
                        used_secondary_index = true;
                    } else if (pred->op == TK_LT || pred->op == TK_LE) {
                        candidate_count = secondary_index_find_range(&g_db->indexes[sec_idx], col_type, NULL, &pred->value, candidate_rows, g_db->indexes[sec_idx].entry_count);
                        used_secondary_index = true;
                    } else {
                        free(candidate_rows);
                        candidate_rows = NULL;
                    }
                }
            }
        }

        if (used_secondary_index && candidate_rows) {
            for (int k = 0; k < candidate_count; k++) {
                int r_idx = candidate_rows[k];
                if (has_parts) {
                    int p = get_row_partition(t0, &t0->rows[r_idx]);
                    if (p < 0 || !(part_mask & (1u << (unsigned)p))) continue;
                }
                current_tuple[0] = r_idx;
                emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, 1, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
                if (*selected >= g_select_max_needed || *work_limit_reached) break;
            }
            free(candidate_rows);
            return;
        }
        if (candidate_rows) free(candidate_rows);

        for (int r0 = 0; r0 < t0->row_count; r0++) {
            if (has_parts) {
                int p = get_row_partition(t0, &t0->rows[r0]);
                if (p < 0 || !(part_mask & (1u << (unsigned)p))) continue;
            }
            current_tuple[0] = r0;
            emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, 1, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
            if (*selected >= g_select_max_needed || *work_limit_reached) return;
    if (is_query_timed_out()) {
        *work_limit_reached = true;
        report_database_error("query canceled due to statement timeout (%u ms)", g_statement_timeout_ms);
        return;
    }
        }
        if (scope->count > 1 && join_types[1] == JOIN_FULL && right_matched) {
            for (int r1 = 0; r1 < scope->tables[1]->row_count; r1++) {
                if (!right_matched[r1]) {
                    current_tuple[0] = -1;
                    current_tuple[1] = r1;
                    emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, 2, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
                    if (*selected >= g_select_max_needed || *work_limit_reached) return;
    if (is_query_timed_out()) {
        *work_limit_reached = true;
        report_database_error("query canceled due to statement timeout (%u ms)", g_statement_timeout_ms);
        return;
    }
                }
            }
        }
        return;
    }

    if (join_types[table_idx] == JOIN_CROSS) {
        for (int b = 0; b < scope->tables[table_idx]->row_count; b++) {
            if (++(*pairs) > MAX_PAIRS) { *work_limit_reached = true; return; }
            current_tuple[table_idx] = b;
            emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, table_idx + 1, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
            if (*selected >= g_select_max_needed || *work_limit_reached) return;
    if (is_query_timed_out()) {
        *work_limit_reached = true;
        report_database_error("query canceled due to statement timeout (%u ms)", g_statement_timeout_ms);
        return;
    }
        }
        return;
    }

    int matched = 0;
    Scope sc = *scope;
    for (int t = 0; t < table_idx; t++) {
        sc.rows[t] = current_tuple[t] >= 0 ? &sc.tables[t]->rows[current_tuple[t]] : NULL;
    }

    if (use_hash[table_idx]) {
        int left_t = (joins[table_idx].left.table == table_idx) ? joins[table_idx].right.table : joins[table_idx].left.table;
        if (left_t < table_idx && current_tuple[left_t] >= 0) {
            const Value* left_val = &scope->tables[left_t]->rows[current_tuple[left_t]].values[left_cols[table_idx]];
            if (!left_val->is_null) {
                int e = join_index_first(&join_indexes[table_idx], left_val);
                while (e >= 0) {
                    int b = join_indexes[table_idx].entries[e].row;
                    if (++(*pairs) > MAX_PAIRS) { *work_limit_reached = true; return; }
                    current_tuple[table_idx] = b;
                    sc.rows[table_idx] = &scope->tables[table_idx]->rows[b];
                    if (evaluate_comparison_predicate(&sc, &joins[table_idx])) {
                        matched++;
                        if (table_idx == 1 && right_matched) right_matched[b] = true;
                        emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, table_idx + 1, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
                        if (*selected >= g_select_max_needed || *work_limit_reached) return;
    if (is_query_timed_out()) {
        *work_limit_reached = true;
        report_database_error("query canceled due to statement timeout (%u ms)", g_statement_timeout_ms);
        return;
    }
                    }
                    e = join_indexes[table_idx].entries[e].next;
                }
            }
        }
    } else {
        for (int b = 0; b < scope->tables[table_idx]->row_count; b++) {
            if (++(*pairs) > MAX_PAIRS) { *work_limit_reached = true; return; }
            current_tuple[table_idx] = b;
            sc.rows[table_idx] = &scope->tables[table_idx]->rows[b];
            if (evaluate_comparison_predicate(&sc, &joins[table_idx])) {
                matched++;
                if (table_idx == 1 && right_matched) right_matched[b] = true;
                emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, table_idx + 1, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
                if (*selected >= g_select_max_needed || *work_limit_reached) return;
    if (is_query_timed_out()) {
        *work_limit_reached = true;
        report_database_error("query canceled due to statement timeout (%u ms)", g_statement_timeout_ms);
        return;
    }
            }
        }
    }

    if ((join_types[table_idx] == JOIN_LEFT || join_types[table_idx] == JOIN_FULL) && matched == 0) {
        current_tuple[table_idx] = -1;
        emit_tuples(scope, joins, join_types, use_hash, join_indexes, left_cols, filter, table_idx + 1, current_tuple, results, selected, pairs, work_limit_reached, right_matched);
    }
}

static bool check_query_pager(int printed, int total_remaining, int* lines_since_prompt) {
    if (!g_interactive_shell || !g_pager_enabled) return false;
    if (total_remaining <= 25) return false;
    if (printed <= 0) return false;
    if (*lines_since_prompt < 25) return false;

    printf("-- More (%d remaining) [Enter: 1 row, Space: page, q: quit] --", total_remaining);
    fflush(stdout);
    int pk = shell_read_key();
    printf("\r                                                                               \r");
    fflush(stdout);
    if (pk == 'q' || pk == 'Q' || pk == SHELL_KEY_CTRL_C) {
        printf("\n[Paging stopped by user]\n");
        return true;
    }
    if (pk == SHELL_KEY_ENTER || pk == '\n') {
        *lines_since_prompt = 24;
    } else {
        *lines_since_prompt = 0;
    }
    return false;
}

static void render_capture_to_output(const QueryResultCapture* cap, int limit, int offset) {
    if (cap->silent) return;

    int total_rows = cap->row_count;
    if (g_output_format == FORMAT_CSV) {
        for (int c = 0; c < cap->col_count; c++) {
            if (c > 0) putchar(',');
            fputs(cap->col_names[c], stdout);
        }
        putchar('\n');
        int printed = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            for (int c = 0; c < cap->col_count; c++) {
                if (c > 0) putchar(',');
                if (!cap->rows[r][c].is_null) {
                    bool quote = false;
                    for (const char* cp = cap->rows[r][c].text; *cp; cp++) {
                        if (*cp == ',' || *cp == '"' || *cp == '\n' || *cp == '\r') { quote = true; break; }
                    }
                    if (quote) {
                        putchar('"');
                        for (const char* cp = cap->rows[r][c].text; *cp; cp++) {
                            if (*cp == '"') putchar('"');
                            putchar(*cp);
                        }
                        putchar('"');
                    } else {
                        fputs(cap->rows[r][c].text, stdout);
                    }
                }
            }
            putchar('\n');
            printed++;
        }
    } else if (g_output_format == FORMAT_JSON) {
        puts("[");
        int printed = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            fputs("  {", stdout);
            for (int c = 0; c < cap->col_count; c++) {
                if (c > 0) fputs(", ", stdout);
                printf("\"%s\": ", cap->col_names[c]);
                if (cap->rows[r][c].is_null) {
                    fputs("null", stdout);
                } else {
                    putchar('"');
                    for (const char* cp = cap->rows[r][c].text; *cp; cp++) {
                        if (*cp == '"') fputs("\\\"", stdout);
                        else if (*cp == '\\') fputs("\\\\", stdout);
                        else if (*cp == '\n') fputs("\\n", stdout);
                        else if (*cp == '\r') fputs("\\r", stdout);
                        else if (*cp == '\t') fputs("\\t", stdout);
                        else putchar(*cp);
                    }
                    putchar('"');
                }
            }
            printed++;
            if (r + 1 < total_rows && printed < limit) puts("},");
            else puts("}");
        }
        puts("]");
    } else if (g_output_format == FORMAT_BOX || (g_interactive_shell && g_output_format == FORMAT_TABLE)) {
        int widths[MAX_COLUMNS];
        for (int c = 0; c < cap->col_count; c++) {
            widths[c] = (int)strlen(cap->col_names[c]);
        }
        int printed = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            for (int c = 0; c < cap->col_count; c++) {
                int len = cap->rows[r][c].is_null ? (int)strlen(g_null_str) : (int)strlen(cap->rows[r][c].text);
                if (len > widths[c]) widths[c] = len;
            }
            printed++;
        }
        fputs("+", stdout);
        for (int c = 0; c < cap->col_count; c++) {
            for (int i = 0; i < widths[c] + 2; i++) fputc('-', stdout);
            fputc('+', stdout);
        }
        fputc('\n', stdout);
        fputs("|", stdout);
        for (int c = 0; c < cap->col_count; c++) {
            printf(" %-*s |", widths[c], cap->col_names[c]);
        }
        fputc('\n', stdout);
        fputs("+", stdout);
        for (int c = 0; c < cap->col_count; c++) {
            for (int i = 0; i < widths[c] + 2; i++) fputc('-', stdout);
            fputc('+', stdout);
        }
        fputc('\n', stdout);
        printed = 0;
        int lines_since_prompt = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            if (check_query_pager(printed, total_rows - r, &lines_since_prompt)) break;
            fputs("|", stdout);
            for (int c = 0; c < cap->col_count; c++) {
                const char* txt = cap->rows[r][c].is_null ? g_null_str : cap->rows[r][c].text;
                printf(" %-*s |", widths[c], txt);
            }
            fputc('\n', stdout);
            printed++;
            lines_since_prompt++;
        }
        fputs("+", stdout);
        for (int c = 0; c < cap->col_count; c++) {
            for (int i = 0; i < widths[c] + 2; i++) fputc('-', stdout);
            fputc('+', stdout);
        }
        fputc('\n', stdout);
        printf("\n%d row%s selected.\n", printed, printed == 1 ? "" : "s");
    } else if (g_output_format == FORMAT_MARKDOWN) {
        fputs("|", stdout);
        for (int c = 0; c < cap->col_count; c++) {
            printf(" %s |", cap->col_names[c]);
        }
        fputc('\n', stdout);
        fputs("|", stdout);
        for (int c = 0; c < cap->col_count; c++) {
            fputs(" --- |", stdout);
        }
        fputc('\n', stdout);
        int printed = 0;
        int lines_since_prompt = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            if (check_query_pager(printed, total_rows - r, &lines_since_prompt)) break;
            fputs("|", stdout);
            for (int c = 0; c < cap->col_count; c++) {
                const char* txt = cap->rows[r][c].is_null ? g_null_str : cap->rows[r][c].text;
                printf(" %s |", txt);
            }
            fputc('\n', stdout);
            printed++;
            lines_since_prompt++;
        }
        printf("\n%d row%s selected.\n", printed, printed == 1 ? "" : "s");
    } else if (g_output_format == FORMAT_VERTICAL) {
        int max_name_len = 0;
        for (int c = 0; c < cap->col_count; c++) {
            int len = (int)strlen(cap->col_names[c]);
            if (len > max_name_len) max_name_len = len;
        }
        int printed = 0;
        int lines_since_prompt = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            if (check_query_pager(printed, total_rows - r, &lines_since_prompt)) break;
            printf("*************************** %d. row ***************************\n", printed + 1);
            for (int c = 0; c < cap->col_count; c++) {
                const char* txt = cap->rows[r][c].is_null ? g_null_str : cap->rows[r][c].text;
                printf("%*s: %s\n", max_name_len, cap->col_names[c], txt);
            }
            printed++;
            lines_since_prompt++;
        }
        printf("\n%d row%s selected.\n", printed, printed == 1 ? "" : "s");
    } else {
        for (int c = 0; c < cap->col_count; c++) {
            if (c > 0) {
                putchar('\t');
                if (g_spool_file) fputc('\t', g_spool_file);
            }
            fputs(cap->col_names[c], stdout);
            if (g_spool_file) fputs(cap->col_names[c], g_spool_file);
        }
        putchar('\n');
        if (g_spool_file) fputc('\n', g_spool_file);

        int printed = 0;
        for (int r = offset; r < total_rows && printed < limit; r++) {
            for (int c = 0; c < cap->col_count; c++) {
                if (c > 0) {
                    putchar('\t');
                    if (g_spool_file) fputc('\t', g_spool_file);
                }
                if (cap->rows[r][c].is_null) {
                    fputs(g_null_str, stdout);
                    if (g_spool_file) fputs(g_null_str, g_spool_file);
                } else {
                    print_escaped_cell(cap->rows[r][c].text);
                }
            }
            putchar('\n');
            if (g_spool_file) fputc('\n', g_spool_file);
            printed++;
        }
        printf("\n%d row%s selected.\n", printed, printed == 1 ? "" : "s");
        if (g_spool_file) {
            fprintf(g_spool_file, "\n%d row%s selected.\n", printed, printed == 1 ? "" : "s");
            fflush(g_spool_file);
        }
    }
}

static bool row_equals(const Value* a, const Value* b, int col_count) {
    for (int c = 0; c < col_count; c++) {
        if (a[c].is_null != b[c].is_null) return false;
        if (!a[c].is_null && strcmp(a[c].text, b[c].text) != 0) return false;
    }
    return true;
}

static bool combine_captures(QueryResultCapture* cap_a, const QueryResultCapture* cap_b, SetOperation op) {
    if (cap_a->col_count != cap_b->col_count) {
        return report_database_error("queries in set operation must have equal number of columns");
    }
    int cols = cap_a->col_count;

    if (op == SET_OP_UNION_ALL) {
        int new_total = cap_a->row_count + cap_b->row_count;
        if (new_total > cap_a->capacity) {
            Value (*new_rows)[MAX_COLUMNS] = realloc(cap_a->rows, (size_t)(new_total > 0 ? new_total : 1) * sizeof(*new_rows));
            if (!new_rows) return report_database_error("cannot allocate memory for UNION ALL");
            cap_a->rows = new_rows;
            cap_a->capacity = new_total;
        }
        for (int r = 0; r < cap_b->row_count; r++) {
            for (int c = 0; c < cols; c++) cap_a->rows[cap_a->row_count][c] = cap_b->rows[r][c];
            cap_a->row_count++;
        }
        return true;
    }

    if (op == SET_OP_UNION) {
        int max_total = cap_a->row_count + cap_b->row_count;
        if (max_total > cap_a->capacity) {
            Value (*new_rows)[MAX_COLUMNS] = realloc(cap_a->rows, (size_t)(max_total > 0 ? max_total : 1) * sizeof(*new_rows));
            if (!new_rows) return report_database_error("cannot allocate memory for UNION");
            cap_a->rows = new_rows;
            cap_a->capacity = max_total;
        }
        for (int r = 0; r < cap_b->row_count; r++) {
            for (int c = 0; c < cols; c++) cap_a->rows[cap_a->row_count][c] = cap_b->rows[r][c];
            cap_a->row_count++;
        }
        int write_idx = 0;
        for (int i = 0; i < cap_a->row_count; i++) {
            bool dup = false;
            for (int j = 0; j < write_idx; j++) {
                if (row_equals(cap_a->rows[i], cap_a->rows[j], cols)) { dup = true; break; }
            }
            if (!dup) {
                if (write_idx != i) {
                    for (int c = 0; c < cols; c++) cap_a->rows[write_idx][c] = cap_a->rows[i][c];
                }
                write_idx++;
            }
        }
        cap_a->row_count = write_idx;
        return true;
    }

    if (op == SET_OP_INTERSECT) {
        int cap = cap_a->row_count < cap_b->row_count ? cap_a->row_count : cap_b->row_count;
        Value (*out_rows)[MAX_COLUMNS] = malloc((size_t)(cap > 0 ? cap : 1) * sizeof(*out_rows));
        if (!out_rows) return report_database_error("cannot allocate memory for INTERSECT");
        int count = 0;
        for (int i = 0; i < cap_a->row_count; i++) {
            bool in_b = false;
            for (int j = 0; j < cap_b->row_count; j++) {
                if (row_equals(cap_a->rows[i], cap_b->rows[j], cols)) { in_b = true; break; }
            }
            if (in_b) {
                bool dup = false;
                for (int k = 0; k < count; k++) {
                    if (row_equals(cap_a->rows[i], out_rows[k], cols)) { dup = true; break; }
                }
                if (!dup) {
                    for (int c = 0; c < cols; c++) out_rows[count][c] = cap_a->rows[i][c];
                    count++;
                }
            }
        }
        free(cap_a->rows);
        cap_a->rows = out_rows;
        cap_a->row_count = count;
        cap_a->capacity = cap > 0 ? cap : 1;
        return true;
    }

    if (op == SET_OP_MINUS) {
        int cap = cap_a->row_count;
        Value (*out_rows)[MAX_COLUMNS] = malloc((size_t)(cap > 0 ? cap : 1) * sizeof(*out_rows));
        if (!out_rows) return report_database_error("cannot allocate memory for MINUS");
        int count = 0;
        for (int i = 0; i < cap_a->row_count; i++) {
            bool in_b = false;
            for (int j = 0; j < cap_b->row_count; j++) {
                if (row_equals(cap_a->rows[i], cap_b->rows[j], cols)) { in_b = true; break; }
            }
            if (!in_b) {
                bool dup = false;
                for (int k = 0; k < count; k++) {
                    if (row_equals(cap_a->rows[i], out_rows[k], cols)) { dup = true; break; }
                }
                if (!dup) {
                    for (int c = 0; c < cols; c++) out_rows[count][c] = cap_a->rows[i][c];
                    count++;
                }
            }
        }
        free(cap_a->rows);
        cap_a->rows = out_rows;
        cap_a->row_count = count;
        cap_a->capacity = cap > 0 ? cap : 1;
        return true;
    }

    return true;
}

static bool parse_window_over_clause(Parser* p, ProjItem* item) {
    if (consume_keyword(p, "OVER")) {
        if (!consume_token(p, TK_LP)) return false;
        if (consume_keyword(p, "PARTITION")) {
            consume_keyword(p, "BY");
            if (!parse_column_reference(p, &item->partition_name)) return false;
            item->has_partition = true;
        }
        if (consume_keyword(p, "ORDER")) {
            consume_keyword(p, "BY");
            if (!parse_column_reference(p, &item->raw_name)) return false;
            if (consume_keyword(p, "DESC")) item->expr_op = 'D';
            else consume_keyword(p, "ASC");
        }
        if (!consume_token(p, TK_RP)) return false;
    }
    return true;
}

static bool execute_select_query_internal(Parser* p, QueryResultCapture* out_capture, int* out_limit, int* out_offset) {
    bool ok = false;
    ProjItem* proj_items = calloc(MAX_COLUMNS, sizeof(*proj_items));
    Filter* filter = calloc(1, sizeof(*filter));
    Predicate* joins = NULL;
    ResultTuple* results = NULL;
    RowData* rows = NULL;
    GroupAccumulator* groups = NULL;
    bool* right_matched = NULL;
    JoinIndex join_indexes[MAX_SCOPE_TABLES] = {{0}};
    bool use_hash[MAX_SCOPE_TABLES] = {false};
    Scope scope;
    memset(&scope, 0, sizeof(scope));

    if (!proj_items || !filter) {
        report_database_error("out of memory for select query");
        goto cleanup;
    }

    bool distinct = consume_keyword(p, "DISTINCT");
    int top_n = -1;
    if (consume_keyword(p, "TOP")) {
        bool has_parens = consume_token(p, TK_LP);
        if (p->kind != TK_NUMBER) goto cleanup;
        errno = 0;
        long tval = strtol(p->text, NULL, 10);
        if (errno || tval < 0) goto cleanup;
        top_n = (int)tval;
        scan_next_token(p);
        if (has_parens && !consume_token(p, TK_RP)) goto cleanup;
    }
    int proj_count = 0;
    bool star = consume_token(p, TK_STAR);
    bool has_agg = false;

    if (!star) {
        do {
            if (proj_count == MAX_COLUMNS) goto cleanup;
            ProjItem* item = &proj_items[proj_count++];

            if (consume_keyword(p, "COUNT")) {
                has_agg = true;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (consume_token(p, TK_STAR)) {
                    item->type = PROJ_COUNT_STAR;
                } else {
                    item->type = PROJ_COUNT_COL;
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "SUM")) {
                has_agg = true; item->type = PROJ_SUM;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "AVG")) {
                has_agg = true; item->type = PROJ_AVG;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "MIN")) {
                has_agg = true; item->type = PROJ_MIN;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "MAX")) {
                has_agg = true; item->type = PROJ_MAX;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "JSON_EXTRACT") || consume_keyword(p, "JSON_VALUE")) {
                item->type = PROJ_JSON_EXTRACT;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) || !consume_token(p, TK_COMMA)) goto cleanup;
                if (p->kind != TK_STRING) goto cleanup;
                snprintf(item->json_path, sizeof(item->json_path), "%s", p->text);
                scan_next_token(p);
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "ROW_NUMBER")) {
                item->type = PROJ_ROW_NUMBER;
                item->is_window = true;
                item->data_type = TYPE_NUMBER;
                if (!consume_token(p, TK_LP) || !consume_token(p, TK_RP)) goto cleanup;
                if (!parse_window_over_clause(p, item)) goto cleanup;
                snprintf(item->display_name, sizeof(item->display_name), "ROW_NUMBER()");
            } else if (consume_keyword(p, "RANK")) {
                item->type = PROJ_RANK;
                item->is_window = true;
                item->data_type = TYPE_NUMBER;
                if (!consume_token(p, TK_LP) || !consume_token(p, TK_RP)) goto cleanup;
                if (!parse_window_over_clause(p, item)) goto cleanup;
                snprintf(item->display_name, sizeof(item->display_name), "RANK()");
            } else if (consume_keyword(p, "DENSE_RANK")) {
                item->type = PROJ_DENSE_RANK;
                item->is_window = true;
                item->data_type = TYPE_NUMBER;
                if (!consume_token(p, TK_LP) || !consume_token(p, TK_RP)) goto cleanup;
                if (!parse_window_over_clause(p, item)) goto cleanup;
                snprintf(item->display_name, sizeof(item->display_name), "DENSE_RANK()");
            } else if (consume_keyword(p, "ROWNUM")) {
                item->type = PROJ_ROW_NUMBER;
                item->is_window = true;
                item->data_type = TYPE_NUMBER;
                snprintf(item->display_name, sizeof(item->display_name), "ROWNUM");
            } else if (consume_keyword(p, "GETDATE") || consume_keyword(p, "NOW")) {
                item->type = PROJ_IDENTITY;
                if (consume_token(p, TK_LP)) consume_token(p, TK_RP);
                time_t now = time(NULL); struct tm tm_val;
#ifdef _WIN32
                localtime_s(&tm_val, &now);
#else
                localtime_r(&now, &tm_val);
#endif
                strftime(item->literal1.text, sizeof(item->literal1.text), "%Y-%m-%d %H:%M:%S", &tm_val);
                item->literal1.is_null = false;
                item->has_literal1 = true;
                item->data_type = TYPE_DATE;
                snprintf(item->display_name, sizeof(item->display_name), "GETDATE()");
            } else if (consume_keyword(p, "CAST")) {
                item->type = PROJ_IDENTITY;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_keyword(p, "AS")) goto cleanup;
                if (p->kind == TK_ID) scan_next_token(p);
                if (consume_token(p, TK_LP)) {
                    while (p->kind != TK_RP && p->kind != TK_END) scan_next_token(p);
                    consume_token(p, TK_RP);
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
                snprintf(item->display_name, sizeof(item->display_name), "CAST");
            } else if (consume_keyword(p, "TO_CHAR") || consume_keyword(p, "TO_NUMBER") || consume_keyword(p, "TO_DATE")) {
                item->type = PROJ_IDENTITY;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (consume_token(p, TK_COMMA)) {
                    if (p->kind == TK_STRING) scan_next_token(p);
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
                snprintf(item->display_name, sizeof(item->display_name), "CONVERT");
            } else if (consume_keyword(p, "UPPER")) {
                item->type = PROJ_UPPER;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "LOWER")) {
                item->type = PROJ_LOWER;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "LENGTH") || consume_keyword(p, "LEN")) {
                item->type = PROJ_LENGTH;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "TRIM")) {
                item->type = PROJ_TRIM;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "SUBSTR") || consume_keyword(p, "SUBSTRING")) {
                item->type = PROJ_SUBSTR;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_COMMA)) goto cleanup;
                bool neg = false;
                if (consume_token(p, TK_MINUS)) neg = true;
                if (p->kind != TK_NUMBER) goto cleanup;
                item->substr_start = (int)strtol(p->text, NULL, 10);
                if (neg) item->substr_start = -item->substr_start;
                scan_next_token(p);
                item->substr_len = -1;
                if (consume_token(p, TK_COMMA)) {
                    if (p->kind != TK_NUMBER) goto cleanup;
                    item->substr_len = (int)strtol(p->text, NULL, 10);
                    scan_next_token(p);
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "CONCAT")) {
                item->type = PROJ_CONCAT;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_COMMA)) goto cleanup;
                if (p->kind == TK_STRING) {
                    snprintf(item->literal2.text, sizeof(item->literal2.text), "%s", p->text);
                    item->literal2.is_null = false;
                    item->has_literal2 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name2)) goto cleanup;
                    item->has_binding2 = true;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "ABS")) {
                item->type = PROJ_ABS;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "ROUND")) {
                item->type = PROJ_ROUND;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                item->round_decimals = 0;
                if (consume_token(p, TK_COMMA)) {
                    if (p->kind != TK_NUMBER) goto cleanup;
                    item->round_decimals = (int)strtol(p->text, NULL, 10);
                    scan_next_token(p);
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "CEIL") || consume_keyword(p, "CEILING")) {
                item->type = PROJ_CEIL;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "FLOOR")) {
                item->type = PROJ_FLOOR;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "MOD")) {
                item->type = PROJ_MOD;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_COMMA) || p->kind != TK_NUMBER) goto cleanup;
                item->mod_divisor = strtold(p->text, NULL);
                scan_next_token(p);
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "COALESCE") || consume_keyword(p, "NVL") || consume_keyword(p, "ISNULL")) {
                item->type = PROJ_COALESCE;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (consume_keyword(p, "NULL")) {
                    item->literal1.is_null = true;
                    item->has_literal1 = true;
                } else if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                    snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                    item->literal1.is_null = false;
                    item->has_literal1 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                }
                if (!consume_token(p, TK_COMMA)) goto cleanup;
                if (consume_keyword(p, "NULL")) {
                    item->literal2.is_null = true;
                    item->has_literal2 = true;
                } else if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                    snprintf(item->literal2.text, sizeof(item->literal2.text), "%s", p->text);
                    item->literal2.is_null = false;
                    item->has_literal2 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name2)) goto cleanup;
                    item->has_binding2 = true;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "NVL2")) {
                item->type = PROJ_NVL2;
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                if (!consume_token(p, TK_COMMA)) goto cleanup;
                if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                    snprintf(item->literal2.text, sizeof(item->literal2.text), "%s", p->text);
                    item->literal2.is_null = false;
                    item->has_literal2 = true;
                    scan_next_token(p);
                } else {
                    if (!parse_column_reference(p, &item->raw_name2)) goto cleanup;
                    item->has_binding2 = true;
                }
                if (!consume_token(p, TK_COMMA)) goto cleanup;
                if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                    snprintf(item->literal3.text, sizeof(item->literal3.text), "%s", p->text);
                    item->literal3.is_null = false;
                    item->has_literal3 = true;
                    scan_next_token(p);
                } else {
                    goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "CASE")) {
                item->type = PROJ_CASE;
                while (consume_keyword(p, "WHEN")) {
                    if (item->case_branch_count >= MAX_CASE_BRANCHES) goto cleanup;
                    CaseBranch* cb = &item->case_branches[item->case_branch_count++];
                    if (!parse_column_reference(p, &cb->raw_name)) goto cleanup;
                    if (consume_keyword(p, "IS")) {
                        if (consume_keyword(p, "NOT")) {
                            if (!consume_keyword(p, "NULL")) goto cleanup;
                            cb->is_null_check = 2;
                        } else {
                            if (!consume_keyword(p, "NULL")) goto cleanup;
                            cb->is_null_check = 1;
                        }
                    } else {
                        if (p->kind != TK_EQ && p->kind != TK_NE && p->kind != TK_LT &&
                            p->kind != TK_GT && p->kind != TK_LE && p->kind != TK_GE) {
                            goto cleanup;
                        }
                        cb->op = p->kind;
                        scan_next_token(p);
                        if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                            snprintf(cb->when_val.text, sizeof(cb->when_val.text), "%s", p->text);
                            cb->when_val.is_null = false;
                            scan_next_token(p);
                        } else {
                            goto cleanup;
                        }
                    }
                    if (!consume_keyword(p, "THEN")) goto cleanup;
                    if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                        snprintf(cb->then_val.text, sizeof(cb->then_val.text), "%s", p->text);
                        cb->then_val.is_null = false;
                        scan_next_token(p);
                    } else {
                        goto cleanup;
                    }
                }
                if (consume_keyword(p, "ELSE")) {
                    item->case_has_else = true;
                    if (consume_keyword(p, "NULL")) {
                        item->case_else_val.is_null = true;
                    } else if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                        snprintf(item->case_else_val.text, sizeof(item->case_else_val.text), "%s", p->text);
                        item->case_else_val.is_null = false;
                        scan_next_token(p);
                    } else {
                        goto cleanup;
                    }
                }
                if (!consume_keyword(p, "END")) goto cleanup;
            } else if (consume_keyword(p, "COSINE_DISTANCE")) {
                item->type = PROJ_COL;
                item->expr_op = 'C';
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) ||
                    !consume_token(p, TK_COMMA) || p->kind != TK_STRING) goto cleanup;
                snprintf(item->expr_operand.text, sizeof(item->expr_operand.text), "%s", p->text);
                item->expr_operand.is_null = false;
                scan_next_token(p);
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "L2_DISTANCE")) {
                item->type = PROJ_COL;
                item->expr_op = 'L';
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &item->raw_name) ||
                    !consume_token(p, TK_COMMA) || p->kind != TK_STRING) goto cleanup;
                snprintf(item->expr_operand.text, sizeof(item->expr_operand.text), "%s", p->text);
                item->expr_operand.is_null = false;
                scan_next_token(p);
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (p->kind == TK_NUMBER || p->kind == TK_STRING) {
                item->type = PROJ_COL;
                item->has_literal1 = true;
                item->literal1.is_null = false;
                snprintf(item->literal1.text, sizeof(item->literal1.text), "%s", p->text);
                snprintf(item->display_name, sizeof(item->display_name), "%s", p->text);
                scan_next_token(p);
                if (p->kind == TK_STAR || p->kind == TK_PLUS || p->kind == TK_MINUS || p->kind == TK_SLASH) {
                    item->expr_op = (p->kind == TK_PLUS ? '+' : p->kind == TK_MINUS ? '-' : p->kind == TK_STAR ? '*' : '/');
                    char op_ch = item->expr_op;
                    scan_next_token(p);
                    if (p->kind == TK_NUMBER) {
                        item->has_literal2 = true;
                        item->literal2.is_null = false;
                        normalize_decimal(p->text, item->literal2.text);
                        double n1 = strtod(item->literal1.text, NULL);
                        double n2 = strtod(p->text, NULL);
                        double res = 0.0;
                        if (op_ch == '+') res = n1 + n2;
                        else if (op_ch == '-') res = n1 - n2;
                        else if (op_ch == '*') res = n1 * n2;
                        else if (op_ch == '/') res = (n2 != 0.0) ? (n1 / n2) : 0.0;
                        if (res == (double)(int64_t)res) {
                            snprintf(item->literal1.text, sizeof(item->literal1.text), "%lld", (long long)res);
                        } else {
                            snprintf(item->literal1.text, sizeof(item->literal1.text), "%.6g", res);
                        }
                        char combined_name[MAX_NAME_LEN];
                        snprintf(combined_name, sizeof(combined_name), "%s %c %s", item->display_name, op_ch, p->text);
                        snprintf(item->display_name, sizeof(item->display_name), "%s", combined_name);
                        scan_next_token(p);
                    } else { goto cleanup; }
                }
            } else {
                item->type = PROJ_COL;
                if (!parse_column_reference(p, &item->raw_name)) goto cleanup;
                if (p->kind == TK_JSON_ARROW || p->kind == TK_JSON_TEXT_ARROW) {
                    item->type = PROJ_JSON_EXTRACT;
                    item->expr_op = (p->kind == TK_JSON_TEXT_ARROW) ? '>' : '-';
                    scan_next_token(p);
                    if (p->kind == TK_STRING || p->kind == TK_NUMBER) {
                        snprintf(item->json_path, sizeof(item->json_path), "%s", p->text);
                        scan_next_token(p);
                    } else goto cleanup;
                } else if (p->kind == TK_COSINE_DIST || p->kind == TK_L2_DIST) {
                    item->expr_op = (p->kind == TK_COSINE_DIST) ? 'C' : 'L';
                    scan_next_token(p);
                    if (p->kind != TK_STRING) goto cleanup;
                    snprintf(item->expr_operand.text, sizeof(item->expr_operand.text), "%s", p->text);
                    item->expr_operand.is_null = false;
                    scan_next_token(p);
                } else if (p->kind == TK_STAR || p->kind == TK_PLUS || p->kind == TK_MINUS || p->kind == TK_SLASH) {
                    item->expr_op = (p->kind == TK_PLUS ? '+' : p->kind == TK_MINUS ? '-' : p->kind == TK_STAR ? '*' : '/');
                    scan_next_token(p);
                    if (p->kind == TK_NUMBER) {
                        normalize_decimal(p->text, item->expr_operand.text);
                        scan_next_token(p);
                    } else { goto cleanup; }
                }
            }

            if (consume_keyword(p, "AS")) {
                if (!parse_identifier(p, item->alias)) goto cleanup;
            } else if (p->kind == TK_ID && !token_is_keyword(p, "FROM") && !token_is_keyword(p, "WHERE") &&
                       !token_is_keyword(p, "GROUP") && !token_is_keyword(p, "HAVING") &&
                       !token_is_keyword(p, "ORDER") && !token_is_keyword(p, "LIMIT") &&
                       !token_is_keyword(p, "FOR") &&
                       !token_is_keyword(p, "UNION") && !token_is_keyword(p, "INTERSECT") &&
                       !token_is_keyword(p, "MINUS") && !token_is_keyword(p, "EXCEPT")) {
                if (!parse_identifier(p, item->alias)) goto cleanup;
            }
        } while (consume_token(p, TK_COMMA));
    }

    char table_name[MAX_NAME_LEN] = {0};
    char table_alias[MAX_NAME_LEN] = {0};
    bool has_from = consume_keyword(p, "FROM");
    if (!has_from) {
        snprintf(table_name, sizeof(table_name), "DUAL");
    } else {
        if (!parse_identifier(p, table_name)) goto cleanup;
        if (consume_token(p, TK_DOT)) {
            char subname[MAX_NAME_LEN];
            if (!parse_identifier(p, subname)) goto cleanup;
            int tidx = find_table_index(g_db, subname);
            if (tidx >= 0) snprintf(table_name, sizeof(table_name), "%s", subname);
            else {
                char fq[MAX_NAME_LEN];
                snprintf(fq, sizeof(fq), "%s.%s", table_name, subname);
                tidx = find_table_index(g_db, fq);
                if (tidx >= 0) snprintf(table_name, sizeof(table_name), "%s", fq);
                else snprintf(table_name, sizeof(table_name), "%s", subname);
            }
        }
        if (consume_keyword(p, "AS")) {
            if (!parse_identifier(p, table_alias)) goto cleanup;
        } else if (p->kind == TK_ID && !token_is_keyword(p, "WHERE") && !token_is_keyword(p, "LEFT") &&
                   !token_is_keyword(p, "FULL") && !token_is_keyword(p, "CROSS") &&
                   !token_is_keyword(p, "INNER") && !token_is_keyword(p, "JOIN") && !token_is_keyword(p, "GROUP") &&
                   !token_is_keyword(p, "HAVING") && !token_is_keyword(p, "ORDER") &&
                   !token_is_keyword(p, "LIMIT") && !token_is_keyword(p, "OFFSET") &&
                   !token_is_keyword(p, "FOR") &&
                   !token_is_keyword(p, "UNION") && !token_is_keyword(p, "INTERSECT") &&
                   !token_is_keyword(p, "MINUS") && !token_is_keyword(p, "EXCEPT")) {
            if (!parse_identifier(p, table_alias)) goto cleanup;
        }
    }

    if (!strcmp(table_name, "DUAL")) {
        const char* header = proj_count == 1 && proj_items[0].alias[0] ? proj_items[0].alias : NULL;
        if (has_agg && proj_count == 1 && proj_items[0].type == PROJ_COUNT_STAR) {
            out_capture->col_count = 1;
            snprintf(out_capture->col_names[0], MAX_NAME_LEN, "%s", header ? header : "COUNT(*)");
            out_capture->rows = malloc(sizeof(*out_capture->rows));
            if (!out_capture->rows) { report_database_error("out of memory"); goto cleanup; }
            out_capture->capacity = 1;
            out_capture->row_count = 1;
            snprintf(out_capture->rows[0][0].text, MAX_VAL_LEN, "1");
            ok = true;
            goto cleanup;
        }
        if (star || (proj_count == 1 && !strcmp(proj_items[0].raw_name.column, "DUMMY") &&
                     (!proj_items[0].raw_name.table[0] || !strcmp(proj_items[0].raw_name.table, "DUAL")))) {
            out_capture->col_count = 1;
            snprintf(out_capture->col_names[0], MAX_NAME_LEN, "%s", header ? header : "DUMMY");
            out_capture->rows = malloc(sizeof(*out_capture->rows));
            if (!out_capture->rows) { report_database_error("out of memory"); goto cleanup; }
            out_capture->capacity = 1;
            out_capture->row_count = 1;
            snprintf(out_capture->rows[0][0].text, MAX_VAL_LEN, "X");
            ok = true;
            goto cleanup;
        }
        if (proj_count == 1 && !strcmp(proj_items[0].raw_name.column, "USER")) {
            out_capture->col_count = 1;
            snprintf(out_capture->col_names[0], MAX_NAME_LEN, "%s", header ? header : "USER");
            out_capture->rows = malloc(sizeof(*out_capture->rows));
            if (!out_capture->rows) { report_database_error("out of memory"); goto cleanup; }
            out_capture->capacity = 1;
            out_capture->row_count = 1;
            snprintf(out_capture->rows[0][0].text, MAX_VAL_LEN, "SYSDBA");
            ok = true;
            goto cleanup;
        }
        if (proj_count == 1 && !strcmp(proj_items[0].raw_name.column, "SYSDATE")) {
            time_t now = time(NULL); struct tm tm;
#ifdef _WIN32
            if (localtime_s(&tm, &now)) { report_database_error("cannot read time"); goto cleanup; }
#else
            if (!localtime_r(&now, &tm)) { report_database_error("cannot read time"); goto cleanup; }
#endif
            char date[32];
            if (!strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm)) { report_database_error("cannot format date"); goto cleanup; }
            out_capture->col_count = 1;
            snprintf(out_capture->col_names[0], MAX_NAME_LEN, "%s", header ? header : "SYSDATE");
            out_capture->rows = malloc(sizeof(*out_capture->rows));
            if (!out_capture->rows) { report_database_error("out of memory"); goto cleanup; }
            out_capture->capacity = 1;
            out_capture->row_count = 1;
            snprintf(out_capture->rows[0][0].text, MAX_VAL_LEN, "%s", date);
            ok = true;
            goto cleanup;
        }

        out_capture->col_count = proj_count;
        out_capture->rows = malloc((size_t)(proj_count > 0 ? proj_count : 1) * sizeof(*out_capture->rows));
        if (!out_capture->rows) { report_database_error("out of memory"); goto cleanup; }
        out_capture->capacity = 1;
        out_capture->row_count = 1;
        for (int c = 0; c < proj_count; c++) {
            snprintf(out_capture->col_names[c], MAX_NAME_LEN, "%s",
                     proj_items[c].alias[0] ? proj_items[c].alias :
                     (proj_items[c].display_name[0] ? proj_items[c].display_name : proj_items[c].raw_name.column));
            Value dummy_null = { .is_null = true, .text = "" };
            if (proj_items[c].has_literal1) {
                out_capture->rows[0][c] = proj_items[c].literal1;
            } else if (proj_items[c].type >= PROJ_UPPER && proj_items[c].type <= PROJ_IDENTITY) {
                evaluate_scalar_item(&proj_items[c], &dummy_null, &dummy_null, &out_capture->rows[0][c]);
            } else {
                out_capture->rows[0][c] = dummy_null;
            }
        }
        ok = true;
        goto cleanup;
    }

    int init_tbl = find_table_index(g_db, table_name);
    if (init_tbl < 0) { report_database_error("table does not exist: %s", table_name); goto cleanup; }
    if (!user_has_table_privilege(g_db, g_network_user, table_name, PRIV_SELECT)) {
        report_database_error("permission denied: SELECT on %s. Ask an administrator to GRANT SELECT ON %s TO '<user>';",
                              table_name, table_name);
        goto cleanup;
    }
    scope.tables[0] = &g_db->tables[init_tbl];
    if (table_alias[0]) snprintf(scope.aliases[0], sizeof(scope.aliases[0]), "%s", table_alias);
    scope.count = 1;

    joins = calloc(MAX_SCOPE_TABLES, sizeof(Predicate));
    if (!joins) { report_database_error("out of memory"); goto cleanup; }
    JoinType join_types[MAX_SCOPE_TABLES] = {JOIN_INNER};
    int left_join_cols[MAX_SCOPE_TABLES] = {0};

    while (scope.count < MAX_SCOPE_TABLES) {
        JoinType jt = JOIN_INNER;
        if (consume_keyword(p, "CROSS")) {
            if (!consume_keyword(p, "JOIN")) goto cleanup;
            jt = JOIN_CROSS;
        } else if (consume_keyword(p, "FULL")) {
            consume_keyword(p, "OUTER");
            if (!consume_keyword(p, "JOIN")) goto cleanup;
            jt = JOIN_FULL;
        } else if (consume_keyword(p, "LEFT")) {
            consume_keyword(p, "OUTER");
            if (!consume_keyword(p, "JOIN")) goto cleanup;
            jt = JOIN_LEFT;
        } else if (consume_keyword(p, "INNER")) {
            if (!consume_keyword(p, "JOIN")) goto cleanup;
            jt = JOIN_INNER;
        } else if (consume_keyword(p, "JOIN")) {
            jt = JOIN_INNER;
        } else {
            break;
        }

        Table* right = resolve_statement_table(p, g_db);
        if (!right) goto cleanup;
        if (!user_has_table_privilege(g_db, g_network_user, right->name, PRIV_SELECT)) {
            report_database_error("permission denied: SELECT on %s", right->name);
            goto cleanup;
        }
        char join_alias[MAX_NAME_LEN] = {0};
        if (consume_keyword(p, "AS")) {
            if (!parse_identifier(p, join_alias)) goto cleanup;
        } else if (p->kind == TK_ID && !token_is_keyword(p, "ON") && jt != JOIN_CROSS) {
            if (!parse_identifier(p, join_alias)) goto cleanup;
        }

        int t_idx = scope.count;
        scope.tables[t_idx] = right;
        if (join_alias[0]) snprintf(scope.aliases[t_idx], sizeof(scope.aliases[t_idx]), "%s", join_alias);
        scope.count++;
        join_types[t_idx] = jt;

        if (jt != JOIN_CROSS) {
            if (!consume_keyword(p, "ON")) goto cleanup;
            if (!parse_comparison_predicate(p, &scope, &joins[t_idx])) goto cleanup;
            if (joins[t_idx].op == TK_EQ && joins[t_idx].right_column) {
                int right_col = -1;
                if (joins[t_idx].right.table == t_idx) {
                    left_join_cols[t_idx] = joins[t_idx].left.column;
                    right_col = joins[t_idx].right.column;
                } else if (joins[t_idx].left.table == t_idx) {
                    left_join_cols[t_idx] = joins[t_idx].right.column;
                    right_col = joins[t_idx].left.column;
                }
                if (right_col >= 0 && join_index_build(&join_indexes[t_idx], right, right_col)) {
                    use_hash[t_idx] = true;
                }
            }
        }
    }

    if (star) {
        proj_count = 0;
        for (int i = 0; i < scope.count; i++) {
            for (int c = 0; c < scope.tables[i]->col_count; c++) {
                if (proj_count == MAX_COLUMNS) goto cleanup;
                ProjItem* item = &proj_items[proj_count++];
                memset(item, 0, sizeof(*item));
                item->type = PROJ_COL;
                item->binding = (Binding){i, c};
                item->data_type = scope.tables[i]->columns[c].type;
                if (scope.count > 1)
                    snprintf(item->display_name, sizeof(item->display_name), "%s.%s", scope.tables[i]->name, scope.tables[i]->columns[c].name);
                else
                    snprintf(item->display_name, sizeof(item->display_name), "%s", scope.tables[i]->columns[c].name);
            }
        }
    } else {
        for (int i = 0; i < proj_count; i++) {
            ProjItem* item = &proj_items[i];
            if (item->type == PROJ_ROW_NUMBER || item->type == PROJ_RANK || item->type == PROJ_DENSE_RANK) {
                item->data_type = TYPE_NUMBER;
                if (item->has_partition && item->partition_name.column[0]) {
                    if (!resolve_column_reference(&scope, &item->partition_name, &item->partition_binding)) goto cleanup;
                    if (item->partition_binding.table & 0x1000) {
                        report_database_error("outer-scope column is not allowed in PARTITION BY");
                        goto cleanup;
                    }
                    bool found_p = false;
                    for (int c = 0; c < proj_count; c++) {
                        if (proj_items[c].type == PROJ_COL &&
                            proj_items[c].binding.table == item->partition_binding.table &&
                            proj_items[c].binding.column == item->partition_binding.column) {
                            found_p = true; break;
                        }
                    }
                    if (!found_p && proj_count < MAX_COLUMNS) {
                        int h = proj_count++;
                        memset(&proj_items[h], 0, sizeof(ProjItem));
                        proj_items[h].type = PROJ_COL;
                        proj_items[h].binding = item->partition_binding;
                        proj_items[h].raw_name = item->partition_name;
                        proj_items[h].data_type = scope.tables[item->partition_binding.table]->columns[item->partition_binding.column].type;
                        proj_items[h].is_hidden = true;
                    }
                }
                if (item->raw_name.column[0]) {
                    if (!resolve_column_reference(&scope, &item->raw_name, &item->binding)) goto cleanup;
                    if (item->binding.table & 0x1000) {
                        report_database_error("outer-scope column is not allowed in a window function");
                        goto cleanup;
                    }
                    bool found_o = false;
                    for (int c = 0; c < proj_count; c++) {
                        if (proj_items[c].type == PROJ_COL &&
                            proj_items[c].binding.table == item->binding.table &&
                            proj_items[c].binding.column == item->binding.column) {
                            found_o = true; break;
                        }
                    }
                    if (!found_o && proj_count < MAX_COLUMNS) {
                        int h = proj_count++;
                        memset(&proj_items[h], 0, sizeof(ProjItem));
                        proj_items[h].type = PROJ_COL;
                        proj_items[h].binding = item->binding;
                        proj_items[h].raw_name = item->raw_name;
                        proj_items[h].data_type = scope.tables[item->binding.table]->columns[item->binding.column].type;
                        proj_items[h].is_hidden = true;
                    }
                }
            } else if (item->type == PROJ_CASE) {
                item->data_type = TYPE_VARCHAR2;
                for (int b = 0; b < item->case_branch_count; b++) {
                    resolve_column_reference(&scope, &item->case_branches[b].raw_name, &item->case_branches[b].binding);
                }
            } else if (item->type >= PROJ_UPPER && item->type <= PROJ_IDENTITY) {
                if (!item->has_literal1 && item->raw_name.column[0]) {
                    if (!resolve_column_reference(&scope, &item->raw_name, &item->binding)) goto cleanup;
                    if (item->binding.table & 0x1000) {
                        report_database_error("outer-scope column is not allowed here: %s", item->raw_name.column);
                        goto cleanup;
                    }
                }
                if (item->has_binding2 && item->raw_name2.column[0]) {
                    if (!resolve_column_reference(&scope, &item->raw_name2, &item->binding2)) goto cleanup;
                    if (item->binding2.table & 0x1000) {
                        report_database_error("outer-scope column is not allowed here: %s", item->raw_name2.column);
                        goto cleanup;
                    }
                }
                if (item->type == PROJ_LENGTH || item->type == PROJ_ABS || item->type == PROJ_ROUND ||
                    item->type == PROJ_CEIL || item->type == PROJ_FLOOR || item->type == PROJ_MOD) {
                    item->data_type = TYPE_NUMBER;
                } else {
                    item->data_type = TYPE_VARCHAR2;
                }
            } else if (item->type != PROJ_COUNT_STAR) {
                if (!resolve_column_reference(&scope, &item->raw_name, &item->binding)) goto cleanup;
                if (item->binding.table & 0x1000) {
                    report_database_error("outer-scope column is not allowed here: %s", item->raw_name.column);
                    goto cleanup;
                }
                item->data_type = (item->expr_op == 'C' || item->expr_op == 'L') ?
                    TYPE_NUMBER : scope.tables[item->binding.table]->columns[item->binding.column].type;
                if ((item->type == PROJ_SUM || item->type == PROJ_AVG) && item->data_type != TYPE_NUMBER) {
                    report_database_error("SUM and AVG require numeric column");
                    goto cleanup;
                }
            } else {
                item->data_type = TYPE_NUMBER;
            }

            if (item->alias[0]) {
                snprintf(item->display_name, sizeof(item->display_name), "%s", item->alias);
            } else {
                const char* cname = item->type == PROJ_COUNT_STAR ? "*" :
                    (item->raw_name.column[0] ? item->raw_name.column : (item->has_literal1 ? item->literal1.text : "?"));
                switch (item->type) {
                case PROJ_COUNT_STAR: snprintf(item->display_name, sizeof(item->display_name), "COUNT(*)"); break;
                case PROJ_COUNT_COL: snprintf(item->display_name, sizeof(item->display_name), "COUNT(%s)", cname); break;
                case PROJ_SUM: snprintf(item->display_name, sizeof(item->display_name), "SUM(%s)", cname); break;
                case PROJ_AVG: snprintf(item->display_name, sizeof(item->display_name), "AVG(%s)", cname); break;
                case PROJ_MIN: snprintf(item->display_name, sizeof(item->display_name), "MIN(%s)", cname); break;
                case PROJ_MAX: snprintf(item->display_name, sizeof(item->display_name), "MAX(%s)", cname); break;
                case PROJ_JSON_EXTRACT:
                    if (item->expr_op == '>') {
                        snprintf(item->display_name, sizeof(item->display_name), "%s->>'%s'", cname, item->json_path);
                    } else if (item->expr_op == '-') {
                        snprintf(item->display_name, sizeof(item->display_name), "%s->'%s'", cname, item->json_path);
                    } else {
                        snprintf(item->display_name, sizeof(item->display_name), "JSON_EXTRACT(%s, '%s')", cname, item->json_path);
                    }
                    break;
                case PROJ_ROW_NUMBER: snprintf(item->display_name, sizeof(item->display_name), "ROW_NUMBER()"); break;
                case PROJ_RANK: snprintf(item->display_name, sizeof(item->display_name), "RANK()"); break;
                case PROJ_DENSE_RANK: snprintf(item->display_name, sizeof(item->display_name), "DENSE_RANK()"); break;
                case PROJ_UPPER: snprintf(item->display_name, sizeof(item->display_name), "UPPER(%s)", cname); break;
                case PROJ_LOWER: snprintf(item->display_name, sizeof(item->display_name), "LOWER(%s)", cname); break;
                case PROJ_LENGTH: snprintf(item->display_name, sizeof(item->display_name), "LENGTH(%s)", cname); break;
                case PROJ_TRIM: snprintf(item->display_name, sizeof(item->display_name), "TRIM(%s)", cname); break;
                case PROJ_SUBSTR: snprintf(item->display_name, sizeof(item->display_name), "SUBSTR(%s)", cname); break;
                case PROJ_CONCAT: snprintf(item->display_name, sizeof(item->display_name), "CONCAT(%s)", cname); break;
                case PROJ_ABS: snprintf(item->display_name, sizeof(item->display_name), "ABS(%s)", cname); break;
                case PROJ_ROUND: snprintf(item->display_name, sizeof(item->display_name), "ROUND(%s)", cname); break;
                case PROJ_CEIL: snprintf(item->display_name, sizeof(item->display_name), "CEIL(%s)", cname); break;
                case PROJ_FLOOR: snprintf(item->display_name, sizeof(item->display_name), "FLOOR(%s)", cname); break;
                case PROJ_MOD: snprintf(item->display_name, sizeof(item->display_name), "MOD(%s)", cname); break;
                case PROJ_COALESCE:
                case PROJ_NVL: snprintf(item->display_name, sizeof(item->display_name), "COALESCE(%s)", cname); break;
                case PROJ_NVL2: snprintf(item->display_name, sizeof(item->display_name), "NVL2(%s)", cname); break;
                case PROJ_IDENTITY:
                    if (!item->display_name[0]) snprintf(item->display_name, sizeof(item->display_name), "%s", cname);
                    break;
                case PROJ_CASE: snprintf(item->display_name, sizeof(item->display_name), "CASE"); break;
                default:
                    if (item->expr_op == 'C') {
                        snprintf(item->display_name, sizeof(item->display_name), "COSINE_DISTANCE(%s)", cname);
                    } else if (item->expr_op == 'L') {
                        snprintf(item->display_name, sizeof(item->display_name), "L2_DISTANCE(%s)", cname);
                    } else if (item->expr_op != 0) {
                        snprintf(item->display_name, sizeof(item->display_name), "%s%c%s", cname, item->expr_op, item->expr_operand.text);
                    } else if (scope.count > 1 && !(item->binding.table & 0x1000) && item->binding.table < scope.count)
                        snprintf(item->display_name, sizeof(item->display_name), "%s.%s", scope.tables[item->binding.table]->name, cname);
                    else
                        snprintf(item->display_name, sizeof(item->display_name), "%s", cname);
                    break;
                }
            }
        }
    }

    if (token_is_keyword(p, "WHERE")) {
        Parser lookahead = *p;
        scan_next_token(&lookahead);
        if (token_is_keyword(&lookahead, "ROWNUM")) {
            scan_next_token(&lookahead);
            if (lookahead.kind == TK_LE || lookahead.kind == TK_LT || lookahead.kind == TK_EQ) {
                TokenKind r_op = lookahead.kind;
                scan_next_token(&lookahead);
                if (lookahead.kind == TK_NUMBER) {
                    errno = 0;
                    long r_val = strtol(lookahead.text, NULL, 10);
                    if (!errno && r_val >= 0) {
                        int r_limit = (r_op == TK_LT) ? (int)(r_val - 1) : (int)r_val;
                        if (r_limit < 0) r_limit = 0;
                        if (top_n < 0 || r_limit < top_n) top_n = r_limit;
                    }
                }
            }
        }
    }

    if (!parse_where_clause(p, &scope, filter)) goto cleanup;

    Binding group_bindings[MAX_COLUMNS];
    int group_count = 0;
    if (consume_keyword(p, "GROUP")) {
        if (!consume_keyword(p, "BY")) goto cleanup;
        do {
            if (group_count == MAX_COLUMNS) goto cleanup;
            ColumnName cn;
            if (!parse_column_reference(p, &cn) || !resolve_column_reference(&scope, &cn, &group_bindings[group_count++])) goto cleanup;
        } while (consume_token(p, TK_COMMA));
    }

    HavingClause having_clause = {0};
    if (consume_keyword(p, "HAVING")) {
        do {
            if (having_clause.count >= MAX_HAVING_CONDS) goto cleanup;
            HavingCond* hc = &having_clause.conds[having_clause.count++];
            if (consume_keyword(p, "COUNT")) {
                if (!consume_token(p, TK_LP)) goto cleanup;
                if (consume_token(p, TK_STAR)) {
                    hc->agg_type = PROJ_COUNT_STAR;
                } else {
                    hc->agg_type = PROJ_COUNT_COL;
                    if (!parse_column_reference(p, &hc->col_ref)) goto cleanup;
                }
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "SUM")) {
                hc->agg_type = PROJ_SUM;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &hc->col_ref) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "AVG")) {
                hc->agg_type = PROJ_AVG;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &hc->col_ref) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "MIN")) {
                hc->agg_type = PROJ_MIN;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &hc->col_ref) || !consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "MAX")) {
                hc->agg_type = PROJ_MAX;
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &hc->col_ref) || !consume_token(p, TK_RP)) goto cleanup;
            } else {
                report_database_error("expected aggregate function in HAVING clause");
                goto cleanup;
            }

            if (p->kind != TK_EQ && p->kind != TK_NE && p->kind != TK_LT &&
                p->kind != TK_GT && p->kind != TK_LE && p->kind != TK_GE) {
                report_database_error("expected comparison operator in HAVING clause");
                goto cleanup;
            }
            hc->op = p->kind;
            scan_next_token(p);

            bool neg = false;
            if (consume_token(p, TK_MINUS)) neg = true;
            if (p->kind != TK_NUMBER) { report_database_error("expected number in HAVING clause"); goto cleanup; }
            hc->target_val = strtold(p->text, NULL);
            if (neg) hc->target_val = -hc->target_val;
            scan_next_token(p);

            int target_idx = -1;
            if (hc->agg_type == PROJ_COUNT_STAR) {
                target_idx = 0;
            } else {
                for (int i = 0; i < proj_count; i++) {
                    if (proj_items[i].type == hc->agg_type &&
                        !strcmp(proj_items[i].raw_name.column, hc->col_ref.column)) {
                        target_idx = i; break;
                    }
                }
                if (target_idx < 0) {
                    if (proj_count == MAX_COLUMNS) goto cleanup;
                    target_idx = proj_count++;
                    memset(&proj_items[target_idx], 0, sizeof(ProjItem));
                    proj_items[target_idx].type = hc->agg_type;
                    proj_items[target_idx].raw_name = hc->col_ref;
                    proj_items[target_idx].is_hidden = true;
                    if (!resolve_column_reference(&scope, &proj_items[target_idx].raw_name, &proj_items[target_idx].binding)) goto cleanup;
                    if (proj_items[target_idx].binding.table & 0x1000) {
                        report_database_error("outer-scope column is not allowed in HAVING");
                        goto cleanup;
                    }
                    proj_items[target_idx].data_type = scope.tables[proj_items[target_idx].binding.table]->columns[proj_items[target_idx].binding.column].type;
                }
            }
            hc->proj_idx = target_idx;
        } while (consume_keyword(p, "AND"));
    }

    if (group_count > 0) {
        for (int i = 0; i < proj_count; i++) {
            if (proj_items[i].type == PROJ_COL) {
                bool found = false;
                for (int g = 0; g < group_count; g++) {
                    if (proj_items[i].binding.table == group_bindings[g].table &&
                        proj_items[i].binding.column == group_bindings[g].column) {
                        found = true; break;
                    }
                }
                if (!found) { report_database_error("column must appear in GROUP BY: %s", proj_items[i].display_name); goto cleanup; }
            }
        }
    } else if (has_agg) {
        for (int i = 0; i < proj_count; i++) {
            if (proj_items[i].type == PROJ_COL) {
                report_database_error("column must appear in GROUP BY or be aggregated: %s", proj_items[i].display_name);
                goto cleanup;
            }
        }
    }

    SortKey sort_keys[MAX_COLUMNS];
    int sort_count = 0;
    if (consume_keyword(p, "ORDER")) {
        if (!consume_keyword(p, "BY")) goto cleanup;
        do {
            if (sort_count == MAX_COLUMNS) goto cleanup;
            ColumnName cn;
            char vec_op = 0;
            char vec_lit[MAX_VAL_LEN] = {0};
            if (consume_keyword(p, "COSINE_DISTANCE")) {
                vec_op = 'C';
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &cn) ||
                    !consume_token(p, TK_COMMA) || p->kind != TK_STRING) goto cleanup;
                snprintf(vec_lit, sizeof(vec_lit), "%s", p->text);
                scan_next_token(p);
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else if (consume_keyword(p, "L2_DISTANCE")) {
                vec_op = 'L';
                if (!consume_token(p, TK_LP) || !parse_column_reference(p, &cn) ||
                    !consume_token(p, TK_COMMA) || p->kind != TK_STRING) goto cleanup;
                snprintf(vec_lit, sizeof(vec_lit), "%s", p->text);
                scan_next_token(p);
                if (!consume_token(p, TK_RP)) goto cleanup;
            } else {
                if (!parse_column_reference(p, &cn)) goto cleanup;
                if (p->kind == TK_COSINE_DIST || p->kind == TK_L2_DIST) {
                    vec_op = (p->kind == TK_COSINE_DIST) ? 'C' : 'L';
                    scan_next_token(p);
                    if (p->kind != TK_STRING) goto cleanup;
                    snprintf(vec_lit, sizeof(vec_lit), "%s", p->text);
                    scan_next_token(p);
                }
            }
            int target_proj = -1;
            if (vec_op != 0) {
                Binding b;
                if (!resolve_column_reference(&scope, &cn, &b)) goto cleanup;
                if (proj_count == MAX_COLUMNS) goto cleanup;
                target_proj = proj_count++;
                memset(&proj_items[target_proj], 0, sizeof(ProjItem));
                proj_items[target_proj].type = PROJ_COL;
                proj_items[target_proj].binding = b;
                proj_items[target_proj].expr_op = vec_op;
                snprintf(proj_items[target_proj].expr_operand.text, sizeof(proj_items[target_proj].expr_operand.text), "%s", vec_lit);
                proj_items[target_proj].expr_operand.is_null = false;
                proj_items[target_proj].data_type = TYPE_NUMBER;
                proj_items[target_proj].is_hidden = true;
            } else {
                for (int i = 0; i < proj_count; i++) {
                    if (proj_items[i].alias[0] && !strcmp(proj_items[i].alias, cn.column)) {
                        target_proj = i; break;
                    }
                }
                if (target_proj < 0) {
                    Binding b;
                    if (resolve_column_reference(&scope, &cn, &b)) {
                        for (int i = 0; i < proj_count; i++) {
                            if (proj_items[i].type == PROJ_COL &&
                                proj_items[i].expr_op == 0 &&
                                proj_items[i].binding.table == b.table &&
                                proj_items[i].binding.column == b.column) {
                                target_proj = i; break;
                            }
                        }
                        if (target_proj < 0) {
                            if (b.table & 0x1000) {
                                report_database_error("outer-scope column is not allowed in ORDER BY");
                                goto cleanup;
                            }
                            if (proj_count == MAX_COLUMNS) goto cleanup;
                            target_proj = proj_count++;
                            memset(&proj_items[target_proj], 0, sizeof(ProjItem));
                            proj_items[target_proj].type = PROJ_COL;
                            proj_items[target_proj].binding = b;
                            proj_items[target_proj].data_type = scope.tables[b.table]->columns[b.column].type;
                            proj_items[target_proj].is_hidden = true;
                        }
                    }
                }
            }
            if (target_proj < 0) { report_database_error("ORDER BY column not found: %s", cn.column); goto cleanup; }
            bool desc = false;
            if (consume_keyword(p, "DESC")) desc = true;
            else consume_keyword(p, "ASC");
            sort_keys[sort_count].proj_idx = target_proj;
            sort_keys[sort_count].desc = desc;
            sort_keys[sort_count].type = proj_items[target_proj].data_type;
            sort_count++;
        } while (consume_token(p, TK_COMMA));
    }

    int limit = (top_n >= 0 && top_n < MAX_RESULTS) ? top_n : ((g_max_rows > 0 && g_max_rows < MAX_RESULTS) ? g_max_rows : MAX_RESULTS);
    int offset = 0;
    for (int iter = 0; iter < 2; iter++) {
        if (consume_keyword(p, "LIMIT")) {
            if (p->kind != TK_NUMBER) goto cleanup;
            errno = 0;
            long val = strtol(p->text, NULL, 10);
            if (errno || val < 0) goto cleanup;
            limit = (int)val;
            scan_next_token(p);
            if (consume_keyword(p, "OFFSET")) {
                if (p->kind != TK_NUMBER) goto cleanup;
                errno = 0;
                long oval = strtol(p->text, NULL, 10);
                if (errno || oval < 0) goto cleanup;
                offset = (int)oval;
                scan_next_token(p);
            }
        } else if (consume_keyword(p, "OFFSET")) {
            if (p->kind != TK_NUMBER) goto cleanup;
            errno = 0;
            long oval = strtol(p->text, NULL, 10);
            if (errno || oval < 0) goto cleanup;
            offset = (int)oval;
            scan_next_token(p);
            if (consume_keyword(p, "LIMIT")) {
                if (p->kind != TK_NUMBER) goto cleanup;
                errno = 0;
                long val = strtol(p->text, NULL, 10);
                if (errno || val < 0) goto cleanup;
                limit = (int)val;
                scan_next_token(p);
            }
        }
    }

    bool for_update = false;
    bool skip_locked = false;
    if (consume_keyword(p, "FOR")) {
        if (consume_keyword(p, "UPDATE")) {
            for_update = true;
            if (consume_keyword(p, "SKIP")) {
                if (consume_keyword(p, "LOCKED")) {
                    skip_locked = true;
                }
            } else if (consume_keyword(p, "NOWAIT")) {
                /* standard non-blocking modifier */
            }
        }
    }

    *out_limit = limit;
    *out_offset = offset;
    g_select_max_needed = (sort_count == 0 && (limit + offset) < MAX_RESULTS) ? (limit + offset) : MAX_RESULTS;

    results = malloc(MAX_RESULTS * sizeof(*results));
    if (!results) {
        report_database_error("cannot allocate result set");
        goto cleanup;
    }

    int selected = 0, pairs = 0;
    bool work_limit = false;
    int current_tuple[MAX_SCOPE_TABLES] = {0};
    if (scope.count > 1 && join_types[1] == JOIN_FULL) {
        right_matched = calloc((size_t)scope.tables[1]->row_count, sizeof(bool));
    }

    emit_tuples(&scope, joins, join_types, use_hash, join_indexes, left_join_cols, filter,
                0, current_tuple, results, &selected, &pairs, &work_limit, right_matched);

    if (work_limit) {
        report_database_error("query work limit reached");
        goto cleanup;
    }

    rows = malloc(MAX_RESULTS * sizeof(*rows));
    if (!rows) {
        report_database_error("cannot allocate row output buffer");
        goto cleanup;
    }
    int total_output_rows = 0;
    static const Value null_val = { .is_null = true, .text = "" };

    if (group_count > 0 || has_agg || having_clause.count > 0) {
        groups = calloc(MAX_RESULTS, sizeof(*groups));
        if (!groups) {
            report_database_error("cannot allocate group accumulators");
            goto cleanup;
        }
        int group_total = 0;

        bool use_vectorized = (group_count == 0 && has_agg && scope.count == 1);
        if (use_vectorized) {
            for (int i = 0; i < proj_count; i++) {
                if (proj_items[i].type != PROJ_COUNT_STAR &&
                    scope.tables[0]->columns[proj_items[i].binding.column].type != TYPE_NUMBER) {
                    use_vectorized = false;
                    break;
                }
            }
        }

        if (use_vectorized) {
            GroupAccumulator* grp = &groups[0];
            grp->count_star = selected;
            for (int i = 0; i < proj_count; i++) {
                if (proj_items[i].type == PROJ_COUNT_STAR) continue;
                int c_idx = proj_items[i].binding.column;
                long double v_sum = 0, v_min = 0, v_max = 0;
                int v_count = 0;
                vector_aggregate_numeric(scope.tables[0], c_idx, filter, &v_sum, &v_min, &v_max, &v_count);
                grp->col_counts[i] = v_count;
                grp->sums[i] = v_sum;
                if (v_count > 0) {
                    grp->has_min[i] = true;
                    format_decimal_val(v_min, grp->mins[i].text, sizeof(grp->mins[i].text));
                    grp->mins[i].is_null = false;
                    grp->has_max[i] = true;
                    format_decimal_val(v_max, grp->maxs[i].text, sizeof(grp->maxs[i].text));
                    grp->maxs[i].is_null = false;
                }
            }
            group_total = 1;
        } else {
            for (int r = 0; r < selected; r++) {
                for (int t = 0; t < scope.count; t++) {
                    scope.rows[t] = results[r].indices[t] >= 0 ? &scope.tables[t]->rows[results[r].indices[t]] : NULL;
                }

                int g_idx = -1;
                if (group_count == 0) {
                    g_idx = 0;
                    if (group_total == 0) group_total = 1;
                } else {
                    for (int g = 0; g < group_total; g++) {
                        bool match = true;
                        for (int k = 0; k < group_count; k++) {
                            int t_idx = group_bindings[k].table;
                            const Value* v = (results[r].indices[t_idx] < 0) ? &null_val :
                                &scope.rows[t_idx]->values[group_bindings[k].column];
                            DataType type = scope.tables[t_idx]->columns[group_bindings[k].column].type;
                            if (v->is_null != groups[g].group_keys[k].is_null ||
                                (!v->is_null && compare_typed_values(type, v, &groups[g].group_keys[k]) != 0)) {
                                match = false; break;
                            }
                        }
                        if (match) { g_idx = g; break; }
                    }
                    if (g_idx < 0) {
                        if (group_total == MAX_RESULTS) {
                            report_database_error("group limit reached");
                            goto cleanup;
                        }
                        g_idx = group_total++;
                        for (int k = 0; k < group_count; k++) {
                            int t_idx = group_bindings[k].table;
                            const Value* v = (results[r].indices[t_idx] < 0) ? &null_val :
                                &scope.rows[t_idx]->values[group_bindings[k].column];
                            groups[g_idx].group_keys[k] = *v;
                        }
                    }
                }

                GroupAccumulator* grp = &groups[g_idx];
                grp->count_star++;

                for (int i = 0; i < proj_count; i++) {
                    if (proj_items[i].type == PROJ_COUNT_STAR) continue;
                    int t_idx = proj_items[i].binding.table;
                    if (t_idx & 0x1000) continue; /* outer-scope refs rejected at parse time */
                    const Value* v = (t_idx < scope.count && results[r].indices[t_idx] >= 0) ?
                        &scope.rows[t_idx]->values[proj_items[i].binding.column] : &null_val;
                    if (!v->is_null) {
                        grp->col_counts[i]++;
                        if (proj_items[i].type == PROJ_SUM || proj_items[i].type == PROJ_AVG) {
                            grp->sums[i] += strtold(v->text, NULL);
                        }
                        if (proj_items[i].type == PROJ_MIN) {
                            if (!grp->has_min[i] || compare_typed_values(proj_items[i].data_type, v, &grp->mins[i]) < 0) {
                                grp->mins[i] = *v;
                                grp->has_min[i] = true;
                            }
                        }
                        if (proj_items[i].type == PROJ_MAX) {
                            if (!grp->has_max[i] || compare_typed_values(proj_items[i].data_type, v, &grp->maxs[i]) > 0) {
                                grp->maxs[i] = *v;
                                grp->has_max[i] = true;
                            }
                        }
                    }
                }
            }
        }

        if (group_count == 0 && group_total == 0) group_total = 1;

        for (int g = 0; g < group_total; g++) {
            GroupAccumulator* grp = &groups[g];

            bool pass_having = true;
            for (int h = 0; h < having_clause.count; h++) {
                const HavingCond* hc = &having_clause.conds[h];
                long double val = 0;
                if (hc->agg_type == PROJ_COUNT_STAR) {
                    val = (long double)grp->count_star;
                } else if (hc->agg_type == PROJ_COUNT_COL) {
                    val = (long double)grp->col_counts[hc->proj_idx];
                } else if (hc->agg_type == PROJ_SUM) {
                    val = grp->sums[hc->proj_idx];
                } else if (hc->agg_type == PROJ_AVG) {
                    val = grp->col_counts[hc->proj_idx] > 0 ? (grp->sums[hc->proj_idx] / (long double)grp->col_counts[hc->proj_idx]) : 0;
                } else if (hc->agg_type == PROJ_MIN) {
                    val = strtold(grp->mins[hc->proj_idx].text, NULL);
                } else if (hc->agg_type == PROJ_MAX) {
                    val = strtold(grp->maxs[hc->proj_idx].text, NULL);
                }

                bool match = false;
                switch (hc->op) {
                case TK_EQ: match = (val == hc->target_val); break;
                case TK_NE: match = (val != hc->target_val); break;
                case TK_LT: match = (val < hc->target_val); break;
                case TK_LE: match = (val <= hc->target_val); break;
                case TK_GT: match = (val > hc->target_val); break;
                case TK_GE: match = (val >= hc->target_val); break;
                default: break;
                }
                if (!match) { pass_having = false; break; }
            }
            if (!pass_having) continue;

            RowData* row = &rows[total_output_rows++];
            for (int i = 0; i < proj_count; i++) {
                Value* cell = &row->cells[i];
                memset(cell, 0, sizeof(*cell));
                switch (proj_items[i].type) {
                case PROJ_COL: {
                    int k = -1;
                    for (int gk = 0; gk < group_count; gk++) {
                        if (group_bindings[gk].table == proj_items[i].binding.table &&
                            group_bindings[gk].column == proj_items[i].binding.column) {
                            k = gk; break;
                        }
                    }
                    if (k >= 0) *cell = grp->group_keys[k];
                    break;
                }
                case PROJ_COUNT_STAR:
                    snprintf(cell->text, sizeof(cell->text), "%d", grp->count_star);
                    break;
                case PROJ_COUNT_COL:
                    snprintf(cell->text, sizeof(cell->text), "%d", grp->col_counts[i]);
                    break;
                case PROJ_SUM:
                    if (grp->col_counts[i] == 0) cell->is_null = true;
                    else format_decimal_val(grp->sums[i], cell->text, sizeof(cell->text));
                    break;
                case PROJ_AVG:
                    if (grp->col_counts[i] == 0) cell->is_null = true;
                    else format_decimal_val(grp->sums[i] / (long double)grp->col_counts[i], cell->text, sizeof(cell->text));
                    break;
                case PROJ_MIN:
                    if (!grp->has_min[i]) cell->is_null = true;
                    else *cell = grp->mins[i];
                    break;
                case PROJ_MAX:
                    if (!grp->has_max[i]) cell->is_null = true;
                    else *cell = grp->maxs[i];
                    break;
                default:
                    cell->is_null = true;
                    break;
                }
            }
        }
    } else {
        for (int r = 0; r < selected; r++) {
            if (for_update && scope.count > 0 && scope.tables[0]) {
                uint32_t rid = (uint32_t)(results[r].indices[0] + 1);
                const char* tname = scope.tables[0]->name;
                int sess = g_current_session_id > 0 ? g_current_session_id : 1;
                if (skip_locked) {
                    if (lock_manager_has_row_conflict(sess, tname, rid, LOCK_MODE_EXCLUSIVE)) {
                        continue;
                    }
                }
                if (!lock_manager_acquire_row(sess, tname, rid, LOCK_MODE_EXCLUSIVE, 2000)) {
                    if (g_last_error[0] != '\0') {
                        report_database_error("%s", g_last_error);
                    } else {
                        report_database_error("could not obtain lock on row in table '%s'", tname);
                    }
                    for (int prev_r = 0; prev_r < r; prev_r++) {
                        uint32_t prev_rid = (uint32_t)(results[prev_r].indices[0] + 1);
                        lock_manager_release_row(sess, tname, prev_rid);
                    }
                    goto cleanup;
                }
            }
            for (int t = 0; t < scope.count; t++) {
                scope.rows[t] = results[r].indices[t] >= 0 ? &scope.tables[t]->rows[results[r].indices[t]] : NULL;
            }
            RowData* row = &rows[total_output_rows++];
            for (int i = 0; i < proj_count; i++) {
                int t_idx = proj_items[i].binding.table;
                const Value* src = (!(t_idx & 0x1000) && t_idx < scope.count && results[r].indices[t_idx] >= 0) ?
                    &scope.rows[t_idx]->values[proj_items[i].binding.column] : &null_val;
                const Value* src2 = NULL;
                if (proj_items[i].has_binding2) {
                    int t2 = proj_items[i].binding2.table;
                    src2 = (!(t2 & 0x1000) && t2 < scope.count && results[r].indices[t2] >= 0) ?
                        &scope.rows[t2]->values[proj_items[i].binding2.column] : &null_val;
                }

                if (proj_items[i].type == PROJ_ROW_NUMBER || proj_items[i].type == PROJ_RANK || proj_items[i].type == PROJ_DENSE_RANK) {
                    /* Placeholder; real values are assigned after final sorting. */
                    snprintf(row->cells[i].text, sizeof(row->cells[i].text), "%d", total_output_rows);
                    row->cells[i].is_null = false;
                } else if (proj_items[i].has_literal1 && !proj_items[i].raw_name.column[0]) {
                    row->cells[i] = proj_items[i].literal1;
                } else if (proj_items[i].type == PROJ_JSON_EXTRACT) {
                    if (src->is_null) {
                        row->cells[i] = null_val;
                    } else {
                        bool ok_extract = extract_json_value(src->text, proj_items[i].json_path, row->cells[i].text, sizeof(row->cells[i].text));
                        row->cells[i].is_null = !ok_extract || row->cells[i].text[0] == '\0';
                    }
                } else if (proj_items[i].type == PROJ_CASE) {
                    evaluate_case_item(&proj_items[i], &scope, &row->cells[i]);
                } else if (proj_items[i].type >= PROJ_UPPER && proj_items[i].type <= PROJ_IDENTITY) {
                    evaluate_scalar_item(&proj_items[i], src, src2, &row->cells[i]);
                } else if (proj_items[i].expr_op != 0 && !src->is_null && !proj_items[i].expr_operand.is_null) {
                    long double res = 0;
                    if (proj_items[i].expr_op == 'C') {
                        res = (long double)vector_cosine_distance(src->text, proj_items[i].expr_operand.text);
                        snprintf(row->cells[i].text, sizeof(row->cells[i].text), "%.4Lf", res);
                        char norm_buf[MAX_VAL_LEN];
                        if (normalize_decimal(row->cells[i].text, norm_buf)) {
                            snprintf(row->cells[i].text, sizeof(row->cells[i].text), "%s", norm_buf);
                        }
                    } else if (proj_items[i].expr_op == 'L') {
                        res = (long double)vector_l2_distance(src->text, proj_items[i].expr_operand.text);
                        snprintf(row->cells[i].text, sizeof(row->cells[i].text), "%.4Lf", res);
                        char norm_buf[MAX_VAL_LEN];
                        if (normalize_decimal(row->cells[i].text, norm_buf)) {
                            snprintf(row->cells[i].text, sizeof(row->cells[i].text), "%s", norm_buf);
                        }
                    } else {
                        long double a = strtold(src->text, NULL);
                        long double b = strtold(proj_items[i].expr_operand.text, NULL);
                        switch (proj_items[i].expr_op) {
                        case '+': res = a + b; break;
                        case '-': res = a - b; break;
                        case '*': res = a * b; break;
                        case '/': res = (b != 0) ? (a / b) : 0; break;
                        }
                        format_decimal_val(res, row->cells[i].text, sizeof(row->cells[i].text));
                    }
                    row->cells[i].is_null = false;
                } else {
                    row->cells[i] = *src;
                }
            }
        }
    }

    if (distinct && total_output_rows > 1) {
        int write_idx = 0;
        for (int i = 0; i < total_output_rows; i++) {
            bool duplicate = false;
            for (int j = 0; j < write_idx; j++) {
                bool match = true;
                for (int c = 0; c < proj_count; c++) {
                    if (proj_items[c].is_hidden) continue;
                    if (rows[i].cells[c].is_null != rows[j].cells[c].is_null ||
                        (!rows[i].cells[c].is_null && strcmp(rows[i].cells[c].text, rows[j].cells[c].text) != 0)) {
                        match = false; break;
                    }
                }
                if (match) { duplicate = true; break; }
            }
            if (!duplicate) {
                if (write_idx != i) rows[write_idx] = rows[i];
                write_idx++;
            }
        }
        total_output_rows = write_idx;
    }

    /* If no explicit ORDER BY was specified, check if any window function requires sorting */
    if (sort_count == 0 && total_output_rows > 1) {
        for (int i = 0; i < proj_count; i++) {
            if (proj_items[i].type == PROJ_ROW_NUMBER || proj_items[i].type == PROJ_RANK || proj_items[i].type == PROJ_DENSE_RANK) {
                if (proj_items[i].has_partition) {
                    for (int c = 0; c < proj_count; c++) {
                        if (proj_items[c].type == PROJ_COL &&
                            proj_items[c].binding.table == proj_items[i].partition_binding.table &&
                            proj_items[c].binding.column == proj_items[i].partition_binding.column) {
                            sort_keys[sort_count].proj_idx = c;
                            sort_keys[sort_count].desc = false;
                            sort_keys[sort_count].type = proj_items[c].data_type;
                            sort_count++;
                            break;
                        }
                    }
                }
                if (proj_items[i].raw_name.column[0]) {
                    for (int c = 0; c < proj_count; c++) {
                        if (proj_items[c].type == PROJ_COL &&
                            proj_items[c].binding.table == proj_items[i].binding.table &&
                            proj_items[c].binding.column == proj_items[i].binding.column) {
                            sort_keys[sort_count].proj_idx = c;
                            sort_keys[sort_count].desc = (proj_items[i].expr_op == 'D');
                            sort_keys[sort_count].type = proj_items[c].data_type;
                            sort_count++;
                            break;
                        }
                    }
                }
                if (sort_count > 0) break;
            }
        }
    } else if (sort_count > 0 && total_output_rows > 1) {
        for (int i = 0; i < proj_count; i++) {
            if ((proj_items[i].type == PROJ_ROW_NUMBER || proj_items[i].type == PROJ_RANK || proj_items[i].type == PROJ_DENSE_RANK) &&
                proj_items[i].has_partition && sort_count < MAX_COLUMNS) {
                int part_proj = -1;
                for (int c = 0; c < proj_count; c++) {
                    if (proj_items[c].type == PROJ_COL &&
                        proj_items[c].binding.table == proj_items[i].partition_binding.table &&
                        proj_items[c].binding.column == proj_items[i].partition_binding.column) {
                        part_proj = c; break;
                    }
                }
                if (part_proj >= 0 && sort_keys[0].proj_idx != part_proj) {
                    for (int s = sort_count; s > 0; s--) sort_keys[s] = sort_keys[s - 1];
                    sort_keys[0].proj_idx = part_proj;
                    sort_keys[0].desc = false;
                    sort_keys[0].type = proj_items[part_proj].data_type;
                    sort_count++;
                }
                break;
            }
        }
    }

    if (sort_count > 0 && total_output_rows > 1) {
        sort_row_data(rows, total_output_rows, sort_keys, sort_count);
    }

    /* Window functions are evaluated over the final (sorted) row order so
       ROW_NUMBER() reflects ORDER BY and RANK() ties share a rank. */
    for (int i = 0; i < proj_count; i++) {
        if (proj_items[i].type != PROJ_ROW_NUMBER && proj_items[i].type != PROJ_RANK && proj_items[i].type != PROJ_DENSE_RANK) continue;
        int order_idx = -1;
        bool order_desc = proj_items[i].expr_op == 'D';
        if (proj_items[i].raw_name.column[0] && !(proj_items[i].binding.table & 0x1000)) {
            for (int c = 0; c < proj_count; c++) {
                if (proj_items[c].type == PROJ_COL &&
                    proj_items[c].binding.table == proj_items[i].binding.table &&
                    proj_items[c].binding.column == proj_items[i].binding.column) {
                    order_idx = c;
                    break;
                }
            }
        }
        int part_idx = -1;
        if (proj_items[i].has_partition && !(proj_items[i].partition_binding.table & 0x1000)) {
            for (int c = 0; c < proj_count; c++) {
                if (proj_items[c].type == PROJ_COL &&
                    proj_items[c].binding.table == proj_items[i].partition_binding.table &&
                    proj_items[c].binding.column == proj_items[i].partition_binding.column) {
                    part_idx = c;
                    break;
                }
            }
        }
        int rank = 0, seq = 0, dense_rank = 0;
        for (int r = 0; r < total_output_rows; r++) {
            bool partition_start = (r == 0);
            if (part_idx >= 0 && r > 0) {
                const Value* cur_p = &rows[r].cells[part_idx];
                const Value* prev_p = &rows[r - 1].cells[part_idx];
                if ((cur_p->is_null != prev_p->is_null) ||
                    (!cur_p->is_null && !prev_p->is_null &&
                     compare_typed_values(proj_items[part_idx].data_type, cur_p, prev_p) != 0)) {
                    partition_start = true;
                }
            }
            if (partition_start) {
                seq = 1;
                rank = 1;
                dense_rank = 1;
            } else {
                seq++;
                if (proj_items[i].type == PROJ_ROW_NUMBER || order_idx < 0) {
                    rank = seq;
                    dense_rank = seq;
                } else {
                    const Value* cur = &rows[r].cells[order_idx];
                    const Value* prev = &rows[r - 1].cells[order_idx];
                    bool same = (cur->is_null && prev->is_null) ||
                        (!cur->is_null && !prev->is_null &&
                         compare_typed_values(proj_items[order_idx].data_type, cur, prev) == 0);
                    rank = same ? rank : seq;
                    dense_rank = same ? dense_rank : (dense_rank + 1);
                }
            }
            (void)order_desc;
            int out_val = (proj_items[i].type == PROJ_DENSE_RANK) ? dense_rank : rank;
            snprintf(rows[r].cells[i].text, sizeof(rows[r].cells[i].text), "%d", out_val);
            rows[r].cells[i].is_null = false;
        }
    }

    out_capture->col_count = 0;
    for (int c = 0; c < proj_count; c++) {
        if (proj_items[c].is_hidden) continue;
        snprintf(out_capture->col_names[out_capture->col_count++],
                 MAX_NAME_LEN, "%s", proj_items[c].display_name);
    }

    int num_rows = total_output_rows;
    out_capture->rows = malloc((size_t)(num_rows > 0 ? num_rows : 1) * sizeof(*out_capture->rows));
    if (!out_capture->rows) {
        report_database_error("out of memory capturing query results");
        goto cleanup;
    }
    out_capture->capacity = num_rows > 0 ? num_rows : 1;
    out_capture->row_count = 0;
    for (int r = 0; r < total_output_rows; r++) {
        int out_c = 0;
        for (int c = 0; c < proj_count; c++) {
            if (proj_items[c].is_hidden) continue;
            out_capture->rows[out_capture->row_count][out_c++] = rows[r].cells[c];
        }
        out_capture->row_count++;
    }

    ok = true;

cleanup:
    g_select_max_needed = MAX_RESULTS;
    for (int i = 1; i < scope.count; i++) {
        if (use_hash[i]) join_index_destroy(&join_indexes[i]);
    }
    if (right_matched) free(right_matched);
    if (results) free(results);
    if (rows) free(rows);
    if (groups) free(groups);
    free(joins);
    free(filter);
    free(proj_items);
    return ok;
}

static bool execute_select_query_impl(Parser* p) {
    QueryResultCapture main_cap = {0};
    main_cap.silent = g_result_capture ? g_result_capture->silent : false;

    int limit = MAX_RESULTS;
    int offset = 0;
    if (!execute_select_query_internal(p, &main_cap, &limit, &offset)) {
        free(main_cap.rows);
        return false;
    }

    while (token_is_keyword(p, "UNION") || token_is_keyword(p, "INTERSECT") ||
           token_is_keyword(p, "MINUS") || token_is_keyword(p, "EXCEPT")) {
        SetOperation op = SET_OP_NONE;
        if (consume_keyword(p, "UNION")) {
            if (consume_keyword(p, "ALL")) op = SET_OP_UNION_ALL;
            else op = SET_OP_UNION;
        } else if (consume_keyword(p, "INTERSECT")) {
            op = SET_OP_INTERSECT;
        } else if (consume_keyword(p, "MINUS") || consume_keyword(p, "EXCEPT")) {
            op = SET_OP_MINUS;
        }

        if (!consume_keyword(p, "SELECT")) {
            free(main_cap.rows);
            return report_database_error("expected SELECT following set operator");
        }

        QueryResultCapture sub_cap = {0};
        sub_cap.silent = true;
        int sub_lim = MAX_RESULTS, sub_off = 0;
        if (!execute_select_query_internal(p, &sub_cap, &sub_lim, &sub_off)) {
            free(sub_cap.rows);
            free(main_cap.rows);
            return false;
        }

        if (!combine_captures(&main_cap, &sub_cap, op)) {
            free(sub_cap.rows);
            free(main_cap.rows);
            return false;
        }
        free(sub_cap.rows);
    }

    if (p->kind != TK_RP && !consume_statement_end(p)) {
        free(main_cap.rows);
        return false;
    }

    if (g_result_capture) {
        g_result_capture->col_count = main_cap.col_count;
        for (int c = 0; c < main_cap.col_count; c++) {
            snprintf(g_result_capture->col_names[c], MAX_NAME_LEN, "%s", main_cap.col_names[c]);
        }
        int available = main_cap.row_count > offset ? main_cap.row_count - offset : 0;
        int num_rows = available < limit ? available : limit;
        if (g_result_capture->capacity < num_rows) {
            Value (*rows)[MAX_COLUMNS] = realloc(g_result_capture->rows,
                (size_t)num_rows * sizeof(*rows));
            if (!rows) {
                free(main_cap.rows);
                return report_database_error("out of memory capturing query results");
            }
            g_result_capture->rows = rows;
            g_result_capture->capacity = num_rows;
        }
        g_result_capture->row_count = 0;
        for (int r = offset; r < main_cap.row_count && g_result_capture->row_count < limit; r++) {
            for (int c = 0; c < main_cap.col_count; c++) {
                g_result_capture->rows[g_result_capture->row_count][c] = main_cap.rows[r][c];
            }
            g_result_capture->row_count++;
        }
    }

    if (!g_result_capture || !g_result_capture->silent) {
        render_capture_to_output(&main_cap, limit, offset);
    }

    free(main_cap.rows);
    return true;
}

bool execute_select_query(Parser* p) {
    static unsigned depth = 0;
    if (depth >= 16) return report_database_error("SELECT nesting limit exceeded");
    depth++;
    bool ok = execute_select_query_impl(p);
    depth--;
    return ok;
}

bool execute_with_query(Parser* p) {
    /* WITH materializes a temporary table in g_db, so it is a mutation and
       must not run in read-only mode or on follower nodes. */
    if (g_readonly) return report_database_error("cannot execute WITH in read-only mode");
    if (g_raft_role == RAFT_FOLLOWER) return report_database_error("follower nodes accept read-only queries");
    char cte_name[MAX_NAME_LEN];
    if (!parse_identifier(p, cte_name) || !consume_keyword(p, "AS") || !consume_token(p, TK_LP)) {
        return report_database_error("expected CTE identifier AS (SELECT ...)");
    }
    if (!consume_keyword(p, "SELECT")) {
        return report_database_error("expected SELECT in CTE definition");
    }
    QueryResultCapture capture = {0};
    capture.silent = true;
    QueryResultCapture* old_cap = g_result_capture;
    g_result_capture = &capture;
    bool cte_ok = execute_select_query(p);
    g_result_capture = old_cap;
    if (!cte_ok || !consume_token(p, TK_RP)) {
        free(capture.rows);
        return false;
    }
    Table* cte_table = calloc(1, sizeof(*cte_table));
    if (!cte_table) {
        free(capture.rows);
        return report_database_error("failed to allocate memory for CTE table");
    }
    memcpy(cte_table->name, cte_name, sizeof(cte_table->name));
    cte_table->col_count = capture.col_count;
    for (int c = 0; c < capture.col_count; c++) {
        memcpy(cte_table->columns[c].name, capture.col_names[c], sizeof(cte_table->columns[c].name));
        cte_table->columns[c].type = TYPE_VARCHAR2;
        cte_table->columns[c].size = 255;
        cte_table->columns[c].nullable = true;
    }
    if (!allocate_table_storage(cte_table, capture.row_count > 0 ? capture.row_count : 16)) {
        free(cte_table);
        free(capture.rows);
        return report_database_error("failed to allocate memory for CTE table");
    }
    for (int r = 0; r < capture.row_count; r++) {
        cte_table->rows[r] = (Row){0};
        for (int c = 0; c < capture.col_count; c++) {
            cte_table->rows[r].values[c] = capture.rows[r][c];
        }
    }
    cte_table->row_count = capture.row_count;
    free(capture.rows);

    if (g_db->table_count >= MAX_TABLES) {
        release_table_storage(cte_table);
        free(cte_table);
        return report_database_error("database table capacity reached for CTE");
    }
    g_db->tables[g_db->table_count++] = *cte_table;
    free(cte_table);

    if (!consume_keyword(p, "SELECT")) {
        release_table_storage(&g_db->tables[--g_db->table_count]);
        memset(&g_db->tables[g_db->table_count], 0, sizeof(Table));
        return report_database_error("expected SELECT following WITH clause");
    }

    bool ok = execute_select_query(p);
    release_table_storage(&g_db->tables[--g_db->table_count]);
    memset(&g_db->tables[g_db->table_count], 0, sizeof(Table));
    return ok;
}
