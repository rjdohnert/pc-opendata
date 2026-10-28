#include "database_engine.h"

size_t encode_slotted_row(const ColumnDef* cols, int col_count, const Row* row, uint8_t* dest, size_t max_size) {
    if (col_count < 1 || col_count > MAX_COLUMNS) return 0;
    size_t header_size = 4 + (size_t)col_count * 2;
    if (header_size > max_size) return 0;
    uint32_t null_bitmap = 0;
    uint16_t offsets[MAX_COLUMNS] = {0};
    size_t payload_offset = header_size;

    for (int c = 0; c < col_count; c++) {
        (void)cols;
        if (row->values[c].is_null) {
            null_bitmap |= (1u << (unsigned)c);
            offsets[c] = (uint16_t)payload_offset;
        } else {
            size_t len = strlen(row->values[c].text);
            if (payload_offset + len > max_size || payload_offset + len > UINT16_MAX) return 0;
            offsets[c] = (uint16_t)payload_offset;
            memcpy(dest + payload_offset, row->values[c].text, len);
            payload_offset += len;
        }
    }
    dest[0] = (uint8_t)(null_bitmap & 0xFFu);
    dest[1] = (uint8_t)((null_bitmap >> 8) & 0xFFu);
    dest[2] = (uint8_t)((null_bitmap >> 16) & 0xFFu);
    dest[3] = (uint8_t)((null_bitmap >> 24) & 0xFFu);
    for (int c = 0; c < col_count; c++) {
        dest[4 + c * 2] = (uint8_t)(offsets[c] & 0xFFu);
        dest[4 + c * 2 + 1] = (uint8_t)((offsets[c] >> 8) & 0xFFu);
    }
    return payload_offset;
}

bool decode_slotted_row(const ColumnDef* cols, int col_count, const uint8_t* src, size_t src_size, Row* row) {
    if (col_count < 1 || col_count > MAX_COLUMNS) return false;
    size_t header_size = 4 + (size_t)col_count * 2;
    if (src_size < header_size) return false;
    uint32_t null_bitmap = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    uint16_t offsets[MAX_COLUMNS];
    for (int c = 0; c < col_count; c++) {
        offsets[c] = (uint16_t)((uint16_t)src[4 + c * 2] | ((uint16_t)src[4 + c * 2 + 1] << 8));
    }
    memset(row, 0, sizeof(*row));
    for (int c = 0; c < col_count; c++) {
        (void)cols;
        if (null_bitmap & (1u << (unsigned)c)) {
            row->values[c].is_null = true;
            row->values[c].text[0] = '\0';
        } else {
            size_t start = offsets[c];
            size_t end = (c + 1 < col_count) ? offsets[c + 1] : src_size;
            if (start > end || end > src_size) return false;
            size_t len = end - start;
            if (len >= MAX_VAL_LEN) return false;
            memcpy(row->values[c].text, src + start, len);
            row->values[c].text[len] = '\0';
            row->values[c].is_null = false;
        }
    }
    return true;
}

bool read_slotted_cell(const ColumnDef* col, const uint8_t* src, size_t src_size, int col_idx, int col_count, Value* val) {
    (void)col;
    if (col_idx < 0 || col_idx >= col_count || col_count > MAX_COLUMNS) return false;
    size_t header_size = 4 + (size_t)col_count * 2;
    if (src_size < header_size) return false;
    uint32_t null_bitmap = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    if (null_bitmap & (1u << (unsigned)col_idx)) {
        val->is_null = true;
        val->text[0] = '\0';
        return true;
    }
    uint16_t start = (uint16_t)((uint16_t)src[4 + col_idx * 2] | ((uint16_t)src[4 + col_idx * 2 + 1] << 8));
    size_t end = src_size;
    if (col_idx + 1 < col_count) {
        end = (size_t)((uint16_t)src[4 + (col_idx + 1) * 2] | ((uint16_t)src[4 + (col_idx + 1) * 2 + 1] << 8));
    }
    if ((size_t)start > end || end > src_size) return false;
    size_t len = end - (size_t)start;
    if (len >= MAX_VAL_LEN) return false;
    memcpy(val->text, src + start, len);
    val->text[len] = '\0';
    val->is_null = false;
    return true;
}
