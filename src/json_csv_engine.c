#include "database_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void normalize_table_name(const char* in, char* out, size_t cap) {
    size_t i = 0;
    while (in[i] && i + 1 < cap) {
        out[i] = uppercase_ascii(in[i]);
        i++;
    }
    out[i] = '\0';
}

static void csv_write_field(FILE* fp, const char* text, bool is_null) {
    if (is_null) return;
    bool needs_quotes = false;
    for (const char* p = text; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        fputs(text, fp);
        return;
    }
    fputc('"', fp);
    for (const char* p = text; *p; p++) {
        if (*p == '"') fputc('"', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
}

bool export_table_csv(const Database* db, const char* table_name, const char* filepath) {
    if (!db || !table_name || !filepath) return false;
    char uname[MAX_NAME_LEN];
    normalize_table_name(table_name, uname, sizeof(uname));
    int tidx = find_table_index(db, uname);
    if (tidx < 0) return report_database_error("table does not exist: %s", table_name);
    const Table* t = &db->tables[tidx];

    FILE* fp = fopen(filepath, "w");
    if (!fp) return report_database_error("cannot open file for writing: %s", filepath);

    for (int c = 0; c < t->col_count; c++) {
        if (c > 0) fputc(',', fp);
        csv_write_field(fp, t->columns[c].name, false);
    }
    fputc('\n', fp);

    for (int r = 0; r < t->row_count; r++) {
        for (int c = 0; c < t->col_count; c++) {
            if (c > 0) fputc(',', fp);
            csv_write_field(fp, t->rows[r].values[c].text, t->rows[r].values[c].is_null);
        }
        fputc('\n', fp);
    }

    fclose(fp);
    return true;
}

static char* csv_next_field(char** cursor, bool* in_quotes, bool* eol, bool* valid) {
    char* s = *cursor;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *eol = true; return NULL; }
    if (*s == '\r') { if (*(s + 1) == '\n') s++; *eol = true; *cursor = s + 1; return NULL; }
    if (*s == '\n') { *eol = true; *cursor = s + 1; return NULL; }

    char* start = s;
    char* write_ptr = s;
    bool quoted = false;
    if (*s == '"') {
        quoted = true;
        s++;
        start = s;
        write_ptr = s;
        bool closed = false;
        while (*s) {
            if (*s == '"') {
                if (*(s + 1) == '"') {
                    *write_ptr++ = '"';
                    s += 2;
                } else {
                    s++;
                    closed = true;
                    break;
                }
            } else {
                *write_ptr++ = *s++;
            }
        }
        if (!closed) *valid = false;
        while (*s == ' ' || *s == '\t') s++;
        if (*s && *s != ',' && *s != '\n' && *s != '\r') *valid = false;
        while (*s && *s != ',' && *s != '\n' && *s != '\r') s++;
    } else {
        while (*s && *s != ',' && *s != '\n' && *s != '\r') {
            if (*s == '"') *valid = false;
            write_ptr++;
            s++;
        }
    }
    char delimiter = *s;
    *write_ptr = '\0';
    if (delimiter == ',') {
        *cursor = s + 1;
        *eol = false;
    } else if (delimiter == '\r') {
        if (*(s + 1) == '\n') s++;
        *cursor = s + 1;
        *eol = true;
    } else if (delimiter == '\n') {
        *cursor = s + 1;
        *eol = true;
    } else {
        *cursor = s;
        *eol = (delimiter == '\0');
    }
    if (in_quotes) *in_quotes = quoted;
    return start;
}

static bool csv_parse_record(char* line, char fields[MAX_COLUMNS][MAX_VAL_LEN],
                             bool quoted_fields[MAX_COLUMNS], int* field_count) {
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
        line[--length] = '\0';
    *field_count = 0;
    if (!length) return true;

    char* cursor = line;
    bool eol = false;
    bool valid = true;
    do {
        if (*field_count >= MAX_COLUMNS) return false;
        bool quoted = false;
        char* field = csv_next_field(&cursor, &quoted, &eol, &valid);
        size_t field_length = field ? strlen(field) : 0;
        if (field_length >= MAX_VAL_LEN) return false;
        snprintf(fields[*field_count], MAX_VAL_LEN, "%s", field ? field : "");
        quoted_fields[*field_count] = quoted;
        (*field_count)++;
    } while (!eol && valid);
    return valid;
}

static void csv_column_identifier(const char* header, int ordinal, char output[MAX_NAME_LEN]) {
    size_t used = 0;
    for (const unsigned char* p = (const unsigned char*)header;
         *p && used + 1 < MAX_NAME_LEN; p++) {
        char c = (char)*p;
        if (is_ascii_digit(c) || is_identifier_start(c) || c == '$' || c == '#') {
            output[used++] = uppercase_ascii(c);
        } else if (used > 0 && output[used - 1] != '_') {
            output[used++] = '_';
        }
    }
    while (used > 0 && output[used - 1] == '_') used--;
    if (used == 0 || !is_identifier_start(output[0])) {
        snprintf(output, MAX_NAME_LEN, "COLUMN_%d", ordinal);
    } else {
        output[used] = '\0';
        if (!is_valid_identifier(output))
            snprintf(output, MAX_NAME_LEN, "COLUMN_%d", ordinal);
    }
}

static void csv_unique_column_identifier(const char* header, int ordinal,
                                         char previous[MAX_COLUMNS][MAX_NAME_LEN],
                                         int previous_count, char output[MAX_NAME_LEN]) {
    csv_column_identifier(header, ordinal, output);
    bool duplicate = false;
    for (int i = 0; i < previous_count; i++)
        if (!strcmp(previous[i], output)) duplicate = true;
    if (!duplicate) return;
    snprintf(output, MAX_NAME_LEN, "COLUMN_%d", ordinal);
    for (int suffix = 2; suffix <= MAX_COLUMNS; suffix++) {
        duplicate = false;
        for (int i = 0; i < previous_count; i++)
            if (!strcmp(previous[i], output)) duplicate = true;
        if (!duplicate) return;
        snprintf(output, MAX_NAME_LEN, "COLUMN_%d_%d", ordinal, suffix);
    }
}

bool import_table_csv(Database* db, const char* table_name, const char* filepath, bool has_header) {
    if (!db || !table_name || !filepath) return false;
    char uname[MAX_NAME_LEN];
    normalize_table_name(table_name, uname, sizeof(uname));
    int tidx = find_table_index(db, uname);
    if (tidx < 0) {
        FILE* hfp = fopen(filepath, "r");
        if (!hfp) return report_database_error("cannot open file for reading: %s", filepath);
        char hline[MAX_QUERY_LEN];
        if (!fgets(hline, sizeof(hline), hfp)) {
            fclose(hfp);
            return report_database_error("CSV file is empty: %s", filepath);
        }
        fclose(hfp);
        char (*hfields)[MAX_VAL_LEN] = calloc(MAX_COLUMNS, sizeof(*hfields));
        if (!hfields) return report_database_error("out of memory");
        bool hquoted[MAX_COLUMNS] = {false};
        int hcount = 0;
        if (!csv_parse_record(hline, hfields, hquoted, &hcount) || hcount < 1) {
            free(hfields);
            return report_database_error("cannot parse CSV header in %s", filepath);
        }
        if (db->table_count >= MAX_TABLES) {
            free(hfields);
            return report_database_error("table limit reached");
        }
        char col_names[MAX_COLUMNS][MAX_NAME_LEN] = {{0}};
        Table* nt = &db->tables[db->table_count];
        memset(nt, 0, sizeof(*nt));
        memcpy(nt->name, uname, sizeof(nt->name));
        nt->col_count = hcount;
        for (int c = 0; c < hcount; c++) {
            csv_unique_column_identifier(hfields[c], c + 1, col_names, c, nt->columns[c].name);
            memcpy(col_names[c], nt->columns[c].name, MAX_NAME_LEN);
            nt->columns[c].type = TYPE_VARCHAR2;
            nt->columns[c].size = 255;
            nt->columns[c].nullable = true;
        }
        free(hfields);
        if (!allocate_table_storage(nt, 64)) return report_database_error("out of memory");
        tidx = db->table_count++;
        has_header = true;
    }
    Table* t = &db->tables[tidx];
    if (!ensure_table_writable(t)) return report_database_error("table is not writable");

    FILE* fp = fopen(filepath, "r");
    if (!fp) return report_database_error("cannot open file for reading: %s", filepath);

    char line[MAX_QUERY_LEN];
    bool header_pending = has_header;
    int column_map[MAX_COLUMNS];
    for (int c = 0; c < t->col_count; c++) column_map[c] = c;
    int imported = 0;
    int original_row_count = t->row_count;
    int source_record = 0;
    bool ok = true;

    while (fgets(line, sizeof(line), fp)) {
        source_record++;
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n' && !feof(fp)) {
            report_database_error("CSV record %d exceeds the supported line length", source_record);
            ok = false;
            break;
        }
        char (*fields)[MAX_VAL_LEN] = calloc(MAX_COLUMNS, sizeof(*fields));
        if (!fields) { report_database_error("out of memory"); ok = false; break; }
        bool quoted_fields[MAX_COLUMNS] = {false};
        int field_count = 0;
        if (!csv_parse_record(line, fields, quoted_fields, &field_count)) {
            free(fields);
            report_database_error("CSV record %d is malformed or has too many/long fields", source_record);
            ok = false;
            break;
        }
        if (field_count == 0) { free(fields); continue; }
        if (header_pending) {
            header_pending = false;
            if (field_count != t->col_count) {
                free(fields);
                report_database_error("CSV header has %d columns but table %s has %d; check the header or table schema",
                                      field_count, t->name, t->col_count);
                ok = false;
                break;
            }
            bool mapped[MAX_COLUMNS] = {false};
            char header_names[MAX_COLUMNS][MAX_NAME_LEN] = {{0}};
            for (int source_col = 0; source_col < field_count; source_col++) {
                char candidate[MAX_NAME_LEN] = {0};
                csv_unique_column_identifier(fields[source_col], source_col + 1,
                                             header_names, source_col, candidate);
                snprintf(header_names[source_col], sizeof(header_names[source_col]), "%s", candidate);
                int table_col = find_column_index(t, candidate);
                if (table_col < 0 && is_valid_identifier(fields[source_col])) {
                    for (size_t i = 0; fields[source_col][i]; i++)
                        candidate[i] = uppercase_ascii(fields[source_col][i]);
                    table_col = find_column_index(t, candidate);
                }
                if (table_col < 0 || mapped[table_col]) {
                    report_database_error("CSV header column '%s' does not uniquely match a column in table %s; use DESCRIBE %s to check names",
                                          fields[source_col], t->name, t->name);
                    ok = false;
                    break;
                }
                mapped[table_col] = true;
                column_map[source_col] = table_col;
            }
            free(fields);
            if (!ok) break;
            continue;
        }
        if (field_count != t->col_count) {
            free(fields);
            report_database_error("CSV record %d has %d fields; table %s expects %d. Check delimiters and quotes",
                                  source_record, field_count, t->name, t->col_count);
            ok = false;
            break;
        }
        if (!reserve_insert_capacity(t)) {
            free(fields);
            ok = false;
            break;
        }
        Row* row = &t->rows[t->row_count];
        memset(row, 0, sizeof(*row));
        for (int source_col = 0; source_col < field_count; source_col++) {
            int table_col = column_map[source_col];
            const char* value = fields[source_col];
            Value* cell = &row->values[table_col];
            cell->is_null = (!value[0] && !quoted_fields[source_col]) ||
                            (!quoted_fields[source_col] && !strcmp(value, "NULL"));
            if (!cell->is_null) {
                if (t->columns[table_col].type == TYPE_NUMBER) {
                    if (!normalize_decimal(value, cell->text)) {
                        report_database_error("CSV record %d column %s contains invalid number '%s'",
                                              source_record, t->columns[table_col].name, value);
                        ok = false;
                        break;
                    }
                } else {
                    snprintf(cell->text, sizeof(cell->text), "%s", value);
                }
            }
            if (!is_valid_column_value(&t->columns[table_col], cell)) {
                report_database_error("CSV record %d value does not fit column %s; check its type and length",
                                      source_record, t->columns[table_col].name);
                ok = false;
                break;
            }
        }
        free(fields);
        if (!ok) break;
        t->row_count++;
        imported++;
    }

    if (ferror(fp)) {
        report_database_error("failed while reading CSV file: %s", filepath);
        ok = false;
    }
    if (fclose(fp) != 0 && ok) {
        report_database_error("failed to close CSV file after reading: %s", filepath);
        ok = false;
    }
    if (ok && imported > 0) ok = validate_relational_integrity(db);
    if (!ok) {
        t->row_count = original_row_count;
        rebuild_secondary_indexes(db);
        return false;
    }
    if (imported > 0) {
        rebuild_secondary_indexes(db);
        g_pending = true;
    }
    printf("%d row%s imported from CSV%s.\n", imported, imported == 1 ? "" : "s",
           has_header ? " (matched by header name)" : "");
    return true;
}

