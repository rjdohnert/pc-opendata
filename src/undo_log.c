#include "database_engine.h"

UndoLog g_undo_log = {0};

void undo_log_init(void) {
    undo_log_clear();
}

void undo_log_clear(void) {
    UndoEntry* cur = g_undo_log.head;
    while (cur) {
        UndoEntry* prev = cur->prev;
        free(cur);
        cur = prev;
    }
    g_undo_log.head = NULL;
    g_undo_log.entry_count = 0;
    g_undo_log.memory_bytes = 0;
}

bool undo_log_push_insert(int table_idx, int row_idx) {
    UndoEntry* entry = (UndoEntry*)malloc(sizeof(UndoEntry));
    if (!entry) return false;
    memset(entry, 0, sizeof(*entry));
    entry->op = UNDO_OP_INSERT;
    entry->table_idx = table_idx;
    entry->row_idx = row_idx;
    entry->prev = g_undo_log.head;
    g_undo_log.head = entry;
    g_undo_log.entry_count++;
    g_undo_log.memory_bytes += sizeof(UndoEntry);
    return true;
}

bool undo_log_push_update(int table_idx, int row_idx, const Row* old_row) {
    UndoEntry* entry = (UndoEntry*)malloc(sizeof(UndoEntry));
    if (!entry) return false;
    memset(entry, 0, sizeof(*entry));
    entry->op = UNDO_OP_UPDATE;
    entry->table_idx = table_idx;
    entry->row_idx = row_idx;
    if (old_row) entry->old_row = *old_row;
    entry->prev = g_undo_log.head;
    g_undo_log.head = entry;
    g_undo_log.entry_count++;
    g_undo_log.memory_bytes += sizeof(UndoEntry);
    return true;
}

bool undo_log_push_delete(int table_idx, int row_idx, const Row* old_row) {
    UndoEntry* entry = (UndoEntry*)malloc(sizeof(UndoEntry));
    if (!entry) return false;
    memset(entry, 0, sizeof(*entry));
    entry->op = UNDO_OP_DELETE;
    entry->table_idx = table_idx;
    entry->row_idx = row_idx;
    if (old_row) entry->old_row = *old_row;
    entry->prev = g_undo_log.head;
    g_undo_log.head = entry;
    g_undo_log.entry_count++;
    g_undo_log.memory_bytes += sizeof(UndoEntry);
    return true;
}

void undo_log_rollback_to(UndoEntry* savepoint, Database* db) {
    if (!db) return;
    while (g_undo_log.head != savepoint) {
        UndoEntry* entry = g_undo_log.head;
        if (!entry) break;
        if (entry->table_idx >= 0 && entry->table_idx < db->table_count) {
            Table* t = &db->tables[entry->table_idx];
            switch (entry->op) {
            case UNDO_OP_INSERT:
                if (entry->row_idx == t->row_count - 1) {
                    t->row_count--;
                } else if (entry->row_idx >= 0 && entry->row_idx < t->row_count) {
                    for (int i = entry->row_idx; i < t->row_count - 1; i++) {
                        t->rows[i] = t->rows[i + 1];
                    }
                    t->row_count--;
                }
                break;
            case UNDO_OP_UPDATE:
                if (entry->row_idx >= 0 && entry->row_idx < t->row_count) {
                    t->rows[entry->row_idx] = entry->old_row;
                }
                break;
            case UNDO_OP_DELETE:
                if (entry->row_idx >= 0 && reserve_insert_capacity(t)) {
                    if (entry->row_idx < t->row_count) {
                        for (int i = t->row_count; i > entry->row_idx; i--) {
                            t->rows[i] = t->rows[i - 1];
                        }
                        t->rows[entry->row_idx] = entry->old_row;
                        t->row_count++;
                    } else {
                        t->rows[t->row_count++] = entry->old_row;
                    }
                }
                break;
            }
        }
        UndoEntry* prev = entry->prev;
        free(entry);
        g_undo_log.head = prev;
        g_undo_log.entry_count--;
        g_undo_log.memory_bytes -= sizeof(UndoEntry);
    }
    rebuild_secondary_indexes(db);
}

void undo_log_rollback_all(Database* db) {
    undo_log_rollback_to(NULL, db);
}
