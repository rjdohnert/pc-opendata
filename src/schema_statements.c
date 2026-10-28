#include "database_engine.h"

static bool parse_optional_precision_scale(Parser* p) {
    if (!consume_token(p, TK_LP)) return true;
    if (p->kind != TK_NUMBER && !(p->kind == TK_ID && strcmp(p->text, "MAX") == 0)) return false;
    scan_next_token(p);
    if (consume_token(p, TK_COMMA)) {
        if (p->kind != TK_NUMBER) return false;
        scan_next_token(p);
    }
    return consume_token(p, TK_RP);
}

static bool parse_fk_actions(Parser* p, ColumnDef* c) {
    c->fk_on_delete = FK_ACTION_RESTRICT;
    c->fk_on_update = FK_ACTION_RESTRICT;
    while (consume_keyword(p, "ON")) {
        if (consume_keyword(p, "DELETE")) {
            if (consume_keyword(p, "CASCADE")) {
                c->fk_on_delete = FK_ACTION_CASCADE;
            } else if (consume_keyword(p, "SET")) {
                if (!consume_keyword(p, "NULL")) return false;
                c->fk_on_delete = FK_ACTION_SET_NULL;
            } else if (consume_keyword(p, "RESTRICT")) {
                c->fk_on_delete = FK_ACTION_RESTRICT;
            } else if (consume_keyword(p, "NO")) {
                consume_keyword(p, "ACTION");
                c->fk_on_delete = FK_ACTION_RESTRICT;
            } else return false;
        } else if (consume_keyword(p, "UPDATE")) {
            if (consume_keyword(p, "CASCADE")) {
                c->fk_on_update = FK_ACTION_CASCADE;
            } else if (consume_keyword(p, "SET")) {
                if (!consume_keyword(p, "NULL")) return false;
                c->fk_on_update = FK_ACTION_SET_NULL;
            } else if (consume_keyword(p, "RESTRICT")) {
                c->fk_on_update = FK_ACTION_RESTRICT;
            } else if (consume_keyword(p, "NO")) {
                consume_keyword(p, "ACTION");
                c->fk_on_update = FK_ACTION_RESTRICT;
            } else return false;
        } else return false;
    }
    return true;
}

