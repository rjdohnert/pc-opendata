#include "database_engine.h"

#ifdef _WIN32
static int64_t seek_page(int fd, int64_t offset) { return _lseeki64(fd, offset, SEEK_SET); }
static int read_page_data(int fd, void* buf, unsigned count) { return _read(fd, buf, count); }
static int write_page_data(int fd, const void* buf, unsigned count) { return _write(fd, buf, count); }
#else
static int64_t seek_page(int fd, int64_t offset) { return (int64_t)lseek(fd, (off_t)offset, SEEK_SET); }
static int read_page_data(int fd, void* buf, unsigned count) { return (int)read(fd, buf, (size_t)count); }
static int write_page_data(int fd, const void* buf, unsigned count) { return (int)write(fd, buf, (size_t)count); }
#endif

bool slotted_page_init(uint8_t* page, uint32_t page_id, uint16_t page_type) {
    if (!page) return false;
    memset(page, 0, PAGE_SIZE);
    SlottedPageHeader* hdr = (SlottedPageHeader*)((void*)page);
    hdr->page_id = page_id;
    hdr->page_type = page_type;
    hdr->item_count = 0;
    hdr->free_start = (uint16_t)sizeof(SlottedPageHeader);
    hdr->free_end = (uint16_t)PAGE_SIZE;
    hdr->next_page = UINT32_MAX;
    hdr->prev_page = UINT32_MAX;
    return true;
}

size_t slotted_page_free_space(const uint8_t* page) {
    if (!page) return 0;
    const SlottedPageHeader* hdr = (const SlottedPageHeader*)((const void*)page);
    if (hdr->free_end > PAGE_SIZE || hdr->free_end < hdr->free_start) return 0;
    size_t space = (size_t)(hdr->free_end - hdr->free_start);
    return space > 4 ? space - 4 : 0;
}

int slotted_page_insert_record(uint8_t* page, const uint8_t* record, size_t record_len) {
    if (!page || !record || record_len == 0 || record_len > PAGE_SIZE - sizeof(SlottedPageHeader) - 4) return -1;
    SlottedPageHeader* hdr = (SlottedPageHeader*)((void*)page);
    /* Validate header invariants before any arithmetic: a corrupted on-disk page
       must not be able to bypass the free-space check or write out of bounds. */
    if (hdr->free_end > PAGE_SIZE || hdr->free_start > hdr->free_end) return -1;
    if ((size_t)sizeof(SlottedPageHeader) + ((size_t)hdr->item_count + 1) * 4 > hdr->free_end) return -1;
    size_t needed = record_len + 4;
    if ((size_t)hdr->free_end < (size_t)hdr->free_start + needed) return -1;

    hdr->free_end = (uint16_t)(hdr->free_end - (uint16_t)record_len);
    memcpy(page + hdr->free_end, record, record_len);

    uint16_t* slots = (uint16_t*)((void*)(page + sizeof(SlottedPageHeader)));
    int slot_idx = (int)hdr->item_count;
    slots[slot_idx * 2] = hdr->free_end;
    slots[slot_idx * 2 + 1] = (uint16_t)record_len;

    hdr->item_count++;
    hdr->free_start = (uint16_t)(hdr->free_start + 4);
    return slot_idx;
}

bool slotted_page_get_record(const uint8_t* page, int slot_idx, const uint8_t** out_data, size_t* out_len) {
    if (!page || slot_idx < 0) return false;
    const SlottedPageHeader* hdr = (const SlottedPageHeader*)((const void*)page);
    if (hdr->free_end > PAGE_SIZE || hdr->free_start > hdr->free_end) return false;
    if ((size_t)sizeof(SlottedPageHeader) + (size_t)hdr->item_count * 4 > hdr->free_end) return false;
    if (slot_idx >= (int)hdr->item_count) return false;

    const uint16_t* slots = (const uint16_t*)((const void*)(page + sizeof(SlottedPageHeader)));
    uint16_t offset = slots[slot_idx * 2];
    uint16_t length = slots[slot_idx * 2 + 1];

    if (offset < sizeof(SlottedPageHeader) || offset + length > PAGE_SIZE) return false;
    *out_data = page + offset;
    *out_len = (size_t)length;
    return true;
}

uint32_t g_buffer_pool_size_mb = 16;