typedef enum {
    CSV_WIZARD_INPUT_ERROR = -1,
    CSV_WIZARD_INPUT_CANCEL = 0,
    CSV_WIZARD_INPUT_OK = 1
} CsvWizardInputResult;

static CsvWizardInputResult csv_wizard_readline(const char* prompt, char* output, size_t capacity) {
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(output, (int)capacity, stdin))
        return ferror(stdin) ? CSV_WIZARD_INPUT_ERROR : CSV_WIZARD_INPUT_CANCEL;
    size_t length = strlen(output);
    bool complete = length > 0 && output[length - 1] == '\n';
    while (length > 0 && (output[length - 1] == '\r' || output[length - 1] == '\n'))
        output[--length] = '\0';
    if (!complete && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        report_database_error("wizard response exceeds the supported length");
        return CSV_WIZARD_INPUT_ERROR;
    }
    return CSV_WIZARD_INPUT_OK;
}

static void csv_wizard_print_value(const char* value) {
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        if (*p < 32 || *p == 127) fputc('?', stdout);
        else fputc((int)*p, stdout);
    }
}

bool run_csv_import_wizard(Database* db) {
    if (!g_interactive_shell)
        return report_database_error("IMPORT WIZARD requires an interactive terminal; use IMPORT TABLE for scripts and automation");
    if (!db) return report_database_error("connect to a database before starting the import wizard");
    if (g_pending) return report_database_error("commit or roll back pending changes before starting the import wizard");

    char filepath[PATH_CAP];
    CsvWizardInputResult input = csv_wizard_readline(
        "CSV file path (blank to cancel): ", filepath, sizeof(filepath));
    if (input != CSV_WIZARD_INPUT_OK) {
        if (input == CSV_WIZARD_INPUT_ERROR) return false;
        puts("Import cancelled.");
        return true;
    }
    if (!filepath[0]) {
        puts("Import cancelled.");
        return true;
    }
    FILE* fp = fopen(filepath, "r");
    if (!fp) return report_database_error("cannot open CSV file '%s'; check the path and file permissions", filepath);

    char line[MAX_QUERY_LEN];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return report_database_error("CSV file is empty or could not be read");
    }
    char (*headers)[MAX_VAL_LEN] = calloc(MAX_COLUMNS, sizeof(*headers));
    if (!headers) { fclose(fp); return report_database_error("out of memory"); }
    bool header_quoted[MAX_COLUMNS] = {false};
    int column_count = 0;
    if (!csv_parse_record(line, headers, header_quoted, &column_count) ||
        column_count < 1 || column_count > MAX_COLUMNS) {
        free(headers);
        fclose(fp);
        return report_database_error("CSV header is invalid; expected between 1 and %d columns", MAX_COLUMNS);
    }

    char column_names[MAX_COLUMNS][MAX_NAME_LEN] = {{0}};
    char column_types[MAX_COLUMNS][32] = {{0}};
    char (*preview)[MAX_COLUMNS][MAX_VAL_LEN] = calloc(5, sizeof(*preview));
    if (!preview) { free(headers); fclose(fp); return report_database_error("out of memory"); }
    int preview_count = 0;
    int max_lengths[MAX_COLUMNS] = {0};
    bool has_sample[MAX_COLUMNS] = {false};
    bool all_numeric[MAX_COLUMNS];
    bool all_dates[MAX_COLUMNS];
    for (int c = 0; c < column_count; c++) {
        all_numeric[c] = true;
        all_dates[c] = true;
        csv_unique_column_identifier(headers[c], c + 1, column_names, c, column_names[c]);
    }

    while (preview_count < 5 && fgets(line, sizeof(line), fp)) {
        char (*values)[MAX_VAL_LEN] = calloc(MAX_COLUMNS, sizeof(*values));
        if (!values) { free(preview); free(headers); fclose(fp); return report_database_error("out of memory"); }
        bool value_quoted[MAX_COLUMNS] = {false};
        int value_count = 0;
        if (!csv_parse_record(line, values, value_quoted, &value_count)) {
            free(values); free(preview); free(headers); fclose(fp);
            return report_database_error("CSV preview row is malformed or has too many/long fields");
        }
        if (value_count == 0) { free(values); continue; }
        if (value_count != column_count) {
            free(values); free(preview); free(headers); fclose(fp);
            return report_database_error("CSV preview row has %d values, but the header has %d columns",
                                         value_count, column_count);
        }
        for (int c = 0; c < column_count; c++) {
            snprintf(preview[preview_count][c], MAX_VAL_LEN, "%s", values[c]);
            size_t length = strlen(values[c]);
            if (length > 255) {
                free(values); free(preview); free(headers); fclose(fp);
                return report_database_error("CSV preview value exceeds the 255-character column limit");
            }
            if ((int)length > max_lengths[c]) max_lengths[c] = (int)length;
            if (!values[c][0] && !value_quoted[c]) continue;
            has_sample[c] = true;
            char normalized[MAX_VAL_LEN];
            if (!normalize_decimal(values[c], normalized)) all_numeric[c] = false;
            if (!is_valid_datetime(values[c])) all_dates[c] = false;
        }
        free(values);
        preview_count++;
    }
    bool read_error = ferror(fp) != 0;
    if (fclose(fp) != 0) read_error = true;
    if (read_error) { free(preview); free(headers); return report_database_error("failed while reading CSV preview"); }

    puts("CSV preview (up to five rows):");
    for (int c = 0; c < column_count; c++) {
        if (c) fputs(" | ", stdout);
        csv_wizard_print_value(headers[c]);
    }
    fputc('\n', stdout);
    for (int r = 0; r < preview_count; r++) {
        for (int c = 0; c < column_count; c++) {
            if (c) fputs(" | ", stdout);
            csv_wizard_print_value(preview[r][c]);
        }
        fputc('\n', stdout);
    }
    for (int c = 0; c < column_count; c++) {
        if (has_sample[c] && all_numeric[c]) {
            snprintf(column_types[c], sizeof(column_types[c]), "NUMBER");
        } else if (has_sample[c] && all_dates[c]) {
            snprintf(column_types[c], sizeof(column_types[c]), "DATE");
        } else {
            snprintf(column_types[c], sizeof(column_types[c]), "VARCHAR2(255)");
        }
    }

    puts("Suggested columns (types inferred from the preview only):");
    for (int c = 0; c < column_count; c++)
        printf("  %s %s  (from header '%s')\n", column_names[c], column_types[c], headers[c]);
    puts("Press Enter to accept a type, or enter NUMBER, DATE, VARCHAR2, or VARCHAR2(1..255) to override.");
    char response[128];
    for (int c = 0; c < column_count; c++) {
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "%s [%s]: ", column_names[c], column_types[c]);
        input = csv_wizard_readline(prompt, response, sizeof(response));
        if (input != CSV_WIZARD_INPUT_OK) {
            free(preview); free(headers);
            if (input == CSV_WIZARD_INPUT_ERROR) return false;
            puts("Import cancelled.");
            return true;
        }
        if (!response[0]) continue;
        for (char* p = response; *p; p++) *p = uppercase_ascii(*p);
        if (!strcmp(response, "NUMBER") || !strcmp(response, "DATE")) {
            snprintf(column_types[c], sizeof(column_types[c]), "%s", response);
        } else if (!strncmp(response, "VARCHAR2", 8) &&
                   (response[8] == '\0' || response[8] == '(')) {
            int size = 255;
            if (response[8] == '(') {
                char* end = NULL;
                long parsed = strtol(response + 9, &end, 10);
                if (!end || *end != ')' || end[1] != '\0' || parsed < 1 || parsed > 255) {
                    free(preview); free(headers);
                    return report_database_error("VARCHAR2 override must be VARCHAR2 or VARCHAR2(1..255)");
                }
                size = (int)parsed;
            }
            snprintf(column_types[c], sizeof(column_types[c]), "VARCHAR2(%d)", size);
        } else {
            free(preview); free(headers);
            return report_database_error("type override must be NUMBER, DATE, VARCHAR2, or VARCHAR2(1..255)");
        }
    }

    char table_name[MAX_NAME_LEN];
    input = csv_wizard_readline("New table name (blank to cancel): ", table_name, sizeof(table_name));
    if (input != CSV_WIZARD_INPUT_OK) {
        free(preview); free(headers);
        if (input == CSV_WIZARD_INPUT_ERROR) return false;
        puts("Import cancelled.");
        return true;
    }
    if (!table_name[0]) {
        free(preview); free(headers);
        puts("Import cancelled.");
        return true;
    }
    for (char* p = table_name; *p; p++) *p = uppercase_ascii(*p);
    if (!is_valid_identifier(table_name) || !strcmp(table_name, "DUAL")) {
        free(preview); free(headers);
        return report_database_error("table name must be a valid identifier");
    }
    if (find_table_index(db, table_name) >= 0) {
        free(preview); free(headers);
        return report_database_error("table %s already exists; choose a different name", table_name);
    }

    char sql[MAX_QUERY_LEN];
    int used = snprintf(sql, sizeof(sql), "CREATE TABLE %s (", table_name);
    if (used < 0 || (size_t)used >= sizeof(sql)) {
        free(preview); free(headers);
        return report_database_error("generated table schema exceeds the supported length");
    }
    for (int c = 0; c < column_count; c++) {
        int written = snprintf(sql + used, sizeof(sql) - (size_t)used, "%s%s %s",
                               c ? ", " : "", column_names[c], column_types[c]);
        if (written < 0 || (size_t)written >= sizeof(sql) - (size_t)used) {
            free(preview); free(headers);
            return report_database_error("generated table schema exceeds the supported length");
        }
        used += written;
    }
    int written = snprintf(sql + used, sizeof(sql) - (size_t)used, ");");
    if (written < 0 || (size_t)written >= sizeof(sql) - (size_t)used) {
        free(preview); free(headers);
        return report_database_error("generated table schema exceeds the supported length");
    }

    printf("Ready to create %s and import the CSV file.\n", table_name);
    input = csv_wizard_readline("Continue? [y/N]: ", response, sizeof(response));
    free(preview);
    free(headers);
    if (input != CSV_WIZARD_INPUT_OK) {
        if (input == CSV_WIZARD_INPUT_ERROR) return false;
        puts("Import cancelled.");
        return true;
    }
    if (strcmp(response, "y") && strcmp(response, "Y") &&
        strcmp(response, "yes") && strcmp(response, "YES")) {
        puts("Import cancelled; no changes made.");
        return true;
    }

    bool previous_autocommit = g_autocommit;
    g_autocommit = false;
    bool quit = false;
    bool ok = execute_console_statement(sql, &quit);
    if (ok) ok = import_table_csv(g_db, table_name, filepath, true);
    if (ok) ok = commit_transaction();
    if (!ok && g_pending && !g_io_uncertain) {
        if (!rollback_transaction()) {
            g_autocommit = previous_autocommit;
            return report_database_error("CSV import failed and automatic rollback also failed");
        }
        puts("Import changes were rolled back; the database was not changed.");
    }
    g_autocommit = previous_autocommit;
    return ok;
}

