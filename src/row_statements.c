#include "database_engine.h"

typedef enum {
    UPSERT_EXPR_LITERAL = 0,
    UPSERT_EXPR_EXISTING_COL,
    UPSERT_EXPR_EXCLUDED_COL
} UpsertOperandKind;

typedef struct {
    int target_col;
    UpsertOperandKind lhs_kind;
    Value lhs_lit;
    int lhs_col;
    char op;
    UpsertOperandKind rhs_kind;
    Value rhs_lit;
    int rhs_col;
} UpsertAssignment;

/* Bound on referential cascade recursion. Self-referencing or cyclic foreign
   keys with cascading actions would otherwise recurse until the stack is
   exhausted. */
#define MAX_FK_CASCADE_DEPTH 32

static bool cascade_delete_parent_value(Database* db, const char* parent_tbl, const char* parent_col, const Value* val, int depth);
static bool cascade_update_parent_value(Database* db, const char* parent_tbl, const char* parent_col, const Value* old_val, const Value* new_val, int depth);
static bool db_has_cascading_cycle(Database* db);

static bool parse_upsert_operand(Parser* p, const Table* table, int target_col,
                                 UpsertOperandKind* out_kind, Value* out_lit, int* out_col) {
    memset(out_lit, 0, sizeof(*out_lit));
    *out_col = -1;
    if (consume_keyword(p, "EXCLUDED")) {
        if (!consume_token(p, TK_DOT)) return false;
        char cname[MAX_NAME_LEN];
        if (!parse_identifier(p, cname)) return false;
        int c = find_column_index(table, cname);
        if (c < 0) return report_database_error("column does not exist in EXCLUDED: %s", cname);
        *out_kind = UPSERT_EXPR_EXCLUDED_COL;
        *out_col = c;
        return true;
    }
    if (consume_keyword(p, "VALUES")) {
        if (!consume_token(p, TK_LP)) return false;
        char cname[MAX_NAME_LEN];
        if (!parse_identifier(p, cname) || !consume_token(p, TK_RP)) return false;
        int c = find_column_index(table, cname);
        if (c < 0) return report_database_error("column does not exist in VALUES(): %s", cname);
        *out_kind = UPSERT_EXPR_EXCLUDED_COL;
        *out_col = c;
        return true;
    }
    if (p->kind == TK_ID && strcmp(p->text, "NULL") != 0) {
        char first_id[MAX_NAME_LEN];
        if (!parse_identifier(p, first_id)) return false;
        if (consume_token(p, TK_DOT)) {
            char second_id[MAX_NAME_LEN];
            if (!parse_identifier(p, second_id)) return false;
            int c = find_column_index(table, second_id);
            if (c < 0) return report_database_error("column does not exist: %s", second_id);
            if (strcmp(first_id, "EXCLUDED") == 0) {
                *out_kind = UPSERT_EXPR_EXCLUDED_COL;
            } else if (strcmp(first_id, table->name) == 0) {
                *out_kind = UPSERT_EXPR_EXISTING_COL;
            } else {
                return report_database_error("unknown qualifier in UPSERT: %s", first_id);
            }
            *out_col = c;
            return true;
        }
        int c = find_column_index(table, first_id);
        if (c < 0) return report_database_error("column does not exist: %s", first_id);
        *out_kind = UPSERT_EXPR_EXISTING_COL;
        *out_col = c;
        return true;
    }
    *out_kind = UPSERT_EXPR_LITERAL;
    return parse_typed_literal(p, &table->columns[target_col], out_lit);
}

static Value eval_upsert_operand(UpsertOperandKind kind, const Value* lit, int col_idx,
                                 const Row* existing_row, const Row* excluded_row) {
    if (kind == UPSERT_EXPR_EXISTING_COL && col_idx >= 0) return existing_row->values[col_idx];
    if (kind == UPSERT_EXPR_EXCLUDED_COL && col_idx >= 0) return excluded_row->values[col_idx];
    return *lit;
}