void buffer_pool_init_sized(BufferPool* bp, uint32_t pool_mb) {
    if (!bp) return;
    memset(bp, 0, sizeof(*bp));
    uint64_t raw_frames = ((uint64_t)pool_mb * 1024ULL * 1024ULL) / PAGE_SIZE;
    if (raw_frames < 64) raw_frames = 64;
    if (raw_frames > 1048576ULL) raw_frames = 1048576ULL;
    uint32_t frames = (uint32_t)raw_frames;
    bp->frame_count = frames;
    bp->hash_size = frames * 2;
    bp->frames = (BufferFrame*)calloc(bp->frame_count, sizeof(BufferFrame));
    bp->hash_table = (int*)malloc((size_t)bp->hash_size * sizeof(int));
    bp->hash_next = (int*)malloc((size_t)bp->frame_count * sizeof(int));
    if (!bp->frames || !bp->hash_table || !bp->hash_next) {
        free(bp->frames);
        free(bp->hash_table);
        free(bp->hash_next);
        memset(bp, 0, sizeof(*bp));
        return;
    }
    for (uint32_t i = 0; i < bp->hash_size; i++) bp->hash_table[i] = -1;
    for (uint32_t i = 0; i < bp->frame_count; i++) bp->hash_next[i] = -1;
}

void buffer_pool_init(BufferPool* bp) {
    uint32_t mb = g_buffer_pool_size_mb > 0 ? g_buffer_pool_size_mb : 16;
    buffer_pool_init_sized(bp, mb);
}

uint8_t* buffer_pool_fetch(BufferPool* bp, int fd, uint32_t page_id, bool is_write) {
    if (!bp || !bp->frames || !bp->hash_table || !bp->hash_next || bp->hash_size == 0) return NULL;
    uint32_t h = page_id % bp->hash_size;
    int curr = bp->hash_table[h];
    while (curr >= 0 && curr < (int)bp->frame_count) {
        if (bp->frames[curr].is_valid && bp->frames[curr].page_id == page_id) {
            bp->frames[curr].pin_count++;
            bp->frames[curr].ref_bit = true;
            if (is_write) bp->frames[curr].is_dirty = true;
            return bp->frames[curr].data;
        }
        curr = bp->hash_next[curr];
    }

    int victim = -1;
    for (uint32_t i = 0; i < bp->frame_count; i++) {
        if (!bp->frames[i].is_valid) {
            victim = (int)i;
            break;
        }
    }

    if (victim < 0) {
        for (uint32_t step = 0; step < bp->frame_count * 2; step++) {
            int idx = (int)(bp->clock_hand % bp->frame_count);
            bp->clock_hand++;
            if (bp->frames[idx].pin_count == 0) {
                if (bp->frames[idx].ref_bit) {
                    bp->frames[idx].ref_bit = false;
                } else {
                    victim = idx;
                    break;
                }
            }
        }
    }

    if (victim < 0) return NULL;

    BufferFrame* frame = &bp->frames[victim];
    if (frame->is_valid) {
        if (frame->is_dirty && fd >= 0) {
            int64_t pos = (int64_t)frame->page_id * PAGE_SIZE;
            if (seek_page(fd, pos) == pos) {
                (void)write_page_data(fd, frame->data, PAGE_SIZE);
            }
        }
        uint32_t old_h = frame->page_id % bp->hash_size;
        int prev = -1;
        int c = bp->hash_table[old_h];
        while (c >= 0 && c < (int)bp->frame_count) {
            if (c == victim) {
                if (prev < 0) bp->hash_table[old_h] = bp->hash_next[victim];
                else bp->hash_next[prev] = bp->hash_next[victim];
                break;
            }
            prev = c;
            c = bp->hash_next[c];
        }
    }

    memset(frame, 0, sizeof(*frame));
    frame->page_id = page_id;
    frame->is_valid = true;
    frame->pin_count = 1;
    frame->ref_bit = true;
    frame->is_dirty = is_write;

    bp->hash_next[victim] = bp->hash_table[h];
    bp->hash_table[h] = victim;

    if (fd >= 0) {
        int64_t pos = (int64_t)page_id * PAGE_SIZE;
        if (seek_page(fd, pos) == pos) {
            int bytes_read = read_page_data(fd, frame->data, PAGE_SIZE);
            if (bytes_read < (int)PAGE_SIZE) {
                memset(frame->data + (bytes_read > 0 ? bytes_read : 0), 0, PAGE_SIZE - (size_t)(bytes_read > 0 ? bytes_read : 0));
            }
        }
    }
    return frame->data;
}

void buffer_pool_unpin(BufferPool* bp, uint32_t page_id, bool is_dirty) {
    if (!bp || !bp->frames || !bp->hash_table || !bp->hash_next || bp->hash_size == 0) return;
    uint32_t h = page_id % bp->hash_size;
    int curr = bp->hash_table[h];
    while (curr >= 0 && curr < (int)bp->frame_count) {
        if (bp->frames[curr].is_valid && bp->frames[curr].page_id == page_id) {
            if (bp->frames[curr].pin_count > 0) bp->frames[curr].pin_count--;
            if (is_dirty) bp->frames[curr].is_dirty = true;
            return;
        }
        curr = bp->hash_next[curr];
    }
}

