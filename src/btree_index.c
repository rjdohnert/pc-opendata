#include "database_engine.h"

#define BTREE_MAX_ENTRIES 40
/* Generous upper bound on tree height; traversal beyond this implies a cycle. */
#define BTREE_MAX_DEPTH 64

typedef struct {
    char text[64];
    uint32_t page_id;
    uint16_t slot_idx;
    uint16_t reserved;
} DiskBTreeLeafEntry;

typedef struct {
    SlottedPageHeader hdr;
    DiskBTreeLeafEntry entries[BTREE_MAX_ENTRIES];
} DiskBTreeLeafPage;

typedef struct {
    char text[64];
    uint32_t child_page_id;
} DiskBTreeInternalEntry;

typedef struct {
    SlottedPageHeader hdr;
    uint32_t leftmost_child;
    uint32_t reserved;
    DiskBTreeInternalEntry entries[BTREE_MAX_ENTRIES];
} DiskBTreeInternalPage;

static int btree_key_compare(DataType type, const char* a, const char* b) {
    Value va, vb;
    memset(&va, 0, sizeof(va));
    memset(&vb, 0, sizeof(vb));
    snprintf(va.text, sizeof(va.text), "%s", a);
    snprintf(vb.text, sizeof(vb.text), "%s", b);
    return compare_typed_values(type, &va, &vb);
}

uint32_t btree_create(BufferPool* bp, int fd, DataType key_type) {
    (void)key_type;
    uint32_t root_id = buffer_pool_alloc_page(bp, fd, PAGE_TYPE_BTREE_LEAF);
    return root_id;
}

static uint32_t btree_find_leaf_with_parent(BufferPool* bp, int fd, uint32_t root_page, DataType key_type, const Value* key, uint32_t* out_parent) {
    uint32_t curr = root_page;
    uint32_t parent = UINT32_MAX;
    int hops = 0;
    while (curr != UINT32_MAX) {
        if (++hops > BTREE_MAX_DEPTH) return UINT32_MAX; /* cycle or corrupt page pointers */
        uint8_t* page = buffer_pool_fetch(bp, fd, curr, false);
        if (!page) return UINT32_MAX;
        SlottedPageHeader* hdr = (SlottedPageHeader*)((void*)page);
        if (hdr->item_count > BTREE_MAX_ENTRIES) {
            buffer_pool_unpin(bp, curr, false);
            return UINT32_MAX; /* corrupt on-disk page header */
        }
        if (hdr->page_type == PAGE_TYPE_BTREE_LEAF) {
            buffer_pool_unpin(bp, curr, false);
            if (out_parent) *out_parent = parent;
            return curr;
        }
        if (hdr->page_type != PAGE_TYPE_BTREE_INTERNAL) {
            buffer_pool_unpin(bp, curr, false);
            return UINT32_MAX;
        }
        parent = curr;
        DiskBTreeInternalPage* internal = (DiskBTreeInternalPage*)((void*)page);
        uint32_t next = internal->leftmost_child;
        for (int i = 0; i < (int)hdr->item_count; i++) {
            if (btree_key_compare(key_type, key->text, internal->entries[i].text) >= 0) {
                next = internal->entries[i].child_page_id;
            } else {
                break;
            }
        }
        buffer_pool_unpin(bp, curr, false);
        curr = next;
    }
    return UINT32_MAX;
}

bool btree_search(BufferPool* bp, int fd, uint32_t root_page, DataType key_type, const Value* key, uint32_t* out_page, int* out_slot) {
    if (!key || !out_page || !out_slot) return false;
    uint32_t leaf_id = btree_find_leaf_with_parent(bp, fd, root_page, key_type, key, NULL);
    if (leaf_id == UINT32_MAX) return false;

    uint8_t* page = buffer_pool_fetch(bp, fd, leaf_id, false);
    if (!page) return false;
    DiskBTreeLeafPage* leaf = (DiskBTreeLeafPage*)((void*)page);
    if (leaf->hdr.item_count > BTREE_MAX_ENTRIES) {
        buffer_pool_unpin(bp, leaf_id, false);
        return false;
    }

    bool found = false;
    for (int i = 0; i < (int)leaf->hdr.item_count; i++) {
        int cmp = btree_key_compare(key_type, key->text, leaf->entries[i].text);
        if (cmp == 0) {
            *out_page = leaf->entries[i].page_id;
            *out_slot = (int)leaf->entries[i].slot_idx;
            found = true;
            break;
        }
        if (cmp < 0) break;
    }
    buffer_pool_unpin(bp, leaf_id, false);
    return found;
}