static void json_write_escaped_string(FILE* fp, const char* text) {
    fputc('"', fp);
    for (const char* p = text; *p; p++) {
        if (*p == '"') fputs("\\\"", fp);
        else if (*p == '\\') fputs("\\\\", fp);
        else if (*p == '\n') fputs("\\n", fp);
        else if (*p == '\r') fputs("\\r", fp);
        else if (*p == '\t') fputs("\\t", fp);
        else fputc(*p, fp);
    }
    fputc('"', fp);
}

bool export_table_json(const Database* db, const char* table_name, const char* filepath) {
    if (!db || !table_name || !filepath) return false;
    char uname[MAX_NAME_LEN];
    normalize_table_name(table_name, uname, sizeof(uname));
    int tidx = find_table_index(db, uname);
    if (tidx < 0) return report_database_error("table does not exist: %s", table_name);
    const Table* t = &db->tables[tidx];

    FILE* fp = fopen(filepath, "w");
    if (!fp) return report_database_error("cannot open file for writing: %s", filepath);

    fputs("[\n", fp);
    for (int r = 0; r < t->row_count; r++) {
        fputs("  {", fp);
        for (int c = 0; c < t->col_count; c++) {
            if (c > 0) fputs(", ", fp);
            json_write_escaped_string(fp, t->columns[c].name);
            fputs(": ", fp);
            if (t->rows[r].values[c].is_null) {
                fputs("null", fp);
            } else if (t->columns[c].type == TYPE_NUMBER) {
                fputs(t->rows[r].values[c].text, fp);
            } else {
                json_write_escaped_string(fp, t->rows[r].values[c].text);
            }
        }
        fputs(r + 1 < t->row_count ? "},\n" : "}\n", fp);
    }
    fputs("]\n", fp);

    fclose(fp);
    return true;
}