static bool parse_single_column_def(Parser* p, Table* table, ColumnDef* c, bool* primary) {
    memset(c, 0, sizeof(*c));
    c->nullable = true;
    if (!parse_identifier(p, c->name) || find_column_index(table, c->name) >= 0) return false;
    if (consume_keyword(p, "NUMBER") || consume_keyword(p, "INT") || consume_keyword(p, "INTEGER") ||
        consume_keyword(p, "BIGINT") || consume_keyword(p, "SMALLINT") || consume_keyword(p, "TINYINT") ||
        consume_keyword(p, "BIT") || consume_keyword(p, "FLOAT") || consume_keyword(p, "REAL") ||
        consume_keyword(p, "DECIMAL") || consume_keyword(p, "NUMERIC") || consume_keyword(p, "MONEY") ||
        consume_keyword(p, "BINARY_FLOAT") || consume_keyword(p, "BINARY_DOUBLE")) {
        c->type = TYPE_NUMBER;
        c->size = 40;
        if (!parse_optional_precision_scale(p)) return false;
    } else if (consume_keyword(p, "DATE") || consume_keyword(p, "DATETIME") ||
               consume_keyword(p, "DATETIME2") || consume_keyword(p, "SMALLDATETIME") ||
               consume_keyword(p, "TIMESTAMP")) {
        c->type = TYPE_DATE;
        c->size = 19;
        if (!parse_optional_precision_scale(p)) return false;
    } else if (consume_keyword(p, "VECTOR")) {
        if (!consume_token(p, TK_LP) || p->kind != TK_NUMBER || strspn(p->text, "0123456789") != strlen(p->text)) return false;
        errno = 0;
        long dim = strtol(p->text, NULL, 10);
        if (errno || dim < 1 || dim > 64) return false;
        c->type = TYPE_VECTOR;
        c->size = (int)dim;
        scan_next_token(p);
        if (!consume_token(p, TK_RP)) return false;
    } else if (consume_keyword(p, "VARCHAR2") || consume_keyword(p, "VARCHAR") ||
               consume_keyword(p, "NVARCHAR") || consume_keyword(p, "NVARCHAR2") ||
               consume_keyword(p, "RAW")) {
        if (!consume_token(p, TK_LP)) return false;
        long size = 255;
        if (consume_keyword(p, "MAX")) {
            size = 255;
        } else {
            if (p->kind != TK_NUMBER || strspn(p->text, "0123456789") != strlen(p->text)) return false;
            errno = 0;
            size = strtol(p->text, NULL, 10);
            if (errno || size < 1 || size >= MAX_VAL_LEN) return false;
            scan_next_token(p);
            if (!consume_keyword(p, "BYTE")) consume_keyword(p, "CHAR");
        }
        c->type = TYPE_VARCHAR2;
        c->size = (int)size;
        if (!consume_token(p, TK_RP)) return false;
    } else if (consume_keyword(p, "CHAR") || consume_keyword(p, "NCHAR")) {
        long size = 1;
        if (consume_token(p, TK_LP)) {
            if (p->kind != TK_NUMBER || strspn(p->text, "0123456789") != strlen(p->text)) return false;
            errno = 0;
            size = strtol(p->text, NULL, 10);
            if (errno || size < 1 || size >= MAX_VAL_LEN) return false;
            scan_next_token(p);
            if (!consume_keyword(p, "BYTE")) consume_keyword(p, "CHAR");
            if (!consume_token(p, TK_RP)) return false;
        }
        c->type = TYPE_VARCHAR2;
        c->size = (int)size;
    } else if (consume_keyword(p, "CLOB") || consume_keyword(p, "BLOB") ||
               consume_keyword(p, "TEXT") || consume_keyword(p, "NTEXT")) {
        c->type = TYPE_VARCHAR2;
        c->size = 255;
    } else return false;

    bool saw_not_null = false, saw_unique = false, saw_ref = false;
    for (;;) {
        if (consume_keyword(p, "CONSTRAINT")) {
            if (!parse_identifier(p, c->constraint_name)) return false;
        } else if (consume_keyword(p, "IDENTITY")) {
            if (!parse_optional_precision_scale(p)) return false;
        } else if (consume_keyword(p, "NOT")) {
            if (saw_not_null || !consume_keyword(p, "NULL")) return false;
            saw_not_null = true; c->nullable = false;
        } else if (consume_keyword(p, "NULL")) {
            if (saw_not_null || c->primary) return false;
            c->nullable = true;
        } else if (consume_keyword(p, "PRIMARY")) {
            if (*primary || saw_unique || !consume_keyword(p, "KEY")) return false;
            *primary = true; saw_unique = true; c->primary = c->unique = true; c->nullable = false;
        } else if (consume_keyword(p, "UNIQUE")) {
            if (saw_unique) return false;
            saw_unique = true; c->unique = true;
        } else if (consume_keyword(p, "REFERENCES")) {
            if (saw_ref || !parse_identifier(p, c->ref_table) || !consume_token(p, TK_LP) ||
                !parse_identifier(p, c->ref_column) || !consume_token(p, TK_RP)) return false;
            saw_ref = true;
            if (!parse_fk_actions(p, c)) return false;
        } else break;
    }
    return true;
}