static int find_conflicting_row(const Table* table, const Row* candidate,
                                const int* conflict_cols, int conflict_col_count) {
    for (int r = 0; r < table->row_count; r++) {
        const Row* er = &table->rows[r];
        if (conflict_col_count > 0) {
            bool match = true;
            for (int k = 0; k < conflict_col_count; k++) {
                int c = conflict_cols[k];
                if (er->values[c].is_null || candidate->values[c].is_null ||
                    compare_typed_values(table->columns[c].type, &er->values[c], &candidate->values[c]) != 0) {
                    match = false;
                    break;
                }
            }
            if (match) return r;
        } else {
            for (int c = 0; c < table->col_count; c++) {
                if (table->columns[c].primary || table->columns[c].unique) {
                    if (!er->values[c].is_null && !candidate->values[c].is_null &&
                        compare_typed_values(table->columns[c].type, &er->values[c], &candidate->values[c]) == 0) {
                        return r;
                    }
                }
            }
            if (table->pk_col_count > 1) {
                bool match = true;
                for (int k = 0; k < table->pk_col_count; k++) {
                    int c = table->pk_cols[k];
                    if (er->values[c].is_null || candidate->values[c].is_null ||
                        compare_typed_values(table->columns[c].type, &er->values[c], &candidate->values[c]) != 0) {
                        match = false;
                        break;
                    }
                }
                if (match) return r;
            }
            for (int u = 0; u < table->uq_count; u++) {
                if (table->uq_col_counts[u] > 1) {
                    bool match = true;
                    for (int k = 0; k < table->uq_col_counts[u]; k++) {
                        int c = table->uq_cols[u][k];
                        if (er->values[c].is_null || candidate->values[c].is_null ||
                            compare_typed_values(table->columns[c].type, &er->values[c], &candidate->values[c]) != 0) {
                            match = false;
                            break;
                        }
                    }
                    if (match) return r;
                }
            }
        }
    }
    return -1;
}