bool extract_json_value(const char* json, const char* path, char* out_val, size_t out_cap) {
    if (!json || !path || !out_val || out_cap == 0) return false;
    out_val[0] = '\0';
    const char* key = path;
    if (key[0] == '$' && key[1] == '.') key += 2;

    size_t klen = strlen(key);
    const char* p = json;
    while (*p) {
        if (*p == '"') {
            p++;
            if (!strncmp(p, key, klen) && p[klen] == '"') {
                p += klen + 1;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == ':') {
                    p++;
                    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                    if (*p == '"') {
                        p++;
                        size_t out_idx = 0;
                        while (*p && *p != '"' && out_idx + 1 < out_cap) {
                            if (*p == '\\' && *(p + 1)) {
                                p++;
                                if (*p == 'n') out_val[out_idx++] = '\n';
                                else if (*p == 'r') out_val[out_idx++] = '\r';
                                else if (*p == 't') out_val[out_idx++] = '\t';
                                else out_val[out_idx++] = *p;
                            } else {
                                out_val[out_idx++] = *p;
                            }
                            p++;
                        }
                        out_val[out_idx] = '\0';
                        return true;
                    } else if (!strncmp(p, "null", 4)) {
                        out_val[0] = '\0';
                        return true;
                    } else {
                        size_t out_idx = 0;
                        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && out_idx + 1 < out_cap) {
                            out_val[out_idx++] = *p++;
                        }
                        out_val[out_idx] = '\0';
                        return true;
                    }
                }
            }
        }
        if (*p) p++;
    }
    return false;
}

