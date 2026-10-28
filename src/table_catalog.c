#include "database_engine.h"

void release_table_storage(Table* table) {
    if (table->storage) {
        if (atomic_fetch_sub(&table->storage->refcount, 1) == 1) {
            free(table->storage->rows);
            free(table->storage);
        }
        table->storage = NULL;
    }
    table->rows = NULL;
    table->capacity = 0;
}

bool allocate_table_storage(Table* table, int capacity) {
    release_table_storage(table);
    if (capacity <= 0) return true;
    RowStorage* s = malloc(sizeof(*s));
    if (!s) return false;
    s->rows = calloc((size_t)capacity, sizeof(Row));
    if (!s->rows) { free(s); return false; }
    atomic_init(&s->refcount, 1);
    s->capacity = capacity;
    table->storage = s;
    table->rows = s->rows;
    table->capacity = capacity;
    return true;
}

bool ensure_table_writable(Table* table) {
    if (!table->storage) {
        int cap = table->capacity > 0 ? table->capacity : 4;
        return allocate_table_storage(table, cap);
    }
    if (atomic_load(&table->storage->refcount) > 1) {
        RowStorage* old = table->storage;
        int cap = old->capacity > 0 ? old->capacity : 4;
        RowStorage* fresh = malloc(sizeof(*fresh));
        if (!fresh) return false;
        fresh->rows = calloc((size_t)cap, sizeof(Row));
        if (!fresh->rows) { free(fresh); return false; }
        if (table->row_count > 0 && old->rows) {
            memcpy(fresh->rows, old->rows, (size_t)table->row_count * sizeof(Row));
        }
        fresh->capacity = cap;
        atomic_init(&fresh->refcount, 1);
        /* Release this table's reference; free the storage when it hits zero so
           a concurrent release cannot strand the block. */
        if (atomic_fetch_sub(&old->refcount, 1) == 1) {
            free(old->rows);
            free(old);
        }
        table->storage = fresh;
        table->rows = fresh->rows;
        table->capacity = cap;
    }
    return true;
}

void destroy_database(Database* db) {
    if (!db) return;
    for (int t = 0; t < db->table_count; t++) release_table_storage(&db->tables[t]);
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].entries) {
            free(db->indexes[i].entries);
            db->indexes[i].entries = NULL;
        }
    }
    free(db);
}

Database* copy_database(const Database* source) {
    Database* copy = calloc(1, sizeof(*copy));
    if (!copy) return NULL;
    copy->table_count = source->table_count;
    copy->user_count = source->user_count;
    memcpy(copy->users, source->users, (size_t)source->user_count * sizeof(UserAccount));
    copy->grant_count = source->grant_count;
    memcpy(copy->grants, source->grants, (size_t)source->grant_count * sizeof(TableGrant));
    for (int t = 0; t < source->table_count; t++) {
        Table* dest = &copy->tables[t];
        *dest = source->tables[t];
        if (dest->storage) {
            atomic_fetch_add(&dest->storage->refcount, 1);
        }
    }
    copy->index_count = source->index_count;
    for (int i = 0; i < source->index_count; i++) {
        copy->indexes[i] = source->indexes[i];
        if (source->indexes[i].capacity > 0 && source->indexes[i].entries) {
            copy->indexes[i].entries = malloc((size_t)source->indexes[i].capacity * sizeof(IndexEntry));
            if (copy->indexes[i].entries) {
                memcpy(copy->indexes[i].entries, source->indexes[i].entries,
                       (size_t)source->indexes[i].entry_count * sizeof(IndexEntry));
            } else {
                copy->indexes[i].capacity = 0;
                copy->indexes[i].entry_count = 0;
            }
        } else {
            copy->indexes[i].entries = NULL;
        }
    }
    return copy;
}

int find_table_index(const Database* db, const char* name) {
    for (int t = 0; t < db->table_count; t++)
        if (strcmp(db->tables[t].name, name) == 0) return t;
    return -1;
}

int find_column_index(const Table* table, const char* name) {
    for (int c = 0; c < table->col_count; c++)
        if (strcmp(table->columns[c].name, name) == 0) return c;
    return -1;
}

bool reserve_insert_capacity(Table* table) {
    if (table->row_count >= MAX_ROWS) return report_database_error("row limit (%d) reached for table %s", MAX_ROWS, table->name);
    if (!ensure_table_writable(table)) return report_database_error("unable to allocate row storage");
    if (table->row_count < table->capacity) return true;
    int capacity = table->capacity ? table->capacity * 2 : 4;
    if (capacity > MAX_ROWS) capacity = MAX_ROWS;
    Row* rows = realloc(table->storage->rows, (size_t)capacity * sizeof(Row));
    if (!rows) return report_database_error("unable to allocate row storage");
    table->storage->rows = rows;
    table->storage->capacity = capacity;
    table->rows = rows;
    table->capacity = capacity;
    return true;
}
