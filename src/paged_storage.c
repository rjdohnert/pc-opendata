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
    const SlottedPageHeader* hdr = (const SlottedPageHeader*)((const void*)page);
    if (hdr->free_end < hdr->free_start) return 0;
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

void buffer_pool_init(BufferPool* bp) {
    memset(bp, 0, sizeof(*bp));
}

uint8_t* buffer_pool_fetch(BufferPool* bp, int fd, uint32_t page_id, bool is_write) {
    for (int i = 0; i < BUFFER_POOL_FRAMES; i++) {
        if (bp->frames[i].is_valid && bp->frames[i].page_id == page_id) {
            bp->frames[i].pin_count++;
            bp->frames[i].ref_bit = true;
            if (is_write) bp->frames[i].is_dirty = true;
            return bp->frames[i].data;
        }
    }

    int victim = -1;
    for (int i = 0; i < BUFFER_POOL_FRAMES; i++) {
        if (!bp->frames[i].is_valid) {
            victim = i;
            break;
        }
    }

    if (victim < 0) {
        for (int step = 0; step < BUFFER_POOL_FRAMES * 2; step++) {
            int idx = (int)(bp->clock_hand % BUFFER_POOL_FRAMES);
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
    if (frame->is_valid && frame->is_dirty && fd >= 0) {
        int64_t pos = (int64_t)frame->page_id * PAGE_SIZE;
        if (seek_page(fd, pos) == pos) {
            (void)write_page_data(fd, frame->data, PAGE_SIZE);
        }
    }

    memset(frame, 0, sizeof(*frame));
    frame->page_id = page_id;
    frame->is_valid = true;
    frame->pin_count = 1;
    frame->ref_bit = true;
    frame->is_dirty = is_write;

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
    for (int i = 0; i < BUFFER_POOL_FRAMES; i++) {
        if (bp->frames[i].is_valid && bp->frames[i].page_id == page_id) {
            if (bp->frames[i].pin_count > 0) bp->frames[i].pin_count--;
            if (is_dirty) bp->frames[i].is_dirty = true;
            return;
        }
    }
}

bool buffer_pool_flush_all(BufferPool* bp, int fd) {
    if (fd < 0) return false;
    bool ok = true;
    for (int i = 0; i < BUFFER_POOL_FRAMES; i++) {
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
    uint32_t new_page_id = bp->page_count++;
    uint8_t* page = buffer_pool_fetch(bp, fd, new_page_id, true);
    if (!page) return UINT32_MAX;
    slotted_page_init(page, new_page_id, page_type);
    buffer_pool_unpin(bp, new_page_id, true);
    return new_page_id;
}

void buffer_pool_destroy(BufferPool* bp, int fd) {
    if (fd >= 0) {
        buffer_pool_flush_all(bp, fd);
    }
    memset(bp, 0, sizeof(*bp));
}