bool execute_create_table(Parser* p, Database* db) {
    if (!consume_keyword(p, "TABLE")) return false;
    if (db->table_count == MAX_TABLES) return report_database_error("table limit reached");
    Table* table = &db->tables[db->table_count];
    memset(table, 0, sizeof(*table));
    if (!parse_identifier(p, table->name) || !strcmp(table->name, "DUAL") ||
        find_table_index(db, table->name) >= 0 || !consume_token(p, TK_LP)) return false;
    bool primary = false;
    do {
        char cname[MAX_NAME_LEN] = {0};
        if (consume_keyword(p, "CONSTRAINT")) {
            if (!parse_identifier(p, cname)) return false;
        }
        if (consume_keyword(p, "PRIMARY")) {
            if (!consume_keyword(p, "KEY") || !consume_token(p, TK_LP)) return false;
            if (primary) return report_database_error("multiple primary keys not allowed");
            primary = true;
            if (cname[0]) memcpy(table->pk_name, cname, sizeof(table->pk_name));
            do {
                char col_name[MAX_NAME_LEN];
                if (!parse_identifier(p, col_name)) return false;
                int col_idx = find_column_index(table, col_name);
                if (col_idx < 0) return report_database_error("column not found: %s", col_name);
                if (table->pk_col_count >= MAX_COLUMNS)
                    return report_database_error("primary key column limit (%d) reached", MAX_COLUMNS);
                for (int k = 0; k < table->pk_col_count; k++)
                    if (table->pk_cols[k] == col_idx)
                        return report_database_error("duplicate column in primary key: %s", col_name);
                table->columns[col_idx].nullable = false;
                table->pk_cols[table->pk_col_count++] = col_idx;
            } while (consume_token(p, TK_COMMA));
            if (!consume_token(p, TK_RP)) return false;
            if (table->pk_col_count == 1) {
                table->columns[table->pk_cols[0]].primary = true;
                table->columns[table->pk_cols[0]].unique = true;
                if (cname[0]) memcpy(table->columns[table->pk_cols[0]].constraint_name, cname, MAX_NAME_LEN);
            }
        } else if (consume_keyword(p, "UNIQUE")) {
            if (!consume_token(p, TK_LP)) return false;
            if (table->uq_count >= MAX_COLUMNS) return false;
            int uq_idx = table->uq_count++;
            if (cname[0]) memcpy(table->uq_names[uq_idx], cname, MAX_NAME_LEN);
            do {
                char col_name[MAX_NAME_LEN];
                if (!parse_identifier(p, col_name)) return false;
                int col_idx = find_column_index(table, col_name);
                if (col_idx < 0) return report_database_error("column not found: %s", col_name);
                if (table->uq_col_counts[uq_idx] >= MAX_COLUMNS)
                    return report_database_error("unique constraint column limit (%d) reached", MAX_COLUMNS);
                for (int k = 0; k < table->uq_col_counts[uq_idx]; k++)
                    if (table->uq_cols[uq_idx][k] == col_idx)
                        return report_database_error("duplicate column in unique constraint: %s", col_name);
                table->uq_cols[uq_idx][table->uq_col_counts[uq_idx]++] = col_idx;
            } while (consume_token(p, TK_COMMA));
            if (!consume_token(p, TK_RP)) return false;
            if (table->uq_col_counts[uq_idx] == 1) {
                table->columns[table->uq_cols[uq_idx][0]].unique = true;
                if (cname[0]) memcpy(table->columns[table->uq_cols[uq_idx][0]].constraint_name, cname, MAX_NAME_LEN);
            }
        } else if (consume_keyword(p, "FOREIGN")) {
            if (!consume_keyword(p, "KEY") || !consume_token(p, TK_LP)) return false;
            char col_name[MAX_NAME_LEN];
            if (!parse_identifier(p, col_name) || !consume_token(p, TK_RP)) return false;
            int col_idx = find_column_index(table, col_name);
            if (col_idx < 0) return report_database_error("column not found: %s", col_name);
            if (!consume_keyword(p, "REFERENCES")) return false;
            ColumnDef* c = &table->columns[col_idx];
            if (cname[0]) memcpy(c->constraint_name, cname, MAX_NAME_LEN);
            if (!parse_identifier(p, c->ref_table) || !consume_token(p, TK_LP) ||
                !parse_identifier(p, c->ref_column) || !consume_token(p, TK_RP)) return false;
            if (!parse_fk_actions(p, c)) return false;
        } else {
            if (table->col_count == MAX_COLUMNS) return false;
            ColumnDef* c = &table->columns[table->col_count];
            if (!parse_single_column_def(p, table, c, &primary)) return false;
            if (c->primary) {
                table->pk_cols[table->pk_col_count++] = table->col_count;
                if (c->constraint_name[0]) memcpy(table->pk_name, c->constraint_name, MAX_NAME_LEN);
            }
            table->col_count++;
        }
    } while (consume_token(p, TK_COMMA));
    if (!consume_token(p, TK_RP)) return false;
    table->engine_type = ENGINE_MEMORY;
    if (consume_keyword(p, "ENGINE")) {
        consume_token(p, TK_EQ);
        if (consume_keyword(p, "DISK")) {
            table->engine_type = ENGINE_DISK;
            table->btree_root_page = 0;
            table->first_data_page = 1;
        } else if (consume_keyword(p, "MEMORY")) {
            table->engine_type = ENGINE_MEMORY;
        } else {
            return report_database_error("unknown storage engine");
        }
    }
    if (!consume_statement_end(p)) return false;
    db->table_count++;
    return true;
}