bool execute_insert_row(Parser* p, Database* db, int* inserted) {
    *inserted = 0;
    if (!consume_keyword(p, "INTO")) return false;
    Table* table = resolve_statement_table(p, db);
    if (!table) return false;
    if (!user_has_table_privilege(db, g_network_user, table->name, PRIV_INSERT))
        return report_database_error("permission denied: INSERT on %s", table->name);
    int col_indices[MAX_COLUMNS];
    int col_count = 0;
    if (consume_token(p, TK_LP)) {
        do {
            char name[MAX_NAME_LEN];
            if (col_count == MAX_COLUMNS || !parse_identifier(p, name)) return false;
            int c = find_column_index(table, name);
            if (c < 0) return report_database_error("column does not exist: %s", name);
            for (int i = 0; i < col_count; i++)
                if (col_indices[i] == c) return report_database_error("duplicate column in INSERT: %s", name);
            col_indices[col_count++] = c;
        } while (consume_token(p, TK_COMMA));
        if (!consume_token(p, TK_RP)) return false;
    } else {
        col_count = table->col_count;
        for (int c = 0; c < col_count; c++) col_indices[c] = c;
    }
    if (!consume_keyword(p, "VALUES")) return false;

    int batch_cap = 16;
    int batch_count = 0;
    Row* batch = calloc((size_t)batch_cap, sizeof(Row));
    if (!batch) return report_database_error("out of memory");

    do {
        if (!consume_token(p, TK_LP)) { free(batch); return false; }
        Row* row = calloc(1, sizeof(Row));
        if (!row) { free(batch); return report_database_error("out of memory"); }
        for (int c = 0; c < table->col_count; c++) row->values[c].is_null = true;
        for (int i = 0; i < col_count; i++) {
            if (i && !consume_token(p, TK_COMMA)) { free(row); free(batch); return false; }
            int c = col_indices[i];
            if (!parse_typed_literal(p, &table->columns[c], &row->values[c])) { free(row); free(batch); return false; }
        }
        if (!consume_token(p, TK_RP)) { free(row); free(batch); return false; }
        if (batch_count == batch_cap) {
            int new_cap = batch_cap * 2;
            Row* grown = realloc(batch, (size_t)new_cap * sizeof(Row));
            if (!grown) { free(row); free(batch); return report_database_error("out of memory"); }
            batch = grown;
            batch_cap = new_cap;
        }
        batch[batch_count++] = *row;
        free(row);
    } while (consume_token(p, TK_COMMA));

    bool has_upsert = false;
    bool do_nothing = false;
    int conflict_cols[MAX_COLUMNS];
    int conflict_col_count = 0;
    UpsertAssignment* upsert_assigns = calloc(MAX_COLUMNS, sizeof(UpsertAssignment));
    if (!upsert_assigns) { free(batch); return report_database_error("out of memory"); }
    int upsert_assign_count = 0;

    if (consume_keyword(p, "ON")) {
        has_upsert = true;
        if (consume_keyword(p, "CONFLICT")) {
            if (consume_token(p, TK_LP)) {
                do {
                    char cname[MAX_NAME_LEN];
                    if (conflict_col_count >= MAX_COLUMNS || !parse_identifier(p, cname)) {
                        free(upsert_assigns); free(batch);
                        return false;
                    }
                    int c = find_column_index(table, cname);
                    if (c < 0) {
                        free(upsert_assigns); free(batch);
                        return report_database_error("column does not exist in ON CONFLICT: %s", cname);
                    }
                    conflict_cols[conflict_col_count++] = c;
                } while (consume_token(p, TK_COMMA));
                if (!consume_token(p, TK_RP)) { free(upsert_assigns); free(batch); return false; }
            }
            if (!consume_keyword(p, "DO")) { free(upsert_assigns); free(batch); return false; }
            if (consume_keyword(p, "NOTHING")) {
                do_nothing = true;
            } else if (consume_keyword(p, "UPDATE")) {
                if (!consume_keyword(p, "SET")) { free(upsert_assigns); free(batch); return false; }
            } else {
                free(upsert_assigns); free(batch);
                return false;
            }
        } else if (consume_keyword(p, "DUPLICATE")) {
            if (!consume_keyword(p, "KEY") || !consume_keyword(p, "UPDATE")) {
                free(upsert_assigns); free(batch);
                return false;
            }
        } else {
            free(upsert_assigns); free(batch);
            return false;
        }

        if (!do_nothing) {
            do {
                char target_name[MAX_NAME_LEN];
                if (upsert_assign_count >= MAX_COLUMNS || !parse_identifier(p, target_name)) {
                    free(upsert_assigns); free(batch);
                    return false;
                }
                int tc = find_column_index(table, target_name);
                if (tc < 0 || !consume_token(p, TK_EQ)) {
                    free(upsert_assigns); free(batch);
                    return false;
                }
                UpsertAssignment* ua = &upsert_assigns[upsert_assign_count];
                memset(ua, 0, sizeof(*ua));
                ua->target_col = tc;
                if (!parse_upsert_operand(p, table, tc, &ua->lhs_kind, &ua->lhs_lit, &ua->lhs_col)) {
                    free(upsert_assigns); free(batch);
                    return false;
                }
                if (p->kind == TK_PLUS || p->kind == TK_MINUS || p->kind == TK_STAR || p->kind == TK_SLASH) {
                    ua->op = (p->kind == TK_PLUS) ? '+' : (p->kind == TK_MINUS) ? '-' : (p->kind == TK_STAR) ? '*' : '/';
                    scan_next_token(p);
                    if (!parse_upsert_operand(p, table, tc, &ua->rhs_kind, &ua->rhs_lit, &ua->rhs_col)) {
                        free(upsert_assigns); free(batch);
                        return false;
                    }
                }
                upsert_assign_count++;
            } while (consume_token(p, TK_COMMA));
        }
    }

    if (!consume_statement_end(p)) {
        free(upsert_assigns); free(batch);
        return false;
    }

    for (int b = 0; b < batch_count; b++) {
        Row* candidate = &batch[b];
        int conflict_idx = has_upsert ? find_conflicting_row(table, candidate, conflict_cols, conflict_col_count) : -1;
        if (conflict_idx >= 0) {
            if (do_nothing) {
                continue;
            }
            if (!ensure_table_writable(table)) {
                free(upsert_assigns); free(batch);
                return report_database_error("unable to allocate row storage");
            }
            Row* existing_copy = malloc(sizeof(Row));
            if (!existing_copy) { free(upsert_assigns); free(batch); return report_database_error("out of memory"); }
            *existing_copy = table->rows[conflict_idx];
            for (int a = 0; a < upsert_assign_count; a++) {
                const UpsertAssignment* ua = &upsert_assigns[a];
                int tc = ua->target_col;
                Value lhs_val = eval_upsert_operand(ua->lhs_kind, &ua->lhs_lit, ua->lhs_col, existing_copy, candidate);
                Value new_val = lhs_val;
                if (ua->op != 0) {
                    Value rhs_val = eval_upsert_operand(ua->rhs_kind, &ua->rhs_lit, ua->rhs_col, existing_copy, candidate);
                    if (lhs_val.is_null || rhs_val.is_null) {
                        new_val.is_null = true;
                        new_val.text[0] = '\0';
                    } else if (table->columns[tc].type == TYPE_NUMBER) {
                        long double lv = strtold(lhs_val.text, NULL);
                        long double rv = strtold(rhs_val.text, NULL);
                        long double res = 0.0L;
                        if (ua->op == '+') res = lv + rv;
                        else if (ua->op == '-') res = lv - rv;
                        else if (ua->op == '*') res = lv * rv;
                        else if (ua->op == '/') {
                            if (rv == 0.0L) {
                                free(existing_copy); free(upsert_assigns); free(batch);
                                return report_database_error("division by zero in UPSERT");
                            }
                            res = lv / rv;
                        }
                        char raw_num[MAX_VAL_LEN];
                        snprintf(raw_num, sizeof(raw_num), "%.10Lf", res);
                        if (!normalize_decimal(raw_num, new_val.text)) {
                            free(existing_copy); free(upsert_assigns); free(batch);
                            return report_database_error("numeric overflow in UPSERT");
                        }
                        new_val.is_null = false;
                    }
                }
                if (!is_valid_column_value(&table->columns[tc], &new_val)) {
                    free(existing_copy); free(upsert_assigns); free(batch);
                    return report_database_error("invalid column value in UPSERT for %s.%s", table->name, table->columns[tc].name);
                }
                Value old_val = table->rows[conflict_idx].values[tc];
                if (compare_typed_values(table->columns[tc].type, &old_val, &new_val) != 0) {
                    if (!cascade_update_parent_value(db, table->name, table->columns[tc].name, &old_val, &new_val, 1)) {
                        free(existing_copy); free(upsert_assigns); free(batch);
                        return false;
                    }
                }
                table->rows[conflict_idx].values[tc] = new_val;
            }
            free(existing_copy);
            table->dead_row_count++;
            (*inserted)++;
        } else {
            if (table->part_type == PART_RANGE && get_row_partition(table, candidate) < 0) {
                free(upsert_assigns); free(batch);
                return report_database_error("partition key value is outside all defined range partitions for table %s", table->name);
            }
            if (!reserve_insert_capacity(table)) {
                free(upsert_assigns); free(batch);
                return false;
            }
            if (table->engine_type == ENGINE_DISK) {
                uint8_t slotted_buf[PAGE_SIZE];
                encode_slotted_row(table->columns, table->col_count, candidate, slotted_buf, sizeof(slotted_buf));
            }
            table->rows[table->row_count++] = *candidate;
            (*inserted)++;
        }
    }

    free(upsert_assigns);
    free(batch);
    rebuild_secondary_indexes(db);
    return true;
}