bool import_table_json(Database* db, const char* table_name, const char* filepath) {
    if (!db || !table_name || !filepath) return false;
    char uname[MAX_NAME_LEN];
    normalize_table_name(table_name, uname, sizeof(uname));
    int tidx = find_table_index(db, uname);
    if (tidx < 0) return report_database_error("table does not exist: %s", table_name);
    Table* t = &db->tables[tidx];
    if (!ensure_table_writable(t)) return report_database_error("table is not writable");

    FILE* fp = fopen(filepath, "rb");
    if (!fp) return report_database_error("cannot open file for reading: %s", filepath);

#define STREAM_CHUNK_SIZE 65536
    char* buf = malloc(STREAM_CHUNK_SIZE * 2);
    if (!buf) { fclose(fp); return report_database_error("memory allocation failed for stream buffer"); }

    size_t in_buf = 0;
    int imported = 0;
    bool eof_reached = false;

    while (!eof_reached || in_buf > 0) {
        if (!eof_reached && in_buf < STREAM_CHUNK_SIZE) {
            size_t n = fread(buf + in_buf, 1, STREAM_CHUNK_SIZE, fp);
            if (n < STREAM_CHUNK_SIZE) eof_reached = true;
            in_buf += n;
            buf[in_buf] = '\0';
        }
        if (in_buf == 0) break;

        char* ptr = buf;
        while (*ptr && *ptr != '{') ptr++;
        if (!*ptr) {
            in_buf = 0;
            continue;
        }

        char* obj_start = ptr;
        ptr++;
        bool in_q = false;
        bool obj_complete = false;
        int brace_depth = 1;
        while (*ptr) {
            if (*ptr == '"') {
                /* A quote is escaped only when preceded by an odd run of backslashes. */
                size_t backslashes = 0;
                for (char* q = ptr - 1; q >= buf && *q == '\\'; q--) backslashes++;
                if (backslashes % 2 == 0) in_q = !in_q;
            } else if (!in_q) {
                if (*ptr == '{') {
                    brace_depth++;
                } else if (*ptr == '}') {
                    brace_depth--;
                    if (brace_depth == 0) {
                        ptr++;
                        obj_complete = true;
                        break;
                    }
                }
            }
            ptr++;
        }

        if (!obj_complete) {
            if (eof_reached) break;
            size_t remaining = (size_t)(buf + in_buf - obj_start);
            if (remaining >= STREAM_CHUNK_SIZE) {
                free(buf);
                fclose(fp);
                return report_database_error("JSON object exceeds streaming buffer (%d bytes)", STREAM_CHUNK_SIZE);
            }
            memmove(buf, obj_start, remaining);
            in_buf = remaining;
            buf[in_buf] = '\0';
            continue;
        }

        char orig_end = *ptr;
        *ptr = '\0';

        if (!reserve_insert_capacity(t)) {
            free(buf);
            fclose(fp);
            return report_database_error("table capacity exceeded during JSON import");
        }
        Row* row = &t->rows[t->row_count];
        for (int c = 0; c < t->col_count; c++) row->values[c].is_null = true;

        const char* p = obj_start + 1;
        while (*p && *p != '}') {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
            if (!*p || *p == '}') break;
            if (*p == '"') {
                p++;
                char kbuf[MAX_NAME_LEN];
                size_t kidx = 0;
                while (*p && *p != '"') {
                    if (kidx + 1 < sizeof(kbuf)) kbuf[kidx++] = *p;
                    p++;
                }
                kbuf[kidx] = '\0';
                if (*p == '"') p++;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == ':') p++;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

                char vbuf[MAX_VAL_LEN];
                size_t vidx = 0;
                bool is_null = false;
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"') {
                        if (*p == '\\' && *(p + 1)) {
                            p++;
                            char ch = *p;
                            if (ch == 'n') ch = '\n';
                            else if (ch == 'r') ch = '\r';
                            else if (ch == 't') ch = '\t';
                            if (vidx + 1 < sizeof(vbuf)) vbuf[vidx++] = ch;
                        } else {
                            if (vidx + 1 < sizeof(vbuf)) vbuf[vidx++] = *p;
                        }
                        p++;
                    }
                    vbuf[vidx] = '\0';
                    if (*p == '"') p++;
                } else if (!strncmp(p, "null", 4)) {
                    is_null = true;
                    p += 4;
                } else if (*p == '{' || *p == '[') {
                    char open_ch = *p;
                    char close_ch = (open_ch == '{') ? '}' : ']';
                    int depth = 0;
                    bool in_str = false;
                    while (*p) {
                        if (*p == '"' && (p == buf || *(p - 1) != '\\')) {
                            in_str = !in_str;
                        } else if (!in_str) {
                            if (*p == open_ch) depth++;
                            else if (*p == close_ch) {
                                depth--;
                                if (vidx + 1 < sizeof(vbuf)) vbuf[vidx++] = *p;
                                p++;
                                if (depth == 0) break;
                                continue;
                            }
                        }
                        if (vidx + 1 < sizeof(vbuf)) vbuf[vidx++] = *p;
                        p++;
                    }
                    vbuf[vidx] = '\0';
                } else {
                    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                        if (vidx + 1 < sizeof(vbuf)) vbuf[vidx++] = *p;
                        p++;
                    }
                    vbuf[vidx] = '\0';
                }

                char ukbuf[MAX_NAME_LEN];
                normalize_table_name(kbuf, ukbuf, sizeof(ukbuf));
                int cidx = find_column_index(t, ukbuf);
                if (cidx >= 0) {
                    row->values[cidx].is_null = is_null;
                    if (!is_null) {
                        if (t->columns[cidx].type == TYPE_NUMBER) normalize_decimal(vbuf, row->values[cidx].text);
                        else snprintf(row->values[cidx].text, sizeof(row->values[cidx].text), "%s", vbuf);
                    }
                }
            } else {
                p++;
            }
        }
        *ptr = orig_end;
        t->row_count++;
        imported++;

        size_t consumed = (size_t)(ptr - buf);
        size_t remaining = in_buf > consumed ? in_buf - consumed : 0;
        memmove(buf, ptr, remaining);
        in_buf = remaining;
        buf[in_buf] = '\0';
    }

    free(buf);
    fclose(fp);
    if (imported > 0) {
        if (!validate_relational_integrity(db)) return false;
        rebuild_secondary_indexes(db);
        g_pending = true;
    }
    printf("%d row%s imported from streaming JSON.\n", imported, imported == 1 ? "" : "s");
    return true;
}

static bool has_file_ext_ci(const char* path, const char* ext) {
    if (!path || !ext) return false;
    size_t plen = strlen(path), elen = strlen(ext);
    if (plen < elen) return false;
    const char* tail = path + (plen - elen);
    for (size_t i = 0; i < elen; i++) {
        if (uppercase_ascii(tail[i]) != uppercase_ascii(ext[i])) return false;
    }
    return true;
}

static void sql_write_escaped_literal(FILE* fp, const ColumnDef* col, const Value* val) {
    if (val->is_null) {
        fputs("NULL", fp);
        return;
    }
    if (col->type == TYPE_NUMBER) {
        fputs(val->text, fp);
        return;
    }
    fputc('\'', fp);
    for (const char* s = val->text; *s; s++) {
        if (*s == '\'') fputc('\'', fp);
        fputc(*s, fp);
    }
    fputc('\'', fp);
}

static void write_table_ddl_and_dml(FILE* fp, const Table* t, int dialect) {
    /* dialect: 0 = Standard/PCOD, 1 = MSSQL, 2 = Oracle */
    fprintf(fp, "CREATE TABLE %s (\n", t->name);
    for (int c = 0; c < t->col_count; c++) {
        const ColumnDef* col = &t->columns[c];
        fputs("  ", fp);
        if (dialect == 1) fprintf(fp, "[%s] ", col->name);
        else fprintf(fp, "%s ", col->name);

        if (col->type == TYPE_NUMBER) {
            if (dialect == 1) fputs("DECIMAL(38, 10)", fp);
            else fputs("NUMBER", fp);
        } else if (col->type == TYPE_DATE) {
            if (dialect == 1) fputs("DATETIME", fp);
            else fputs("DATE", fp);
        } else if (col->type == TYPE_VECTOR) {
            fprintf(fp, "VECTOR(%d)", col->size);
        } else {
            if (dialect == 1) fprintf(fp, "NVARCHAR(%d)", col->size);
            else if (dialect == 2) fprintf(fp, "VARCHAR2(%d BYTE)", col->size);
            else fprintf(fp, "VARCHAR2(%d)", col->size);
        }

        if (col->primary && t->pk_col_count <= 1) {
            fputs(" PRIMARY KEY", fp);
        } else {
            if (!col->nullable) fputs(" NOT NULL", fp);
            if (col->unique) fputs(" UNIQUE", fp);
        }
        if (col->ref_table[0]) {
            fprintf(fp, " REFERENCES %s(%s)", col->ref_table, col->ref_column);
            if (col->fk_on_delete == FK_ACTION_CASCADE) fputs(" ON DELETE CASCADE", fp);
            else if (col->fk_on_delete == FK_ACTION_SET_NULL) fputs(" ON DELETE SET NULL", fp);
            if (col->fk_on_update == FK_ACTION_CASCADE) fputs(" ON UPDATE CASCADE", fp);
            else if (col->fk_on_update == FK_ACTION_SET_NULL) fputs(" ON UPDATE SET NULL", fp);
        }
        if (c + 1 < t->col_count || t->pk_col_count > 1) fputs(",\n", fp);
        else fputs("\n", fp);
    }
    if (t->pk_col_count > 1) {
        fputs("  PRIMARY KEY (", fp);
        for (int k = 0; k < t->pk_col_count; k++) {
            if (k > 0) fputs(", ", fp);
            fputs(t->columns[t->pk_cols[k]].name, fp);
        }
        fputs(")\n", fp);
    }
    fputs(");\n", fp);
    if (dialect == 1) fputs("GO\n", fp);

    for (int r = 0; r < t->row_count; r++) {
        fprintf(fp, "INSERT INTO %s VALUES (", t->name);
        for (int c = 0; c < t->col_count; c++) {
            if (c > 0) fputs(", ", fp);
            sql_write_escaped_literal(fp, &t->columns[c], &t->rows[r].values[c]);
        }
        fputs(");\n", fp);
    }
    if (dialect == 1 && t->row_count > 0) fputs("GO\n", fp);
}