bool btree_insert(BufferPool* bp, int fd, uint32_t* root_page, DataType key_type, const Value* key, uint32_t data_page, int slot_idx) {
    if (!key || strlen(key->text) >= 64)
        return report_database_error("disk B+ tree keys are limited to 63 bytes");
    uint32_t parent_id = UINT32_MAX;
    uint32_t leaf_id = btree_find_leaf_with_parent(bp, fd, *root_page, key_type, key, &parent_id);
    if (leaf_id == UINT32_MAX) return false;

    uint8_t* page = buffer_pool_fetch(bp, fd, leaf_id, true);
    if (!page) return false;
    DiskBTreeLeafPage* leaf = (DiskBTreeLeafPage*)((void*)page);
    if (leaf->hdr.item_count > BTREE_MAX_ENTRIES) {
        buffer_pool_unpin(bp, leaf_id, false);
        return false;
    }

    int pos = 0;
    while (pos < (int)leaf->hdr.item_count && btree_key_compare(key_type, leaf->entries[pos].text, key->text) < 0) {
        pos++;
    }

    if (leaf->hdr.item_count < BTREE_MAX_ENTRIES) {
        for (int i = (int)leaf->hdr.item_count; i > pos; i--) {
            leaf->entries[i] = leaf->entries[i - 1];
        }
        snprintf(leaf->entries[pos].text, sizeof(leaf->entries[pos].text), "%.63s", key->text);
        leaf->entries[pos].page_id = data_page;
        leaf->entries[pos].slot_idx = (uint16_t)slot_idx;
        leaf->hdr.item_count++;
        buffer_pool_unpin(bp, leaf_id, true);
        return true;
    }

    /* Leaf split: 50/50 */
    uint32_t new_leaf_id = buffer_pool_alloc_page(bp, fd, PAGE_TYPE_BTREE_LEAF);
    uint8_t* new_page = buffer_pool_fetch(bp, fd, new_leaf_id, true);
    if (!new_page) { buffer_pool_unpin(bp, leaf_id, false); return false; }
    DiskBTreeLeafPage* new_leaf = (DiskBTreeLeafPage*)((void*)new_page);

    int split = BTREE_MAX_ENTRIES / 2;
    int j = 0;
    for (int i = split; i < BTREE_MAX_ENTRIES; i++) {
        new_leaf->entries[j++] = leaf->entries[i];
    }
    new_leaf->hdr.item_count = (uint16_t)j;
    leaf->hdr.item_count = (uint16_t)split;

    new_leaf->hdr.next_page = leaf->hdr.next_page;
    new_leaf->hdr.prev_page = leaf_id;
    leaf->hdr.next_page = new_leaf_id;

    /* Insert into appropriate half */
    if (pos < split) {
        for (int i = (int)leaf->hdr.item_count; i > pos; i--) leaf->entries[i] = leaf->entries[i - 1];
        snprintf(leaf->entries[pos].text, sizeof(leaf->entries[pos].text), "%.63s", key->text);
        leaf->entries[pos].page_id = data_page;
        leaf->entries[pos].slot_idx = (uint16_t)slot_idx;
        leaf->hdr.item_count++;
    } else {
        int npos = pos - split;
        for (int i = (int)new_leaf->hdr.item_count; i > npos; i--) new_leaf->entries[i] = new_leaf->entries[i - 1];
        snprintf(new_leaf->entries[npos].text, sizeof(new_leaf->entries[npos].text), "%.63s", key->text);
        new_leaf->entries[npos].page_id = data_page;
        new_leaf->entries[npos].slot_idx = (uint16_t)slot_idx;
        new_leaf->hdr.item_count++;
    }

    /* Update parent internal page or create new root */
    if (leaf_id == *root_page) {
        uint32_t new_root_id = buffer_pool_alloc_page(bp, fd, PAGE_TYPE_BTREE_INTERNAL);
        uint8_t* root_p = buffer_pool_fetch(bp, fd, new_root_id, true);
        if (root_p) {
            DiskBTreeInternalPage* root_internal = (DiskBTreeInternalPage*)((void*)root_p);
            root_internal->leftmost_child = leaf_id;
            snprintf(root_internal->entries[0].text, sizeof(root_internal->entries[0].text), "%s", new_leaf->entries[0].text);
            root_internal->entries[0].child_page_id = new_leaf_id;
            root_internal->hdr.item_count = 1;
            buffer_pool_unpin(bp, new_root_id, true);
            *root_page = new_root_id;
        } else {
            buffer_pool_unpin(bp, leaf_id, false);
            buffer_pool_unpin(bp, new_leaf_id, false);
            return report_database_error("cannot allocate root page for B+ tree");
        }
    } else if (parent_id != UINT32_MAX) {
        uint8_t* parent_p = buffer_pool_fetch(bp, fd, parent_id, true);
        if (!parent_p || ((SlottedPageHeader*)((void*)parent_p))->item_count >= BTREE_MAX_ENTRIES) {
            /* Internal node split is not implemented; fail loudly instead of
               silently orphaning the new leaf and losing half its keys. */
            if (parent_p) buffer_pool_unpin(bp, parent_id, false);
            buffer_pool_unpin(bp, leaf_id, true);
            buffer_pool_unpin(bp, new_leaf_id, true);
            return report_database_error("disk B+ tree internal node is full; insert aborted");
        }
        DiskBTreeInternalPage* p_int = (DiskBTreeInternalPage*)((void*)parent_p);
        int ipos = 0;
        while (ipos < (int)p_int->hdr.item_count &&
               btree_key_compare(key_type, p_int->entries[ipos].text, new_leaf->entries[0].text) < 0) {
            ipos++;
        }
        for (int k = (int)p_int->hdr.item_count; k > ipos; k--) {
            p_int->entries[k] = p_int->entries[k - 1];
        }
        snprintf(p_int->entries[ipos].text, sizeof(p_int->entries[ipos].text), "%s", new_leaf->entries[0].text);
        p_int->entries[ipos].child_page_id = new_leaf_id;
        p_int->hdr.item_count++;
        buffer_pool_unpin(bp, parent_id, true);
    }

    buffer_pool_unpin(bp, leaf_id, true);
    buffer_pool_unpin(bp, new_leaf_id, true);
    return true;
}