static bool cascade_delete_parent_value(Database* db, const char* parent_tbl, const char* parent_col, const Value* val, int depth) {
    if (val->is_null) return true;
    if (depth > MAX_FK_CASCADE_DEPTH)
        return report_database_error("foreign key cascade depth limit reached (cyclic references?)");
    for (int t = 0; t < db->table_count; t++) {
        Table* child = &db->tables[t];
        for (int c = 0; c < child->col_count; c++) {
            ColumnDef* cdef = &child->columns[c];
            if (strcmp(cdef->ref_table, parent_tbl) == 0 && strcmp(cdef->ref_column, parent_col) == 0) {
                if (cdef->fk_on_delete == FK_ACTION_CASCADE) {
                    if (child->row_count > 0 && !ensure_table_writable(child)) return false;
                    int child_write = 0;
                    for (int cr = 0; cr < child->row_count; cr++) {
                        Value* cv = &child->rows[cr].values[c];
                        if (!cv->is_null && compare_typed_values(cdef->type, cv, val) == 0) {
                            child->dead_row_count++;
                            for (int cc = 0; cc < child->col_count; cc++) {
                                if (!cascade_delete_parent_value(db, child->name, child->columns[cc].name, &child->rows[cr].values[cc], depth + 1)) {
                                    return false;
                                }
                            }
                        } else {
                            if (child_write != cr) {
                                child->rows[child_write] = child->rows[cr];
                            }
                            child_write++;
                        }
                    }
                    child->row_count = child_write;
                } else if (cdef->fk_on_delete == FK_ACTION_SET_NULL) {
                    if (!cdef->nullable) {
                        return report_database_error("cannot SET NULL on NOT NULL column %s.%s", child->name, cdef->name);
                    }
                    if (child->row_count > 0 && !ensure_table_writable(child)) return false;
                    for (int cr = 0; cr < child->row_count; cr++) {
                        Value* cv = &child->rows[cr].values[c];
                        if (!cv->is_null && compare_typed_values(cdef->type, cv, val) == 0) {
                            cv->is_null = true;
                            cv->text[0] = '\0';
                        }
                    }
                }
            }
        }
    }
    return true;
}