bool dump_database_sql(const Database* db, const char* filepath) {
    if (!db) return false;
    FILE* fp = stdout;
    if (filepath && filepath[0]) {
        fp = fopen(filepath, "w");
        if (!fp) return report_database_error("cannot open dump file for writing: %s", filepath);
    }
    fprintf(fp, "-- PC/OpenData SQL Server (%s) Database Dump\n", SERVER_VERSION);
    for (int t = 0; t < db->table_count; t++) {
        write_table_ddl_and_dml(fp, &db->tables[t], 0);
    }
    for (int i = 0; i < db->index_count; i++) {
        const SecondaryIndex* idx = &db->indexes[i];
        int tidx = find_table_index(db, idx->table_name);
        if (tidx >= 0 && idx->col_idx >= 0 && idx->col_idx < db->tables[tidx].col_count) {
            fprintf(fp, "CREATE %sINDEX %s ON %s(%s);\n",
                    idx->is_unique ? "UNIQUE " : "",
                    idx->name, idx->table_name,
                    db->tables[tidx].columns[idx->col_idx].name);
        }
    }
    fprintf(fp, "COMMIT;\n");
    if (fp != stdout) {
        fclose(fp);
        printf("Database dumped to '%s' (%d table%s).\n",
               filepath, db->table_count, db->table_count == 1 ? "" : "s");
    }
    return true;
}