bool buffer_pool_flush_all(BufferPool* bp, int fd) {
    if (!bp || !bp->frames || fd < 0) return false;
    bool ok = true;
    for (uint32_t i = 0; i < bp->frame_count; i++) {
        if (bp->frames[i].is_valid && bp->frames[i].is_dirty) {
            int64_t pos = (int64_t)bp->frames[i].page_id * PAGE_SIZE;
            if (seek_page(fd, pos) != pos || write_page_data(fd, bp->frames[i].data, PAGE_SIZE) != (int)PAGE_SIZE) {
                ok = false;
            } else {
                bp->frames[i].is_dirty = false;
            }
        }
    }
    return ok;
}

uint32_t buffer_pool_alloc_page(BufferPool* bp, int fd, uint16_t page_type) {
    if (!bp) return UINT32_MAX;
    uint32_t new_page_id = bp->page_count++;
    uint8_t* page = buffer_pool_fetch(bp, fd, new_page_id, true);
    if (!page) {
        bp->page_count--;
        return UINT32_MAX;
    }
    slotted_page_init(page, new_page_id, page_type);
    buffer_pool_unpin(bp, new_page_id, true);
    return new_page_id;
}

void buffer_pool_destroy(BufferPool* bp, int fd) {
    if (!bp) return;
    if (fd >= 0) {
        buffer_pool_flush_all(bp, fd);
    }
    if (bp->frames) {
        free(bp->frames);
        bp->frames = NULL;
    }
    if (bp->hash_table) {
        free(bp->hash_table);
        bp->hash_table = NULL;
    }
    if (bp->hash_next) {
        free(bp->hash_next);
        bp->hash_next = NULL;
    }
    memset(bp, 0, sizeof(*bp));
}

bool slotted_overflow_write(BufferPool* bp, int fd, const uint8_t* data, size_t len, uint32_t* out_first_page) {
    if (!bp || !out_first_page) return false;
    if (len > 0 && !data) return false;

    const size_t chunk_cap = PAGE_SIZE - sizeof(OverflowPageHeader);
    uint32_t first_pid = UINT32_MAX;
    uint32_t prev_pid = UINT32_MAX;
    size_t offset = 0;

    do {
        size_t chunk = len - offset;
        if (chunk > chunk_cap) chunk = chunk_cap;

        uint32_t pid = buffer_pool_alloc_page(bp, fd, PAGE_TYPE_OVERFLOW);
        if (pid == UINT32_MAX) return false;

        if (first_pid == UINT32_MAX) {
            first_pid = pid;
        }

        if (prev_pid != UINT32_MAX) {
            uint8_t* prev_page = buffer_pool_fetch(bp, fd, prev_pid, true);
            if (prev_page) {
                OverflowPageHeader* prev_hdr = (OverflowPageHeader*)((void*)prev_page);
                prev_hdr->next_page = pid;
                buffer_pool_unpin(bp, prev_pid, true);
            }
        }

        uint8_t* page = buffer_pool_fetch(bp, fd, pid, true);
        if (!page) return false;
        memset(page, 0, PAGE_SIZE);
        OverflowPageHeader* hdr = (OverflowPageHeader*)((void*)page);
        hdr->next_page = UINT32_MAX;
        hdr->data_len = (uint16_t)chunk;
        hdr->reserved = 0;
        if (chunk > 0) {
            memcpy(page + sizeof(OverflowPageHeader), data + offset, chunk);
        }
        buffer_pool_unpin(bp, pid, true);

        prev_pid = pid;
        offset += chunk;
    } while (offset < len);

    *out_first_page = first_pid;
    return true;
}

bool slotted_overflow_read(BufferPool* bp, int fd, uint32_t first_page, uint8_t* dest, size_t dest_cap, size_t* out_len) {
    if (!bp || !dest || !out_len || first_page == UINT32_MAX) return false;

    size_t total_read = 0;
    uint32_t curr_pid = first_page;
    const size_t chunk_cap = PAGE_SIZE - sizeof(OverflowPageHeader);
    uint32_t pages_visited = 0;

    while (curr_pid != UINT32_MAX && pages_visited < 100000) {
        pages_visited++;
        uint8_t* page = buffer_pool_fetch(bp, fd, curr_pid, false);
        if (!page) return false;

        const OverflowPageHeader* hdr = (const OverflowPageHeader*)((const void*)page);
        uint16_t chunk_len = hdr->data_len;
        if (chunk_len > chunk_cap) {
            buffer_pool_unpin(bp, curr_pid, false);
            return false;
        }

        if (total_read + chunk_len > dest_cap) {
            buffer_pool_unpin(bp, curr_pid, false);
            return false;
        }

        if (chunk_len > 0) {
            memcpy(dest + total_read, page + sizeof(OverflowPageHeader), chunk_len);
            total_read += chunk_len;
        }

        uint32_t next = hdr->next_page;
        buffer_pool_unpin(bp, curr_pid, false);
        curr_pid = next;
    }

    *out_len = total_read;
    return true;
}