bool execute_drop_table(Parser* p, Database* db) {
    char name[MAX_NAME_LEN];
    if (!consume_keyword(p, "TABLE") || !parse_identifier(p, name) || !consume_statement_end(p)) return false;
    int t = find_table_index(db, name);
    if (t < 0) return report_database_error("table does not exist: %s", name);
    if (g_network_user[0])
        return report_database_error("permission denied: DROP TABLE requires the ADMIN role");
    for (int i = 0; i < db->index_count; ) {
        if (strcmp(db->indexes[i].table_name, name) == 0) {
            if (db->indexes[i].entries) free(db->indexes[i].entries);
            for (int j = i; j + 1 < db->index_count; j++) {
                db->indexes[j] = db->indexes[j + 1];
            }
            db->index_count--;
            memset(&db->indexes[db->index_count], 0, sizeof(SecondaryIndex));
        } else {
            if (db->indexes[i].table_idx > t) {
                db->indexes[i].table_idx--;
            }
            i++;
        }
    }
    release_table_storage(&db->tables[t]);
    for (int i = t; i + 1 < db->table_count; i++) db->tables[i] = db->tables[i + 1];
    memset(&db->tables[--db->table_count], 0, sizeof(Table));
    /* Grants referencing the dropped table are no longer meaningful. */
    for (int i = 0; i < db->grant_count; ) {
        if (!strcmp(db->grants[i].table, name)) {
            memmove(&db->grants[i], &db->grants[i + 1],
                    (size_t)(db->grant_count - i - 1) * sizeof(TableGrant));
            db->grant_count--;
            memset(&db->grants[db->grant_count], 0, sizeof(TableGrant));
        } else i++;
    }
    return true;
}

bool execute_truncate_table(Parser* p, Database* db) {
    if (!consume_keyword(p, "TABLE")) return false;
    Table* table = resolve_statement_table(p, db);
    if (!table || !consume_statement_end(p)) return false;
    if (!user_has_table_privilege(db, g_network_user, table->name, PRIV_DELETE))
        return report_database_error("permission denied: TRUNCATE on %s", table->name);
    if (table->row_count > 0 && !ensure_table_writable(table))
        return report_database_error("unable to allocate row storage");
    table->row_count = 0;
    table->stats.is_analyzed = false;
    for (int i = 0; i < db->index_count; i++) {
        if (strcmp(db->indexes[i].table_name, table->name) == 0) {
            db->indexes[i].entry_count = 0;
        }
    }
    return true;
}