static bool write_binary_db_snapshot_with_magic(const Database* db, const char* table_name,
                                                const char* filepath, const char* magic) {
    Database* sub_db = NULL;
    const Database* src_db = db;
    if (table_name && table_name[0]) {
        char uname[MAX_NAME_LEN];
        normalize_table_name(table_name, uname, sizeof(uname));
        int tidx = find_table_index(db, uname);
        if (tidx < 0) return report_database_error("table does not exist: %s", table_name);
        sub_db = calloc(1, sizeof(*sub_db));
        if (!sub_db) return report_database_error("out of memory");
        sub_db->table_count = 1;
        sub_db->tables[0] = db->tables[tidx];
        src_db = sub_db;
    }
    Image img = {0};
    uint64_t id = g_identity ? g_identity : 1ULL;
    uint64_t gen = g_generation ? g_generation : 1ULL;
    if (!encode_database_snapshot(src_db, id, gen, &img)) {
        free(sub_db);
        free(img.data);
        return report_database_error("failed to encode database snapshot");
    }
    free(sub_db);
    if (img.size >= 32 && magic && strlen(magic) == 8) {
        memcpy(img.data, magic, 8);
        uint32_t crc = UINT32_MAX;
        for (size_t i = 0; i < img.size - 4; i++) {
            crc ^= img.data[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        crc = ~crc;
        for (unsigned i = 0; i < 4; i++) {
            img.data[img.size - 4 + i] = (unsigned char)(crc >> (i * 8));
        }
    }
    FILE* fp = fopen(filepath, "wb");
    if (!fp) {
        free(img.data);
        return report_database_error("cannot open file for writing: %s", filepath);
    }
    bool ok = (fwrite(img.data, 1, img.size, fp) == img.size);
    fclose(fp);
    free(img.data);
    if (!ok) return report_database_error("failed writing snapshot file: %s", filepath);
    return true;
}

bool export_database_mssql(const Database* db, const char* table_name, const char* filepath) {
    if (!db || !filepath) return false;
    if (has_file_ext_ci(filepath, ".mdf") || has_file_ext_ci(filepath, ".ndf") ||
        has_file_ext_ci(filepath, ".ldf") || has_file_ext_ci(filepath, ".bak")) {
        return write_binary_db_snapshot_with_magic(db, table_name, filepath, "MSQLMDF1");
    }
    FILE* fp = fopen(filepath, "w");
    if (!fp) return report_database_error("cannot open MSSQL file for writing: %s", filepath);
    fputs("-- Microsoft SQL Server (T-SQL) Export from PC/OpenData SQL Server\n", fp);
    if (table_name && table_name[0]) {
        char uname[MAX_NAME_LEN];
        normalize_table_name(table_name, uname, sizeof(uname));
        int tidx = find_table_index(db, uname);
        if (tidx < 0) { fclose(fp); return report_database_error("table does not exist: %s", table_name); }
        write_table_ddl_and_dml(fp, &db->tables[tidx], 1);
    } else {
        for (int t = 0; t < db->table_count; t++) {
            write_table_ddl_and_dml(fp, &db->tables[t], 1);
        }
    }
    fclose(fp);
    return true;
}

bool export_database_oracle(const Database* db, const char* table_name, const char* filepath) {
    if (!db || !filepath) return false;
    if (has_file_ext_ci(filepath, ".dbf") || has_file_ext_ci(filepath, ".ora") ||
        has_file_ext_ci(filepath, ".dmp") || has_file_ext_ci(filepath, ".ctl")) {
        return write_binary_db_snapshot_with_magic(db, table_name, filepath, "ORCLDBF1");
    }
    FILE* fp = fopen(filepath, "w");
    if (!fp) return report_database_error("cannot open Oracle file for writing: %s", filepath);
    fputs("-- Oracle Database Export from PC/OpenData SQL Server\n", fp);
    if (table_name && table_name[0]) {
        char uname[MAX_NAME_LEN];
        normalize_table_name(table_name, uname, sizeof(uname));
        int tidx = find_table_index(db, uname);
        if (tidx < 0) { fclose(fp); return report_database_error("table does not exist: %s", table_name); }
        write_table_ddl_and_dml(fp, &db->tables[tidx], 2);
    } else {
        for (int t = 0; t < db->table_count; t++) {
            write_table_ddl_and_dml(fp, &db->tables[t], 2);
        }
    }
    fputs("COMMIT;\n", fp);
    fclose(fp);
    return true;
}

static bool import_database_external_file(Database* db, const char* table_name, const char* filepath, const char* label) {
    if (!db || !filepath) return false;
    FILE* probe = fopen(filepath, "rb");
    if (!probe) return report_database_error("cannot open %s file for reading: %s", label, filepath);
    unsigned char hdr[8] = {0};
    size_t nread = fread(hdr, 1, 8, probe);
    fclose(probe);

    if (nread == 8 && (memcmp(hdr, "MSQLMDF1", 8) == 0 ||
                       memcmp(hdr, "ORCLDBF1", 8) == 0 ||
                       memcmp(hdr, "PCOD0001", 8) == 0)) {
        Image img = {0};
        if (!read_snapshot_file(filepath, &img)) {
            free(img.data);
            return report_database_error("cannot read binary %s file: %s", label, filepath);
        }
        uint64_t id = 0, gen = 0;
        Database* imported_db = decode_database_snapshot(&img, &id, &gen);
        free(img.data);
        if (!imported_db) return report_database_error("corrupt or invalid %s file: %s", label, filepath);

        int rows_imported = 0;
        for (int st = 0; st < imported_db->table_count; st++) {
            const Table* src_t = &imported_db->tables[st];
            char target_name[MAX_NAME_LEN];
            if (table_name && table_name[0] && imported_db->table_count == 1) {
                normalize_table_name(table_name, target_name, sizeof(target_name));
            } else if (table_name && table_name[0]) {
                char ufilter[MAX_NAME_LEN];
                normalize_table_name(table_name, ufilter, sizeof(ufilter));
                if (strcmp(src_t->name, ufilter) != 0) continue;
                snprintf(target_name, sizeof(target_name), "%s", src_t->name);
            } else {
                snprintf(target_name, sizeof(target_name), "%s", src_t->name);
            }

            int dst_idx = find_table_index(db, target_name);
            if (dst_idx < 0) {
                if (db->table_count >= MAX_TABLES) {
                    destroy_database(imported_db);
                    return report_database_error("table limit reached during %s import", label);
                }
                dst_idx = db->table_count++;
                Table* dst_t = &db->tables[dst_idx];
                memset(dst_t, 0, sizeof(*dst_t));
                snprintf(dst_t->name, sizeof(dst_t->name), "%s", target_name);
                dst_t->col_count = src_t->col_count;
                for (int c = 0; c < src_t->col_count; c++) dst_t->columns[c] = src_t->columns[c];
                dst_t->pk_col_count = src_t->pk_col_count;
                for (int k = 0; k < src_t->pk_col_count; k++) dst_t->pk_cols[k] = src_t->pk_cols[k];
            }
            Table* dst_t = &db->tables[dst_idx];
            if (!ensure_table_writable(dst_t)) {
                destroy_database(imported_db);
                return report_database_error("cannot allocate storage during %s import", label);
            }
            for (int r = 0; r < src_t->row_count; r++) {
                if (!reserve_insert_capacity(dst_t)) {
                    destroy_database(imported_db);
                    return false;
                }
                Row* new_row = &dst_t->rows[dst_t->row_count++];
                memset(new_row, 0, sizeof(*new_row));
                for (int c = 0; c < dst_t->col_count; c++) {
                    if (c < src_t->col_count) new_row->values[c] = src_t->rows[r].values[c];
                    else new_row->values[c].is_null = true;
                }
                rows_imported++;
            }
        }
        destroy_database(imported_db);
        if (!validate_relational_integrity(db)) return false;
        rebuild_secondary_indexes(db);
        g_pending = true;
        printf("%d row%s imported from %s file.\n", rows_imported, rows_imported == 1 ? "" : "s", label);
        return true;
    }

    FILE* fp = fopen(filepath, "r");
    if (!fp) return report_database_error("cannot open %s script for reading: %s", label, filepath);
    char stmt_buf[MAX_QUERY_LEN];
    size_t stmt_len = 0;
    char line[4096];
    int executed = 0;
    while (fgets(line, sizeof(line), fp)) {
        const char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (trimmed[0] == '-' && trimmed[1] == '-') continue;
        if ((uppercase_ascii(trimmed[0]) == 'G' && uppercase_ascii(trimmed[1]) == 'O' &&
             (trimmed[2] == '\r' || trimmed[2] == '\n' || trimmed[2] == '\0' || trimmed[2] == ' ')) ||
            (trimmed[0] == '/' && (trimmed[1] == '\r' || trimmed[1] == '\n' || trimmed[1] == '\0'))) {
            if (stmt_len > 0) {
                if (stmt_len + 2 < sizeof(stmt_buf)) {
                    stmt_buf[stmt_len++] = ';';
                    stmt_buf[stmt_len] = '\0';
                }
            } else {
                continue;
            }
        } else {
            size_t llen = strlen(line);
            if (stmt_len + llen + 1 >= sizeof(stmt_buf)) {
                fclose(fp);
                return report_database_error("statement too long in %s script", label);
            }
            memcpy(stmt_buf + stmt_len, line, llen);
            stmt_len += llen;
            stmt_buf[stmt_len] = '\0';
        }

        char* semi = strchr(stmt_buf, ';');
        while (semi) {
            *semi = '\0';
            const char* s = stmt_buf;
            while (is_sql_whitespace(*s)) s++;
            if (*s) {
                Parser sp = { .cursor = s };
                scan_next_token(&sp);
                if (consume_keyword(&sp, "CREATE")) {
                    if (!execute_create_table(&sp, db)) { fclose(fp); return false; }
                    executed++;
                } else if (consume_keyword(&sp, "INSERT")) {
                    int ins = 0;
                    if (!execute_insert_row(&sp, db, &ins)) { fclose(fp); return false; }
                    executed += ins;
                } else if (consume_keyword(&sp, "ALTER")) {
                    if (!execute_alter_table(&sp, db)) { fclose(fp); return false; }
                    executed++;
                }
            }
            size_t rem = strlen(semi + 1);
            memmove(stmt_buf, semi + 1, rem + 1);
            stmt_len = rem;
            semi = strchr(stmt_buf, ';');
        }
    }
    fclose(fp);
    if (!validate_relational_integrity(db)) return false;
    rebuild_secondary_indexes(db);
    g_pending = true;
    printf("%d statement%s/row%s imported from %s script.\n",
           executed, executed == 1 ? "" : "s", executed == 1 ? "" : "s", label);
    return true;
}

bool import_database_mssql(Database* db, const char* table_name, const char* filepath) {
    return import_database_external_file(db, table_name, filepath, "MSSQL");
}

bool import_database_oracle(Database* db, const char* table_name, const char* filepath) {
    return import_database_external_file(db, table_name, filepath, "ORACLE");
}

bool diff_database_schema(const Database* db, const char* other_db_path) {
    if (!db || !other_db_path || !other_db_path[0]) return false;
    Image img = {0};
    if (!read_snapshot_file(other_db_path, &img)) {
        free(img.data);
        return report_database_error("cannot read comparison database file: %s", other_db_path);
    }
    uint64_t id = 0, gen = 0;
    Database* other = decode_database_snapshot(&img, &id, &gen);
    free(img.data);
    if (!other) return report_database_error("invalid or corrupt comparison database file: %s", other_db_path);

    printf("=== SCHEMA DIFF: ACTIVE vs '%s' ===\n", other_db_path);
    int diffs = 0;
    for (int t = 0; t < db->table_count; t++) {
        const Table* a = &db->tables[t];
        int ot = find_table_index(other, a->name);
        if (ot < 0) {
            printf("+ TABLE %s (only in ACTIVE, %d columns)\n", a->name, a->col_count);
            diffs++;
            continue;
        }
        const Table* b = &other->tables[ot];
        for (int c = 0; c < a->col_count; c++) {
            const ColumnDef* ca = &a->columns[c];
            int oc = find_column_index(b, ca->name);
            if (oc < 0) {
                printf("+ COLUMN %s.%s (only in ACTIVE)\n", a->name, ca->name);
                diffs++;
            } else {
                const ColumnDef* cb = &b->columns[oc];
                if (ca->type != cb->type || ca->size != cb->size ||
                    ca->nullable != cb->nullable || ca->primary != cb->primary ||
                    ca->unique != cb->unique) {
                    printf("~ COLUMN %s.%s differs (ACTIVE type=%d size=%d null=%d vs TARGET type=%d size=%d null=%d)\n",
                           a->name, ca->name, (int)ca->type, ca->size, (int)ca->nullable,
                           (int)cb->type, cb->size, (int)cb->nullable);
                    diffs++;
                }
            }
        }
        for (int c = 0; c < b->col_count; c++) {
            if (find_column_index(a, b->columns[c].name) < 0) {
                printf("- COLUMN %s.%s (only in '%s')\n", a->name, b->columns[c].name, other_db_path);
                diffs++;
            }
        }
    }
    for (int t = 0; t < other->table_count; t++) {
        if (find_table_index(db, other->tables[t].name) < 0) {
            printf("- TABLE %s (only in '%s', %d columns)\n",
                   other->tables[t].name, other_db_path, other->tables[t].col_count);
            diffs++;
        }
    }
    if (diffs == 0) {
        puts("Schemas are identical (0 differences).");
    } else {
        printf("Total schema differences: %d\n", diffs);
    }
    destroy_database(other);
    return true;
}

bool execute_export_statement(Parser* p) {
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: EXPORT requires the ADMIN role");
    if (consume_keyword(p, "MSSQL") || consume_keyword(p, "ORACLE")) {
        /* Handled by checking token before consume; let's support both forms below */
    }
    char tbl[MAX_NAME_LEN] = {0};
    bool is_csv = false, is_json = false, is_mssql = false, is_oracle = false;

    if (consume_keyword(p, "TABLE")) {
        if (!parse_identifier(p, tbl)) return report_database_error("expected table name");
        if (!consume_keyword(p, "TO")) return report_database_error("expected TO after table name");
        is_csv = consume_keyword(p, "CSV");
        is_json = !is_csv && consume_keyword(p, "JSON");
        is_mssql = !is_csv && !is_json && consume_keyword(p, "MSSQL");
        is_oracle = !is_csv && !is_json && !is_mssql && consume_keyword(p, "ORACLE");
        if (!is_csv && !is_json && !is_mssql && !is_oracle)
            return report_database_error("expected CSV, JSON, MSSQL, or ORACLE format");
    } else if (token_is_keyword(p, "MSSQL") || token_is_keyword(p, "ORACLE")) {
        is_mssql = consume_keyword(p, "MSSQL");
        is_oracle = !is_mssql && consume_keyword(p, "ORACLE");
        if (consume_keyword(p, "FROM")) {
            if (!parse_identifier(p, tbl)) return report_database_error("expected table name after FROM");
        }
        if (!consume_keyword(p, "TO")) return report_database_error("expected TO in EXPORT");
    } else {
        return report_database_error("expected TABLE, MSSQL, or ORACLE after EXPORT");
    }

    if (p->kind != TK_STRING) return report_database_error("expected destination file path string literal");
    char filepath[PATH_CAP];
    snprintf(filepath, sizeof(filepath), "%s", p->text);
    scan_next_token(p);
    if (!consume_statement_end(p)) return false;

    bool ok = false;
    const char* fmt_name = "CSV";
    if (is_csv) { ok = export_table_csv(g_db, tbl, filepath); fmt_name = "CSV"; }
    else if (is_json) { ok = export_table_json(g_db, tbl, filepath); fmt_name = "JSON"; }
    else if (is_mssql) { ok = export_database_mssql(g_db, tbl[0] ? tbl : NULL, filepath); fmt_name = "MSSQL"; }
    else if (is_oracle) { ok = export_database_oracle(g_db, tbl[0] ? tbl : NULL, filepath); fmt_name = "ORACLE"; }

    if (ok) {
        if (tbl[0]) printf("Table '%s' exported to %s: %s\n", tbl, fmt_name, filepath);
        else printf("Database exported to %s: %s\n", fmt_name, filepath);
    }
    return ok;
}

bool execute_import_statement(Parser* p) {
    if (g_readonly) return report_database_error("cannot IMPORT in read-only mode");
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: IMPORT requires the ADMIN role");
    if (g_raft_role == RAFT_FOLLOWER) return report_database_error("follower nodes accept read-only queries");
    if (consume_keyword(p, "WIZARD")) {
        if (!consume_statement_end(p)) return false;
        return run_csv_import_wizard(g_db);
    }
    if (p->kind == TK_STRING || token_is_keyword(p, "FILE")) {
        if (token_is_keyword(p, "FILE")) scan_next_token(p);
        if (p->kind != TK_STRING) return report_database_error("expected source file path string literal");
        char filepath[PATH_CAP];
        snprintf(filepath, sizeof(filepath), "%s", p->text);
        scan_next_token(p);
        char tbl[MAX_NAME_LEN] = {0};
        if (consume_keyword(p, "INTO") || consume_keyword(p, "TABLE")) {
            if (!parse_identifier(p, tbl)) return report_database_error("expected table name after INTO");
        } else {
            const char* base = strrchr(filepath, '/');
            if (!base) base = strrchr(filepath, '\\');
            base = base ? base + 1 : filepath;
            size_t k = 0;
            while (base[k] && base[k] != '.' && k + 1 < sizeof(tbl)) {
                tbl[k] = uppercase_ascii(base[k]);
                k++;
            }
            tbl[k] = '\0';
        }
        if (!consume_statement_end(p)) return false;
        const char* dot = strrchr(filepath, '.');
        bool is_json_file = (dot && (!strcmp(dot, ".json") || !strcmp(dot, ".JSON")));
        if (is_json_file) return import_table_json(g_db, tbl, filepath);
        return import_table_csv(g_db, tbl, filepath, true);
    }
    char tbl[MAX_NAME_LEN] = {0};
    bool is_csv = false, is_json = false, is_mssql = false, is_oracle = false;

    if (consume_keyword(p, "TABLE")) {
        if (!parse_identifier(p, tbl)) return report_database_error("expected table name");
        if (!consume_keyword(p, "FROM")) return report_database_error("expected FROM after table name");
        is_csv = consume_keyword(p, "CSV");
        is_json = !is_csv && consume_keyword(p, "JSON");
        is_mssql = !is_csv && !is_json && consume_keyword(p, "MSSQL");
        is_oracle = !is_csv && !is_json && !is_mssql && consume_keyword(p, "ORACLE");
        if (!is_csv && !is_json && !is_mssql && !is_oracle)
            return report_database_error("expected CSV, JSON, MSSQL, or ORACLE format");
    } else if (token_is_keyword(p, "MSSQL") || token_is_keyword(p, "ORACLE")) {
        is_mssql = consume_keyword(p, "MSSQL");
        is_oracle = !is_mssql && consume_keyword(p, "ORACLE");
        if (consume_keyword(p, "INTO")) {
            if (!parse_identifier(p, tbl)) return report_database_error("expected table name after INTO");
        }
        if (!consume_keyword(p, "FROM")) return report_database_error("expected FROM in IMPORT");
    } else {
        return report_database_error("expected TABLE, MSSQL, or ORACLE after IMPORT");
    }

    if (p->kind != TK_STRING) return report_database_error("expected source file path string literal");
    char filepath[PATH_CAP];
    snprintf(filepath, sizeof(filepath), "%s", p->text);
    scan_next_token(p);

    bool has_header = false;
    if (is_csv && consume_keyword(p, "HEADER")) has_header = true;
    if (!consume_statement_end(p)) return false;

    if (is_csv) return import_table_csv(g_db, tbl, filepath, has_header);
    if (is_json) return import_table_json(g_db, tbl, filepath);
    if (is_mssql) return import_database_mssql(g_db, tbl[0] ? tbl : NULL, filepath);
    return import_database_oracle(g_db, tbl[0] ? tbl : NULL, filepath);
}
