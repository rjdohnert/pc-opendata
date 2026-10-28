#include "database_engine.h"

bool validate_relational_integrity(const Database* db) {
    if (db->table_count < 0 || db->table_count > MAX_TABLES) return report_database_error("invalid table count");
    int total_rows = 0;
    for (int t = 0; t < db->table_count; t++) {
        const Table* table = &db->tables[t];
        if (!is_valid_identifier(table->name) || !strcmp(table->name, "DUAL") ||
            table->col_count < 1 || table->col_count > MAX_COLUMNS ||
            table->row_count < 0 || table->row_count > MAX_ROWS)
            return report_database_error("invalid schema or row count (table %s exceeds %d rows)", table->name, MAX_ROWS);
        /* Constraint metadata must stay within its fixed arrays and reference
           real columns; corrupted counts would otherwise drive OOB access. */
        if (table->pk_col_count < 0 || table->pk_col_count > MAX_COLUMNS)
            return report_database_error("invalid primary key metadata");
        for (int k = 0; k < table->pk_col_count; k++)
            if (table->pk_cols[k] < 0 || table->pk_cols[k] >= table->col_count)
                return report_database_error("invalid primary key column reference");
        if (table->uq_count < 0 || table->uq_count > MAX_COLUMNS)
            return report_database_error("invalid unique constraint metadata");
        for (int u = 0; u < table->uq_count; u++) {
            if (table->uq_col_counts[u] < 0 || table->uq_col_counts[u] > MAX_COLUMNS)
                return report_database_error("invalid unique constraint metadata");
            for (int k = 0; k < table->uq_col_counts[u]; k++)
                if (table->uq_cols[u][k] < 0 || table->uq_cols[u][k] >= table->col_count)
                    return report_database_error("invalid unique constraint column reference");
        }
        total_rows += table->row_count;
        if (total_rows > MAX_TOTAL_ROWS) return report_database_error("total database row limit (%d) reached", MAX_TOTAL_ROWS);
        for (int other = 0; other < t; other++)
            if (!strcmp(table->name, db->tables[other].name)) return report_database_error("duplicate table");
        int primary_count = 0;
        for (int c = 0; c < table->col_count; c++) {
            const ColumnDef* column = &table->columns[c];
            if (!is_valid_identifier(column->name) || column->type < TYPE_NUMBER || column->type > TYPE_VECTOR ||
                column->size < 1 || column->size >= MAX_VAL_LEN ||
                (column->type == TYPE_NUMBER && column->size != 40) ||
                (column->type == TYPE_DATE && column->size != 19) ||
                (column->type == TYPE_VECTOR && column->size > 64) ||
                (column->primary && (column->nullable || !column->unique)))
                return report_database_error("invalid column definition");
            primary_count += column->primary;
            if (primary_count > 1 && table->pk_col_count <= 1) return report_database_error("only one single-column primary key is supported");
            for (int other = 0; other < c; other++)
                if (!strcmp(column->name, table->columns[other].name)) return report_database_error("duplicate column");
            const Table* parent = NULL; int parent_column = -1;
            if (column->ref_table[0]) {
                int p = find_table_index(db, column->ref_table);
                if (p >= 0) { parent = &db->tables[p]; parent_column = find_column_index(parent, column->ref_column); }
                if (!parent || parent_column < 0 || !parent->columns[parent_column].unique ||
                    parent->columns[parent_column].type != column->type ||
                    parent->columns[parent_column].size != column->size)
                    return report_database_error("invalid REFERENCES target for %s.%s", table->name, column->name);
            } else if (column->ref_column[0]) return report_database_error("invalid reference metadata");

            HashIndex unique_idx = {0};
            if (column->unique && table->row_count > 0) {
                hash_index_init(&unique_idx, table->row_count);
            }
            HashIndex parent_idx = {0};
            if (parent && parent->row_count > 0) {
                if (hash_index_init(&parent_idx, parent->row_count)) {
                    for (int pr = 0; pr < parent->row_count; pr++) {
                        hash_index_insert(&parent_idx, parent, parent_column, pr);
                    }
                }
            }

            bool ok = true;
            for (int r = 0; r < table->row_count; r++) {
                const Value* value = &table->rows[r].values[c];
                if (!is_valid_column_value(column, value)) {
                    report_database_error("invalid value or NOT NULL violation in %s.%s", table->name, column->name);
                    ok = false; break;
                }
                if (value->is_null) continue;
                if (column->unique) {
                    int dup = hash_index_find(&unique_idx, table, c, value);
                    if (dup >= 0) {
                        report_database_error("UNIQUE/PRIMARY KEY violation in %s.%s", table->name, column->name);
                        ok = false; break;
                    }
                    hash_index_insert(&unique_idx, table, c, r);
                }
                if (parent) {
                    int found = parent_idx.slots ? hash_index_find(&parent_idx, parent, parent_column, value) : -1;
                    if (found < 0) {
                        bool linear_found = false;
                        for (int pr = 0; pr < parent->row_count; pr++) {
                            const Value* pv = &parent->rows[pr].values[parent_column];
                            if (!pv->is_null && !compare_typed_values(column->type, value, pv)) { linear_found = true; break; }
                        }
                        if (!linear_found) {
                            report_database_error("FOREIGN KEY violation in %s.%s", table->name, column->name);
                            ok = false; break;
                        }
                    }
                }
            }
            hash_index_destroy(&unique_idx);
            hash_index_destroy(&parent_idx);
            if (!ok) return false;
        }

        if (table->pk_col_count > 1 && table->row_count > 0) {
            for (int r1 = 0; r1 < table->row_count; r1++) {
                for (int k = 0; k < table->pk_col_count; k++) {
                    int c_idx = table->pk_cols[k];
                    if (table->rows[r1].values[c_idx].is_null) {
                        return report_database_error("PRIMARY KEY column cannot be NULL: %s.%s", table->name, table->columns[c_idx].name);
                    }
                }
                for (int r2 = r1 + 1; r2 < table->row_count; r2++) {
                    bool match = true;
                    for (int k = 0; k < table->pk_col_count; k++) {
                        int c_idx = table->pk_cols[k];
                        if (compare_typed_values(table->columns[c_idx].type,
                                                 &table->rows[r1].values[c_idx],
                                                 &table->rows[r2].values[c_idx]) != 0) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        return report_database_error("composite PRIMARY KEY violation in %s", table->name);
                    }
                }
            }
        }

        for (int u = 0; u < table->uq_count; u++) {
            if (table->uq_col_counts[u] > 1 && table->row_count > 1) {
                for (int r1 = 0; r1 < table->row_count; r1++) {
                    for (int r2 = r1 + 1; r2 < table->row_count; r2++) {
                        bool match = true;
                        bool any_null = false;
                        for (int k = 0; k < table->uq_col_counts[u]; k++) {
                            int c_idx = table->uq_cols[u][k];
                            if (table->rows[r1].values[c_idx].is_null || table->rows[r2].values[c_idx].is_null) {
                                any_null = true;
                                break;
                            }
                            if (compare_typed_values(table->columns[c_idx].type,
                                                     &table->rows[r1].values[c_idx],
                                                     &table->rows[r2].values[c_idx]) != 0) {
                                match = false;
                                break;
                            }
                        }
                        if (match && !any_null) {
                            return report_database_error("composite UNIQUE constraint violation in %s", table->name);
                        }
                    }
                }
            }
        }
    }

    if (db->index_count < 0 || db->index_count > MAX_SECONDARY_INDEXES)
        return report_database_error("invalid secondary index count");
    for (int i = 0; i < db->index_count; i++) {
        const SecondaryIndex* idx = &db->indexes[i];
        if (idx->is_unique) {
            int t = find_table_index(db, idx->table_name);
            if (t >= 0) {
                const Table* table = &db->tables[t];
                int c_idx = idx->col_idx;
                if (c_idx >= 0 && c_idx < table->col_count && table->row_count > 1) {
                    for (int r1 = 0; r1 < table->row_count; r1++) {
                        const Value* v1 = &table->rows[r1].values[c_idx];
                        if (v1->is_null) continue;
                        for (int r2 = r1 + 1; r2 < table->row_count; r2++) {
                            const Value* v2 = &table->rows[r2].values[c_idx];
                            if (v2->is_null) continue;
                            if (compare_typed_values(table->columns[c_idx].type, v1, v2) == 0) {
                                return report_database_error("unique index violation on %s(%s)", table->name, table->columns[c_idx].name);
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}