static bool cascade_update_parent_value(Database* db, const char* parent_tbl, const char* parent_col, const Value* old_val, const Value* new_val, int depth) {
    if (old_val->is_null) return true;
    if (depth > MAX_FK_CASCADE_DEPTH)
        return report_database_error("foreign key cascade depth limit reached (cyclic references?)");
    for (int t = 0; t < db->table_count; t++) {
        Table* child = &db->tables[t];
        for (int c = 0; c < child->col_count; c++) {
            ColumnDef* cdef = &child->columns[c];
            if (strcmp(cdef->ref_table, parent_tbl) == 0 && strcmp(cdef->ref_column, parent_col) == 0) {
                if (cdef->fk_on_update == FK_ACTION_CASCADE) {
                    if (child->row_count > 0 && !ensure_table_writable(child)) return false;
                    for (int cr = 0; cr < child->row_count; cr++) {
                        Value* cv = &child->rows[cr].values[c];
                        if (!cv->is_null && compare_typed_values(cdef->type, cv, old_val) == 0) {
                            Value prev_child_val = *cv;
                            *cv = *new_val;
                            if (!cascade_update_parent_value(db, child->name, cdef->name, &prev_child_val, new_val, depth + 1)) {
                                return false;
                            }
                        }
                    }
                } else if (cdef->fk_on_update == FK_ACTION_SET_NULL) {
                    if (!cdef->nullable) {
                        return report_database_error("cannot SET NULL on NOT NULL column %s.%s", child->name, cdef->name);
                    }
                    if (child->row_count > 0 && !ensure_table_writable(child)) return false;
                    for (int cr = 0; cr < child->row_count; cr++) {
                        Value* cv = &child->rows[cr].values[c];
                        if (!cv->is_null && compare_typed_values(cdef->type, cv, old_val) == 0) {
                            cv->is_null = true;
                            cv->text[0] = '\0';
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool execute_update_or_delete(Parser* p, Database* db, bool deleting, int* affected) {
    if (deleting && !consume_keyword(p, "FROM")) return false;
    Table* table = resolve_statement_table(p, db);
    if (!table) return false;
    if (!user_has_table_privilege(db, g_network_user, table->name,
                                  deleting ? PRIV_DELETE : PRIV_UPDATE))
        return report_database_error("permission denied: %s on %s",
                                     deleting ? "DELETE" : "UPDATE", table->name);
    int assignment_columns[MAX_COLUMNS], assignments = 0;
    Value* assignment_values = calloc(MAX_COLUMNS, sizeof(Value));
    if (!assignment_values) return report_database_error("out of memory");
    if (!deleting) {
        if (!consume_keyword(p, "SET")) { free(assignment_values); return false; }
        do {
            char name[MAX_NAME_LEN];
            if (assignments == MAX_COLUMNS || !parse_identifier(p, name)) { free(assignment_values); return false; }
            int c = find_column_index(table, name);
            if (c < 0 || !consume_token(p, TK_EQ)) { free(assignment_values); return false; }
            for (int i = 0; i < assignments; i++) if (assignment_columns[i] == c) { free(assignment_values); return false; }
            assignment_columns[assignments] = c;
            if (!parse_typed_literal(p, &table->columns[c], &assignment_values[assignments++])) { free(assignment_values); return false; }
        } while (consume_token(p, TK_COMMA));
    }
    Scope scope;
    memset(&scope, 0, sizeof(scope));
    scope.tables[0] = table;
    scope.count = 1;
    Filter* filter = calloc(1, sizeof(Filter));
    if (!filter) { free(assignment_values); return report_database_error("out of memory"); }
    if (!parse_where_clause(p, &scope, filter) || !consume_statement_end(p)) {
        free(filter); free(assignment_values); return false;
    }
    if (db_has_cascading_cycle(db)) {
        free(filter); free(assignment_values);
        return report_database_error("cascading foreign key cycle involving table %s is not supported", table->name);
    }
    if (table->row_count > 0 && !ensure_table_writable(table)) {
        free(filter); free(assignment_values);
        return report_database_error("unable to allocate row storage");
    }
    int write = 0; *affected = 0;
    for (int r = 0; r < table->row_count; r++) {
        scope.rows[0] = &table->rows[r];
        bool match = evaluate_where_clause(&scope, filter);
        if (match) (*affected)++;
        if (deleting) {
            if (match) {
                for (int c = 0; c < table->col_count; c++) {
                    if (!cascade_delete_parent_value(db, table->name, table->columns[c].name, &table->rows[r].values[c], 1)) {
                        free(filter); free(assignment_values);
                        return false;
                    }
                }
            } else {
                table->rows[write++] = table->rows[r];
            }
        } else if (match) {
            for (int i = 0; i < assignments; i++) {
                int c = assignment_columns[i];
                Value old_val = table->rows[r].values[c];
                Value new_val = assignment_values[i];
                if (compare_typed_values(table->columns[c].type, &old_val, &new_val) != 0) {
                    if (!cascade_update_parent_value(db, table->name, table->columns[c].name, &old_val, &new_val, 1)) {
                        free(filter); free(assignment_values);
                        return false;
                    }
                }
                table->rows[r].values[c] = new_val;
            }
            if (table->part_type == PART_RANGE && get_row_partition(table, &table->rows[r]) < 0) {
                free(filter); free(assignment_values);
                return report_database_error("updated partition key value is outside all defined range partitions for table %s", table->name);
            }
        }
    }
    if (deleting) table->row_count = write;
    if (*affected > 0) table->dead_row_count += *affected;
    free(filter);
    free(assignment_values);
    rebuild_secondary_indexes(db);
    return true;
}

/* Detect self-referencing or cyclic cascading foreign keys up front so the
   recursive cascade walkers cannot corrupt in-place compaction state. */
static bool table_has_cascading_cycle(Database* db, const Table* table) {
    for (int c = 0; c < table->col_count; c++) {
        const ColumnDef* cdef = &table->columns[c];
        if (!cdef->ref_table[0]) continue;
        if (cdef->fk_on_delete != FK_ACTION_CASCADE && cdef->fk_on_update != FK_ACTION_CASCADE) continue;
        if (!strcmp(cdef->ref_table, table->name)) return true;
    }
    /* Two-table cycle: this table references another table that references back. */
    for (int c = 0; c < table->col_count; c++) {
        const ColumnDef* cdef = &table->columns[c];
        if (!cdef->ref_table[0]) continue;
        if (cdef->fk_on_delete != FK_ACTION_CASCADE && cdef->fk_on_update != FK_ACTION_CASCADE) continue;
        int other_idx = find_table_index(db, cdef->ref_table);
        if (other_idx < 0) continue;
        const Table* other = &db->tables[other_idx];
        for (int oc = 0; oc < other->col_count; oc++) {
            const ColumnDef* odef = &other->columns[oc];
            if (!odef->ref_table[0]) continue;
            if (odef->fk_on_delete != FK_ACTION_CASCADE && odef->fk_on_update != FK_ACTION_CASCADE) continue;
            if (!strcmp(odef->ref_table, table->name)) return true;
        }
    }
    return false;
}

/* Reject the statement when any cascading cycle exists in the schema, not just
   on the target table: a cascade from the target can reach the cycle. */
static bool db_has_cascading_cycle(Database* db) {
    for (int t = 0; t < db->table_count; t++) {
        if (table_has_cascading_cycle(db, &db->tables[t])) return true;
    }
    return false;
}