int btree_range_scan(BufferPool* bp, int fd, uint32_t root_page, DataType key_type, const Value* low, const Value* high, uint32_t* out_pages, int* out_slots, int max_results) {
    if (!low || !out_pages || !out_slots || max_results <= 0) return 0;
    uint32_t leaf_id = btree_find_leaf_with_parent(bp, fd, root_page, key_type, low, NULL);
    if (leaf_id == UINT32_MAX) return 0;

    int count = 0;
    int hops = 0;
    while (leaf_id != UINT32_MAX && count < max_results) {
        if (++hops > BTREE_MAX_DEPTH * 4) break; /* corrupt next_page chain */
        uint8_t* page = buffer_pool_fetch(bp, fd, leaf_id, false);
        if (!page) break;
        DiskBTreeLeafPage* leaf = (DiskBTreeLeafPage*)((void*)page);
        if (leaf->hdr.item_count > BTREE_MAX_ENTRIES) {
            buffer_pool_unpin(bp, leaf_id, false);
            break;
        }

        bool exceeded = false;
        for (int i = 0; i < (int)leaf->hdr.item_count && count < max_results; i++) {
            if (btree_key_compare(key_type, leaf->entries[i].text, low->text) >= 0) {
                if (high && btree_key_compare(key_type, leaf->entries[i].text, high->text) > 0) {
                    exceeded = true;
                    break;
                }
                out_pages[count] = leaf->entries[i].page_id;
                out_slots[count] = (int)leaf->entries[i].slot_idx;
                count++;
            }
        }
        uint32_t next = leaf->hdr.next_page;
        buffer_pool_unpin(bp, leaf_id, false);
        if (exceeded) break;
        leaf_id = next;
    }
    return count;
}