bool execute_alter_table(Parser* p, Database* db) {
    if (!consume_keyword(p, "TABLE")) return false;
    Table* table = resolve_statement_table(p, db);
    if (!table) return false;
    if (g_network_user[0])
        return report_database_error("permission denied: ALTER TABLE requires the ADMIN role");
    if (consume_keyword(p, "ADD")) {
        if (consume_keyword(p, "CONSTRAINT")) {
            char cname[MAX_NAME_LEN];
            if (!parse_identifier(p, cname)) return false;
            if (consume_keyword(p, "PRIMARY")) {
                if (!consume_keyword(p, "KEY") || !consume_token(p, TK_LP)) return false;
                if (table->pk_col_count > 0) return report_database_error("table already has a primary key");
                for (int c = 0; c < table->col_count; c++) {
                    if (table->columns[c].primary) return report_database_error("table already has a primary key");
                }
                int pk_cols[MAX_COLUMNS];
                int pk_count = 0;
                do {
                    char col_name[MAX_NAME_LEN];
                    if (pk_count >= MAX_COLUMNS || !parse_identifier(p, col_name)) return false;
                    int col_idx = find_column_index(table, col_name);
                    if (col_idx < 0) return report_database_error("column not found: %s", col_name);
                    pk_cols[pk_count++] = col_idx;
                } while (consume_token(p, TK_COMMA));
                if (!consume_token(p, TK_RP) || !consume_statement_end(p)) return false;
                for (int k = 0; k < pk_count; k++) {
                    table->columns[pk_cols[k]].nullable = false;
                    table->pk_cols[k] = pk_cols[k];
                }
                table->pk_col_count = pk_count;
                memcpy(table->pk_name, cname, MAX_NAME_LEN);
                if (pk_count == 1) {
                    table->columns[pk_cols[0]].primary = true;
                    table->columns[pk_cols[0]].unique = true;
                    memcpy(table->columns[pk_cols[0]].constraint_name, cname, MAX_NAME_LEN);
                }
                return true;
            } else if (consume_keyword(p, "UNIQUE")) {
                if (!consume_token(p, TK_LP)) return false;
                if (table->uq_count >= MAX_COLUMNS) return false;
                int uq_idx = table->uq_count;
                int uq_cols[MAX_COLUMNS];
                int uq_count = 0;
                do {
                    char col_name[MAX_NAME_LEN];
                    if (uq_count >= MAX_COLUMNS || !parse_identifier(p, col_name)) return false;
                    int col_idx = find_column_index(table, col_name);
                    if (col_idx < 0) return report_database_error("column not found: %s", col_name);
                    uq_cols[uq_count++] = col_idx;
                } while (consume_token(p, TK_COMMA));
                if (!consume_token(p, TK_RP) || !consume_statement_end(p)) return false;
                for (int k = 0; k < uq_count; k++) {
                    table->uq_cols[uq_idx][k] = uq_cols[k];
                }
                table->uq_col_counts[uq_idx] = uq_count;
                memcpy(table->uq_names[uq_idx], cname, MAX_NAME_LEN);
                table->uq_count++;
                if (uq_count == 1) {
                    table->columns[uq_cols[0]].unique = true;
                    memcpy(table->columns[uq_cols[0]].constraint_name, cname, MAX_NAME_LEN);
                }
                return true;
            } else if (consume_keyword(p, "FOREIGN")) {
                if (!consume_keyword(p, "KEY") || !consume_token(p, TK_LP)) return false;
                char col_name[MAX_NAME_LEN];
                if (!parse_identifier(p, col_name) || !consume_token(p, TK_RP)) return false;
                int col_idx = find_column_index(table, col_name);
                if (col_idx < 0) return report_database_error("column not found: %s", col_name);
                if (!consume_keyword(p, "REFERENCES")) return false;
                ColumnDef* c = &table->columns[col_idx];
                if (!parse_identifier(p, c->ref_table) || !consume_token(p, TK_LP) ||
                    !parse_identifier(p, c->ref_column) || !consume_token(p, TK_RP)) return false;
                if (!parse_fk_actions(p, c) || !consume_statement_end(p)) return false;
                memcpy(c->constraint_name, cname, MAX_NAME_LEN);
                return true;
            }
            return false;
        }
        consume_keyword(p, "COLUMN");
        if (table->col_count == MAX_COLUMNS) return report_database_error("column limit reached");
        bool primary = false;
        for (int c = 0; c < table->col_count; c++) if (table->columns[c].primary) primary = true;
        ColumnDef new_col;
        if (!parse_single_column_def(p, table, &new_col, &primary)) return false;
        if (!consume_statement_end(p)) return false;
        if (!new_col.nullable && table->row_count > 0)
            return report_database_error("cannot add NOT NULL column to table with existing rows");
        if (new_col.primary && table->row_count > 0)
            return report_database_error("cannot add PRIMARY KEY column to table with existing rows");
        if (table->row_count > 0 && !ensure_table_writable(table))
            return report_database_error("unable to allocate row storage");
        table->columns[table->col_count] = new_col;
        for (int r = 0; r < table->row_count; r++)
            table->rows[r].values[table->col_count].is_null = true;
        table->col_count++;
        return true;
    }
    if (consume_keyword(p, "DROP")) {
        if (consume_keyword(p, "CONSTRAINT")) {
            char cname[MAX_NAME_LEN];
            if (!parse_identifier(p, cname) || !consume_statement_end(p)) return false;
            bool found = false;
            if (strcmp(table->pk_name, cname) == 0) {
                for (int k = 0; k < table->pk_col_count; k++) {
                    int c_idx = table->pk_cols[k];
                    table->columns[c_idx].primary = false;
                    table->columns[c_idx].unique = false;
                    if (strcmp(table->columns[c_idx].constraint_name, cname) == 0) {
                        table->columns[c_idx].constraint_name[0] = '\0';
                    }
                }
                table->pk_col_count = 0;
                table->pk_name[0] = '\0';
                found = true;
            }
            for (int u = 0; u < table->uq_count; ) {
                if (strcmp(table->uq_names[u], cname) == 0) {
                    if (table->uq_col_counts[u] == 1) {
                        int c_idx = table->uq_cols[u][0];
                        if (!table->columns[c_idx].primary) table->columns[c_idx].unique = false;
                        if (strcmp(table->columns[c_idx].constraint_name, cname) == 0) {
                            table->columns[c_idx].constraint_name[0] = '\0';
                        }
                    }
                    for (int j = u; j + 1 < table->uq_count; j++) {
                        memcpy(table->uq_cols[j], table->uq_cols[j + 1], sizeof(table->uq_cols[j]));
                        table->uq_col_counts[j] = table->uq_col_counts[j + 1];
                        memcpy(table->uq_names[j], table->uq_names[j + 1], MAX_NAME_LEN);
                    }
                    table->uq_count--;
                    found = true;
                } else {
                    u++;
                }
            }
            for (int c = 0; c < table->col_count; c++) {
                ColumnDef* col = &table->columns[c];
                if (strcmp(col->constraint_name, cname) == 0) {
                    if (col->ref_table[0]) {
                        col->ref_table[0] = '\0';
                        col->ref_column[0] = '\0';
                        col->fk_on_delete = FK_ACTION_RESTRICT;
                        col->fk_on_update = FK_ACTION_RESTRICT;
                    }
                    if (col->primary) {
                        col->primary = false;
                        col->unique = false;
                        table->pk_col_count = 0;
                        table->pk_name[0] = '\0';
                    } else if (col->unique) {
                        col->unique = false;
                    }
                    col->constraint_name[0] = '\0';
                    found = true;
                }
            }
            if (!found) return report_database_error("constraint does not exist: %s", cname);
            return true;
        }
        consume_keyword(p, "COLUMN");
        char col_name[MAX_NAME_LEN];
        if (!parse_identifier(p, col_name) || !consume_statement_end(p)) return false;
        int col_idx = find_column_index(table, col_name);
        if (col_idx < 0) return report_database_error("column does not exist: %s", col_name);
        if (table->col_count <= 1) return report_database_error("cannot drop only column from table");
        for (int t = 0; t < db->table_count; t++) {
            Table* other = &db->tables[t];
            for (int c = 0; c < other->col_count; c++) {
                if (!strcmp(other->columns[c].ref_table, table->name) &&
                    !strcmp(other->columns[c].ref_column, col_name)) {
                    return report_database_error("cannot drop column referenced by foreign key: %s.%s", other->name, other->columns[c].name);
                }
            }
        }
        if (table->row_count > 0 && !ensure_table_writable(table))
            return report_database_error("unable to allocate row storage");
        for (int r = 0; r < table->row_count; r++) {
            for (int c = col_idx; c + 1 < table->col_count; c++)
                table->rows[r].values[c] = table->rows[r].values[c + 1];
            memset(&table->rows[r].values[table->col_count - 1], 0, sizeof(Value));
        }
        for (int c = col_idx; c + 1 < table->col_count; c++)
            table->columns[c] = table->columns[c + 1];
        memset(&table->columns[--table->col_count], 0, sizeof(ColumnDef));
        /* Shift constraint column references and drop constraints covering the removed column. */
        {
            int pk_write = 0;
            for (int k = 0; k < table->pk_col_count; k++) {
                if (table->pk_cols[k] == col_idx) continue;
                table->pk_cols[pk_write++] = table->pk_cols[k] > col_idx ? table->pk_cols[k] - 1 : table->pk_cols[k];
            }
            if (pk_write != table->pk_col_count) { table->pk_name[0] = '\0'; }
            table->pk_col_count = pk_write;
            for (int u = 0; u < table->uq_count; ) {
                int uq_write = 0;
                for (int k = 0; k < table->uq_col_counts[u]; k++) {
                    if (table->uq_cols[u][k] == col_idx) continue;
                    table->uq_cols[u][uq_write++] = table->uq_cols[u][k] > col_idx ? table->uq_cols[u][k] - 1 : table->uq_cols[u][k];
                }
                if (uq_write == 0) {
                    for (int j = u; j + 1 < table->uq_count; j++) {
                        memcpy(table->uq_cols[j], table->uq_cols[j + 1], sizeof(table->uq_cols[j]));
                        table->uq_col_counts[j] = table->uq_col_counts[j + 1];
                        memcpy(table->uq_names[j], table->uq_names[j + 1], MAX_NAME_LEN);
                    }
                    table->uq_count--;
                } else {
                    table->uq_col_counts[u] = uq_write;
                    u++;
                }
            }
        }
        /* Drop or shift secondary indexes referencing this table's columns. */
        for (int i = 0; i < db->index_count; ) {
            if (strcmp(db->indexes[i].table_name, table->name) == 0) {
                if (db->indexes[i].col_idx == col_idx) {
                    free(db->indexes[i].entries);
                    for (int j = i; j + 1 < db->index_count; j++)
                        db->indexes[j] = db->indexes[j + 1];
                    db->index_count--;
                    memset(&db->indexes[db->index_count], 0, sizeof(SecondaryIndex));
                    continue;
                }
                if (db->indexes[i].col_idx > col_idx) {
                    db->indexes[i].col_idx--;
                    build_secondary_index(&db->indexes[i], table, db->indexes[i].col_idx);
                }
            }
            i++;
        }
        return true;
    }
    if (consume_keyword(p, "RENAME")) {
        if (consume_keyword(p, "COLUMN")) {
            char old_name[MAX_NAME_LEN], new_name[MAX_NAME_LEN];
            if (!parse_identifier(p, old_name) || !consume_keyword(p, "TO") ||
                !parse_identifier(p, new_name) || !consume_statement_end(p)) return false;
            int col_idx = find_column_index(table, old_name);
            if (col_idx < 0) return report_database_error("column does not exist: %s", old_name);
            if (find_column_index(table, new_name) >= 0)
                return report_database_error("column already exists: %s", new_name);
            memcpy(table->columns[col_idx].name, new_name, sizeof(table->columns[col_idx].name));
            for (int t = 0; t < db->table_count; t++) {
                Table* other = &db->tables[t];
                for (int c = 0; c < other->col_count; c++) {
                    if (!strcmp(other->columns[c].ref_table, table->name) &&
                        !strcmp(other->columns[c].ref_column, old_name)) {
                        memcpy(other->columns[c].ref_column, new_name, sizeof(other->columns[c].ref_column));
                    }
                }
            }
            return true;
        }
        if (consume_keyword(p, "TO")) {
            char new_tbl_name[MAX_NAME_LEN];
            if (!parse_identifier(p, new_tbl_name) || !consume_statement_end(p)) return false;
            if (!strcmp(new_tbl_name, "DUAL") || find_table_index(db, new_tbl_name) >= 0)
                return report_database_error("table already exists or invalid: %s", new_tbl_name);
            char old_tbl_name[MAX_NAME_LEN];
            memcpy(old_tbl_name, table->name, sizeof(old_tbl_name));
            memcpy(table->name, new_tbl_name, sizeof(table->name));
            for (int t = 0; t < db->table_count; t++) {
                Table* other = &db->tables[t];
                for (int c = 0; c < other->col_count; c++) {
                    if (!strcmp(other->columns[c].ref_table, old_tbl_name)) {
                        memcpy(other->columns[c].ref_table, new_tbl_name, sizeof(other->columns[c].ref_table));
                    }
                }
            }
            for (int i = 0; i < db->index_count; i++) {
                if (!strcmp(db->indexes[i].table_name, old_tbl_name)) {
                    memcpy(db->indexes[i].table_name, new_tbl_name, sizeof(db->indexes[i].table_name));
                }
            }
            return true;
        }
    }
    return false;
}

Table* resolve_statement_table(Parser* p, Database* db) {
    char name[MAX_NAME_LEN];
    if (!parse_identifier(p, name)) return NULL;
    int t = find_table_index(db, name);
    if (t < 0) { report_database_error("table does not exist: %s", name); return NULL; }
    return &db->tables[t];
}
