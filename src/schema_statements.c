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
    if (!parse_identifier(p, c->name) || (table && find_column_index(table, c->name) >= 0)) return false;
    if (consume_keyword(p, "NUMBER") || consume_keyword(p, "INT") || consume_keyword(p, "INTEGER") ||
        consume_keyword(p, "BIGINT") || consume_keyword(p, "SMALLINT") || consume_keyword(p, "TINYINT") ||
        consume_keyword(p, "BIT") || consume_keyword(p, "FLOAT") || consume_keyword(p, "REAL") ||
        consume_keyword(p, "DECIMAL") || consume_keyword(p, "NUMERIC") || consume_keyword(p, "MONEY") ||
        consume_keyword(p, "BINARY_FLOAT") || consume_keyword(p, "BINARY_DOUBLE")) {
        c->type = TYPE_NUMBER;
        c->size = 40;
        if (!parse_optional_precision_scale(p)) return false;
    } else if (consume_keyword(p, "SERIAL") || consume_keyword(p, "BIGSERIAL")) {
        c->type = TYPE_NUMBER;
        c->size = 40;
        c->auto_increment = true;
    } else if (consume_keyword(p, "BOOLEAN") || consume_keyword(p, "BOOL")) {
        c->type = TYPE_NUMBER;
        c->size = 40;
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
               consume_keyword(p, "RAW") || consume_keyword(p, "STRING")) {
        long size = 255;
        if (consume_token(p, TK_LP)) {
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
            if (!consume_token(p, TK_RP)) return false;
        }
        c->type = TYPE_VARCHAR2;
        c->size = (int)size;
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
               consume_keyword(p, "TEXT") || consume_keyword(p, "NTEXT") ||
               consume_keyword(p, "XML")) {
        c->type = TYPE_VARCHAR2;
        c->size = 255;
    } else if (consume_keyword(p, "UNIQUEIDENTIFIER") || consume_keyword(p, "UUID") || consume_keyword(p, "GUID")) {
        c->type = TYPE_VARCHAR2;
        c->size = 36;
    } else if (consume_keyword(p, "ROWID") || consume_keyword(p, "UROWID")) {
        c->type = TYPE_VARCHAR2;
        c->size = 18;
    } else if (consume_keyword(p, "VARBINARY") || consume_keyword(p, "IMAGE") || consume_keyword(p, "BINARY")) {
        long size = 255;
        if (consume_token(p, TK_LP)) {
            if (consume_keyword(p, "MAX")) {
                size = 255;
            } else if (p->kind == TK_NUMBER) {
                errno = 0;
                size = strtol(p->text, NULL, 10);
                if (errno || size < 1 || size >= MAX_VAL_LEN) size = 255;
                scan_next_token(p);
            }
            if (!consume_token(p, TK_RP)) return false;
        }
        c->type = TYPE_VARCHAR2;
        c->size = (int)size;
    } else return false;

    bool saw_not_null = false, saw_unique = false, saw_ref = false;
    for (;;) {
        if (consume_keyword(p, "CONSTRAINT")) {
            if (!parse_identifier(p, c->constraint_name)) return false;
        } else if (consume_keyword(p, "IDENTITY") || consume_keyword(p, "AUTO_INCREMENT") || consume_keyword(p, "AUTOINCREMENT")) {
            c->auto_increment = true;
            if (token_is_keyword(p, "IDENTITY")) parse_optional_precision_scale(p);
            else if (consume_token(p, TK_LP)) {
                while (p->kind != TK_RP && p->kind != TK_END) scan_next_token(p);
                consume_token(p, TK_RP);
            }
        } else if (consume_keyword(p, "DEFAULT")) {
            c->has_default = true;
            if (consume_keyword(p, "NULL")) {
                c->default_value[0] = '\0';
            } else if (consume_keyword(p, "CURRENT_TIMESTAMP") || consume_keyword(p, "NOW") || consume_keyword(p, "SYSDATE")) {
                time_t now = time(NULL);
                struct tm tm_val;
#ifdef _WIN32
                gmtime_s(&tm_val, &now);
#else
                gmtime_r(&now, &tm_val);
#endif
                strftime(c->default_value, sizeof(c->default_value), "%Y-%m-%d %H:%M:%S", &tm_val);
            } else if (consume_keyword(p, "TRUE")) {
                strcpy(c->default_value, "1");
            } else if (consume_keyword(p, "FALSE")) {
                strcpy(c->default_value, "0");
            } else if (p->kind == TK_NUMBER || p->kind == TK_STRING) {
                snprintf(c->default_value, sizeof(c->default_value), "%s", p->text);
                scan_next_token(p);
            }
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
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: CREATE TABLE requires the ADMIN role");
    bool if_not_exists = false;
    if (consume_keyword(p, "IF")) {
        if (!consume_keyword(p, "NOT") || !consume_keyword(p, "EXISTS")) return false;
        if_not_exists = true;
    }
    if (db->table_count == MAX_TABLES) return report_database_error("table limit reached");
    char tbl_name[MAX_NAME_LEN];
    if (!parse_identifier(p, tbl_name) || !strcmp(tbl_name, "DUAL")) return false;
    if (consume_token(p, TK_DOT)) {
        char subname[MAX_NAME_LEN];
        if (!parse_identifier(p, subname) || !strcmp(subname, "DUAL")) return false;
        snprintf(tbl_name, sizeof(tbl_name), "%s", subname);
    }
    if (find_table_index(db, tbl_name) >= 0) {
        if (if_not_exists) {
            while (p->kind != TK_SEMI && p->kind != TK_END && !p->failed) {
                scan_next_token(p);
            }
            consume_token(p, TK_SEMI);
            return true;
        }
        return report_database_error("table already exists: %s", tbl_name);
    }

    if (consume_keyword(p, "AS")) {
        if (!consume_keyword(p, "SELECT")) return report_database_error("expected SELECT after AS in CREATE TABLE");
        QueryResultCapture cap = {0};
        cap.silent = true;
        QueryResultCapture* old_cap = g_result_capture;
        g_result_capture = &cap;
        bool sel_ok = execute_select_query(p);
        g_result_capture = old_cap;
        if (!sel_ok) {
            free(cap.rows);
            return false;
        }
        Table* table = &db->tables[db->table_count];
        memset(table, 0, sizeof(*table));
        memcpy(table->name, tbl_name, sizeof(table->name));
        table->col_count = cap.col_count;
        for (int c = 0; c < cap.col_count; c++) {
            memcpy(table->columns[c].name, cap.col_names[c], sizeof(table->columns[c].name));
            table->columns[c].type = TYPE_VARCHAR2;
            table->columns[c].size = 255;
            table->columns[c].nullable = true;
        }
        if (!allocate_table_storage(table, cap.row_count > 0 ? cap.row_count : 16)) {
            free(cap.rows);
            return report_database_error("failed to allocate table storage");
        }
        for (int r = 0; r < cap.row_count; r++) {
            for (int c = 0; c < cap.col_count; c++) {
                table->rows[r].values[c] = cap.rows[r][c];
            }
            table->row_count++;
        }
        free(cap.rows);
        db->table_count++;
        return true;
    }

    Table* table = &db->tables[db->table_count];
    memset(table, 0, sizeof(*table));
    memcpy(table->name, tbl_name, sizeof(table->name));
    if (!consume_token(p, TK_LP)) return false;
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
    table->part_type = PART_NONE;
    for (int opt_iter = 0; opt_iter < 2; opt_iter++) {
        if (consume_keyword(p, "ENGINE")) {
            consume_token(p, TK_EQ);
            if (consume_keyword(p, "DISK")) {
                table->engine_type = ENGINE_DISK;
                table->btree_root_page = 0;
                table->first_data_page = 1;
            } else if (consume_keyword(p, "COLUMNAR")) {
                table->engine_type = ENGINE_COLUMNAR;
            } else if (consume_keyword(p, "MEMORY")) {
                table->engine_type = ENGINE_MEMORY;
            } else {
                return report_database_error("unknown storage engine");
            }
        } else if (consume_keyword(p, "PARTITION")) {
            if (!consume_keyword(p, "BY")) return false;
            if (consume_keyword(p, "HASH")) {
                if (!consume_token(p, TK_LP)) return false;
                char pcol[MAX_NAME_LEN];
                if (!parse_identifier(p, pcol) || !consume_token(p, TK_RP)) return false;
                int pci = find_column_index(table, pcol);
                if (pci < 0) return report_database_error("partition column not found: %s", pcol);
                int pcnt = 4;
                if (consume_keyword(p, "PARTITIONS")) {
                    if (p->kind != TK_NUMBER) return false;
                    pcnt = (int)strtol(p->text, NULL, 10);
                    scan_next_token(p);
                }
                if (pcnt < 1 || pcnt > MAX_PARTITIONS) {
                    return report_database_error("partition count must be between 1 and %d", MAX_PARTITIONS);
                }
                table->part_type = PART_HASH;
                table->part_col_idx = pci;
                table->part_count = pcnt;
                for (int pi = 0; pi < pcnt; pi++) {
                    snprintf(table->partitions[pi].name, sizeof(table->partitions[pi].name), "P%d", pi);
                }
            } else if (consume_keyword(p, "RANGE")) {
                if (!consume_token(p, TK_LP)) return false;
                char pcol[MAX_NAME_LEN];
                if (!parse_identifier(p, pcol) || !consume_token(p, TK_RP)) return false;
                int pci = find_column_index(table, pcol);
                if (pci < 0) return report_database_error("partition column not found: %s", pcol);
                if (!consume_token(p, TK_LP)) return false;
                table->part_type = PART_RANGE;
                table->part_col_idx = pci;
                table->part_count = 0;
                do {
                    if (table->part_count >= MAX_PARTITIONS) {
                        return report_database_error("maximum range partitions (%d) exceeded", MAX_PARTITIONS);
                    }
                    if (!consume_keyword(p, "PARTITION")) return false;
                    PartitionDef* pd = &table->partitions[table->part_count];
                    memset(pd, 0, sizeof(*pd));
                    if (!parse_identifier(p, pd->name)) return false;
                    if (!consume_keyword(p, "VALUES") || !consume_keyword(p, "LESS") ||
                        !consume_keyword(p, "THAN") || !consume_token(p, TK_LP)) return false;
                    if (consume_keyword(p, "MAXVALUE")) {
                        pd->is_maxvalue = true;
                        snprintf(pd->upper_bound, sizeof(pd->upper_bound), "MAXVALUE");
                    } else {
                        Value bound_val = {0};
                        if (!parse_typed_literal(p, &table->columns[pci], &bound_val) || bound_val.is_null) {
                            return report_database_error("invalid range partition upper bound for %s", pd->name);
                        }
                        snprintf(pd->upper_bound, sizeof(pd->upper_bound), "%.63s", bound_val.text);
                    }
                    if (!consume_token(p, TK_RP)) return false;
                    table->part_count++;
                } while (consume_token(p, TK_COMMA));
                if (!consume_token(p, TK_RP)) return false;
            } else {
                return report_database_error("expected HASH or RANGE after PARTITION BY");
            }
        }
    }
    if (!consume_statement_end(p)) return false;
    db->table_count++;
    return true;
}

bool execute_drop_table(Parser* p, Database* db) {
    char name[MAX_NAME_LEN];
    if (!consume_keyword(p, "TABLE")) return false;
    bool if_exists = false;
    if (consume_keyword(p, "IF")) {
        if (!consume_keyword(p, "EXISTS")) return false;
        if_exists = true;
    }
    if (!parse_identifier(p, name) || !consume_statement_end(p)) return false;
    int t = find_table_index(db, name);
    if (t < 0) {
        if (if_exists) return true;
        return report_table_not_found(db, name);
    }
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: DROP TABLE requires the ADMIN role");
    for (int i = 0; i < db->index_count; ) {
        if (strcmp(db->indexes[i].table_name, name) == 0) {
            if (db->indexes[i].is_hnsw) hnsw_index_destroy(&db->indexes[i]);
            else free(db->indexes[i].entries);
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
            if (db->indexes[i].is_hnsw) {
                hnsw_index_build(&db->indexes[i], table, db->indexes[i].col_idx);
            } else {
                db->indexes[i].entry_count = 0;
            }
        }
    }
    return true;
}

bool execute_alter_table(Parser* p, Database* db) {
    if (!consume_keyword(p, "TABLE")) return false;
    Table* table = resolve_statement_table(p, db);
    if (!table) return false;
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
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
        bool if_not_exists = false;
        if (consume_keyword(p, "IF")) {
            if (!consume_keyword(p, "NOT") || !consume_keyword(p, "EXISTS")) return false;
            if_not_exists = true;
        }
        if (if_not_exists && p->kind == TK_ID && find_column_index(table, p->text) >= 0) {
            while (p->kind != TK_SEMI && p->kind != TK_END && !p->failed) scan_next_token(p);
            consume_token(p, TK_SEMI);
            return true;
        }
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
        bool if_exists = false;
        if (consume_keyword(p, "IF")) {
            if (!consume_keyword(p, "EXISTS")) return false;
            if_exists = true;
        }
        char col_name[MAX_NAME_LEN];
        if (!parse_identifier(p, col_name) || !consume_statement_end(p)) return false;
        int col_idx = find_column_index(table, col_name);
        if (col_idx < 0) {
            if (if_exists) return true;
            return report_column_not_found(table, col_name);
        }
        if (table->col_count <= 1) return report_database_error("cannot drop only column from table");
        if (table->part_type != PART_NONE && table->part_col_idx == col_idx)
            return report_database_error("cannot drop partition key column: %s", col_name);
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
        if (table->part_type != PART_NONE && table->part_col_idx > col_idx) {
            table->part_col_idx--;
        }
        /* Drop or shift secondary indexes referencing this table's columns. */
        for (int i = 0; i < db->index_count; ) {
            if (strcmp(db->indexes[i].table_name, table->name) == 0) {
                if (db->indexes[i].col_idx == col_idx) {
                    if (db->indexes[i].is_hnsw) hnsw_index_destroy(&db->indexes[i]);
                    else free(db->indexes[i].entries);
                    for (int j = i; j + 1 < db->index_count; j++)
                        db->indexes[j] = db->indexes[j + 1];
                    db->index_count--;
                    memset(&db->indexes[db->index_count], 0, sizeof(SecondaryIndex));
                    continue;
                }
                if (db->indexes[i].col_idx > col_idx) {
                    db->indexes[i].col_idx--;
                    if (db->indexes[i].is_hnsw)
                        hnsw_index_build(&db->indexes[i], table, db->indexes[i].col_idx);
                    else
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
            for (int i = 0; i < db->grant_count; i++) {
                if (!strcmp(db->grants[i].table, old_tbl_name)) {
                    memcpy(db->grants[i].table, new_tbl_name, sizeof(db->grants[i].table));
                }
            }
            return true;
        }
    }
    if (consume_keyword(p, "MODIFY")) {
        consume_keyword(p, "COLUMN");
        bool primary = false;
        ColumnDef dummy_col;
        if (!parse_single_column_def(p, NULL, &dummy_col, &primary)) return false;
        int col_idx = find_column_index(table, dummy_col.name);
        if (col_idx < 0) return report_column_not_found(table, dummy_col.name);
        if (!consume_statement_end(p)) return false;
        for (int r = 0; r < table->row_count; r++) {
            if (!dummy_col.nullable && table->rows[r].values[col_idx].is_null) {
                return report_database_error("column '%s' contains NULL values; cannot modify to NOT NULL", dummy_col.name);
            }
            if (!table->rows[r].values[col_idx].is_null &&
                !is_valid_column_value(&dummy_col, &table->rows[r].values[col_idx])) {
                return report_database_error("existing value in row %d exceeds new column specification", r + 1);
            }
        }
        table->columns[col_idx].type = dummy_col.type;
        table->columns[col_idx].size = dummy_col.size;
        table->columns[col_idx].nullable = dummy_col.nullable;
        if (dummy_col.has_default) {
            table->columns[col_idx].has_default = true;
            memcpy(table->columns[col_idx].default_value, dummy_col.default_value, sizeof(table->columns[col_idx].default_value));
        }
        return true;
    }
    return false;
}

Table* resolve_statement_table(Parser* p, Database* db) {
    char name[MAX_NAME_LEN];
    if (!parse_identifier(p, name)) return NULL;
    if (consume_token(p, TK_DOT)) {
        char subname[MAX_NAME_LEN];
        if (!parse_identifier(p, subname)) return NULL;
        int t = find_table_index(db, subname);
        if (t >= 0) return &db->tables[t];
        char fq[MAX_NAME_LEN];
        snprintf(fq, sizeof(fq), "%s.%s", name, subname);
        t = find_table_index(db, fq);
        if (t >= 0) return &db->tables[t];
        snprintf(name, sizeof(name), "%s", subname);
    }
    int t = find_table_index(db, name);
    if (t < 0) { report_table_not_found(db, name); return NULL; }
    return &db->tables[t];
}
