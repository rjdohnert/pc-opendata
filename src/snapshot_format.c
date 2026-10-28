#include "database_engine.h"

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

static bool derive_tde_raw_key(unsigned char key_bytes[32]) {
    if (!g_tde_enabled || !g_tde_key[0]) return false;
    size_t len = strlen(g_tde_key);
    if (len == 64) {
        for (int i = 0; i < 32; i++) {
            unsigned int byte_val = 0;
            if (sscanf(g_tde_key + i * 2, "%02x", &byte_val) != 1) return false;
            key_bytes[i] = (unsigned char)byte_val;
        }
        return true;
    }
    memset(key_bytes, 0, 32);
    memcpy(key_bytes, g_tde_key, len < 32 ? len : 32);
    return true;
}

static bool tde_encrypt_image(Image* image) {
    unsigned char key_bytes[32];
    if (!derive_tde_raw_key(key_bytes)) return false;
    unsigned char iv[16];
#ifdef _WIN32
    if (BCryptGenRandom(NULL, iv, sizeof(iv), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key_h = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) < 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0); return false;
    }
    if (BCryptGenerateSymmetricKey(alg, &key_h, NULL, 0, key_bytes, 32, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0); return false;
    }
    ULONG cipher_len = 0;
    unsigned char iv_copy[16];
    memcpy(iv_copy, iv, 16);
    if (BCryptEncrypt(key_h, image->data, (ULONG)image->size, NULL, iv_copy, 16, NULL, 0, &cipher_len, BCRYPT_BLOCK_PADDING) < 0) {
        BCryptDestroyKey(key_h); BCryptCloseAlgorithmProvider(alg, 0); return false;
    }
    size_t total_enc_size = 8 + 16 + 4 + cipher_len;
    unsigned char* enc_data = malloc(total_enc_size);
    if (!enc_data) { BCryptDestroyKey(key_h); BCryptCloseAlgorithmProvider(alg, 0); return false; }
    memcpy(enc_data, "PCODEN01", 8);
    memcpy(enc_data + 8, iv, 16);
    uint32_t orig_sz = (uint32_t)image->size;
    enc_data[24] = (unsigned char)(orig_sz);
    enc_data[25] = (unsigned char)(orig_sz >> 8);
    enc_data[26] = (unsigned char)(orig_sz >> 16);
    enc_data[27] = (unsigned char)(orig_sz >> 24);
    memcpy(iv_copy, iv, 16);
    ULONG written = 0;
    if (BCryptEncrypt(key_h, image->data, (ULONG)image->size, NULL, iv_copy, 16, enc_data + 28, cipher_len, &written, BCRYPT_BLOCK_PADDING) < 0) {
        free(enc_data); BCryptDestroyKey(key_h); BCryptCloseAlgorithmProvider(alg, 0); return false;
    }
    BCryptDestroyKey(key_h);
    BCryptCloseAlgorithmProvider(alg, 0);
    free(image->data);
    image->data = enc_data;
    image->size = image->capacity = total_enc_size;
    return true;
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    RAND_bytes(iv, sizeof(iv));
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key_bytes, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); return false;
    }
    size_t total_enc_size = 8 + 16 + 4 + image->size + 32;
    unsigned char* enc_data = malloc(total_enc_size);
    if (!enc_data) { EVP_CIPHER_CTX_free(ctx); return false; }
    memcpy(enc_data, "PCODEN01", 8);
    memcpy(enc_data + 8, iv, 16);
    uint32_t orig_sz = (uint32_t)image->size;
    enc_data[24] = (unsigned char)(orig_sz);
    enc_data[25] = (unsigned char)(orig_sz >> 8);
    enc_data[26] = (unsigned char)(orig_sz >> 16);
    enc_data[27] = (unsigned char)(orig_sz >> 24);
    int outlen = 0;
    EVP_EncryptUpdate(ctx, enc_data + 28, &outlen, image->data, (int)image->size);
    int total_cipher = outlen;
    EVP_EncryptFinal_ex(ctx, enc_data + 28 + total_cipher, &outlen);
    total_cipher += outlen;
    EVP_CIPHER_CTX_free(ctx);
    free(image->data);
    image->data = enc_data;
    image->size = image->capacity = 28 + (size_t)total_cipher;
    return true;
#endif
}

static bool tde_decrypt_image(Image* image) {
    if (!image || image->size < 28 || memcmp(image->data, "PCODEN01", 8) != 0) return false;
    unsigned char key_bytes[32];
    if (!derive_tde_raw_key(key_bytes)) {
        report_database_error("database is encrypted with TDE: encryption key required");
        return false;
    }
    unsigned char iv[16];
    memcpy(iv, image->data + 8, 16);
    uint32_t orig_sz = (uint32_t)image->data[24] | ((uint32_t)image->data[25] << 8) |
                       ((uint32_t)image->data[26] << 16) | ((uint32_t)image->data[27] << 24);
    if (orig_sz > MAX_IMAGE) return false;
    const unsigned char* cipher = image->data + 28;
    size_t cipher_len = image->size - 28;
    unsigned char* plain = malloc((size_t)orig_sz + 64);
    if (!plain) return false;

#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key_h = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) < 0) { free(plain); return false; }
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0) {
        free(plain); BCryptCloseAlgorithmProvider(alg, 0); return false;
    }
    if (BCryptGenerateSymmetricKey(alg, &key_h, NULL, 0, key_bytes, 32, 0) < 0) {
        free(plain); BCryptCloseAlgorithmProvider(alg, 0); return false;
    }
    ULONG plain_len = 0;
    unsigned char iv_copy[16];
    memcpy(iv_copy, iv, 16);
    if (BCryptDecrypt(key_h, (PUCHAR)cipher, (ULONG)cipher_len, NULL, iv_copy, 16, plain, orig_sz + 64, &plain_len, BCRYPT_BLOCK_PADDING) < 0) {
        free(plain); BCryptDestroyKey(key_h); BCryptCloseAlgorithmProvider(alg, 0);
        report_database_error("TDE decryption failed: invalid encryption key");
        return false;
    }
    BCryptDestroyKey(key_h);
    BCryptCloseAlgorithmProvider(alg, 0);
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(plain); return false; }
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key_bytes, iv) != 1) {
        free(plain); EVP_CIPHER_CTX_free(ctx); return false;
    }
    int outlen = 0;
    EVP_DecryptUpdate(ctx, plain, &outlen, cipher, (int)cipher_len);
    int total_plain = outlen;
    if (EVP_DecryptFinal_ex(ctx, plain + total_plain, &outlen) != 1) {
        free(plain); EVP_CIPHER_CTX_free(ctx);
        report_database_error("TDE decryption failed: invalid encryption key");
        return false;
    }
    total_plain += outlen;
    EVP_CIPHER_CTX_free(ctx);
#endif
    free(image->data);
    image->data = plain;
    image->size = image->capacity = (size_t)orig_sz;
    return true;
}

static bool has_extension_ci(const char* path, const char* ext) {
    if (!path || !ext) return false;
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char* tail = path + (plen - elen);
    for (size_t i = 0; i < elen; i++) {
        if (uppercase_ascii(tail[i]) != uppercase_ascii(ext[i])) return false;
    }
    return true;
}

static const char* select_snapshot_magic(const char* path) {
    if (has_extension_ci(path, ".mdf") || has_extension_ci(path, ".ndf") ||
        has_extension_ci(path, ".ldf") || has_extension_ci(path, ".bak")) {
        return "MSQLMDF1";
    }
    if (has_extension_ci(path, ".dbf") || has_extension_ci(path, ".ora") ||
        has_extension_ci(path, ".dmp") || has_extension_ci(path, ".ctl")) {
        return "ORCLDBF1";
    }
    return "PCOD0001";
}

static bool append_snapshot_bytes(Image* image, const void* data, size_t size) {
    if (!image->ok || size > MAX_IMAGE || image->size > MAX_IMAGE - size) return image->ok = false;
    if (image->size + size > image->capacity) {
        size_t capacity = image->capacity ? image->capacity : 4096;
        while (capacity < image->size + size) capacity *= 2;
        unsigned char* bytes = realloc(image->data, capacity);
        if (!bytes) return image->ok = false;
        image->data = bytes; image->capacity = capacity;
    }
    memcpy(image->data + image->size, data, size);
    image->size += size; return true;
}
static void encode_u32(Image* image, uint32_t value) {
    unsigned char bytes[4];
    for (unsigned i = 0; i < 4; i++) bytes[i] = (unsigned char)(value >> (i * 8));
    append_snapshot_bytes(image, bytes, 4);
}
static void encode_u64(Image* image, uint64_t value) {
    encode_u32(image, (uint32_t)value); encode_u32(image, (uint32_t)(value >> 32));
}
static void encode_text(Image* image, const char* text) {
    size_t length = strlen(text);
    encode_u32(image, (uint32_t)length); append_snapshot_bytes(image, text, length);
}
static bool decode_snapshot_bytes(Image* image, void* dest, size_t size) {
    if (!image->ok || image->offset > image->size || size > image->size - image->offset)
        return image->ok = false;
    memcpy(dest, image->data + image->offset, size);
    image->offset += size; return true;
}
static uint32_t decode_u32(Image* image) {
    unsigned char b[4] = {0};
    decode_snapshot_bytes(image, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t decode_u64(Image* image) {
    uint64_t low = decode_u32(image), high = decode_u32(image);
    return low | (high << 32);
}
static bool decode_text(Image* image, char* text, size_t capacity) {
    uint32_t length = decode_u32(image);
    if (!image->ok || length >= capacity || !decode_snapshot_bytes(image, text, length))
        return image->ok = false;
    text[length] = '\0';
    if (strlen(text) != length) return image->ok = false;
    return true;
}
static uint32_t calculate_crc32(const unsigned char* bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool encode_database_snapshot(const Database* db, uint64_t identity, uint64_t generation, Image* image) {
    memset(image, 0, sizeof(*image)); image->ok = true;
    append_snapshot_bytes(image, select_snapshot_magic(g_path), 8);
    encode_u64(image, identity); encode_u64(image, generation);
    encode_u32(image, (uint32_t)db->table_count);
    for (int t = 0; t < db->table_count; t++) {
        const Table* table = &db->tables[t];
        encode_text(image, table->name);
        encode_u32(image, (uint32_t)table->col_count); encode_u32(image, (uint32_t)table->row_count);
        for (int c = 0; c < table->col_count; c++) {
            const ColumnDef* column = &table->columns[c];
            encode_text(image, column->name); encode_u32(image, (uint32_t)column->type);
            encode_u32(image, (uint32_t)column->size);
            encode_u32(image, (column->nullable ? 1u : 0u) | (column->unique ? 2u : 0u) | (column->primary ? 4u : 0u));
            encode_text(image, column->ref_table); encode_text(image, column->ref_column);
            encode_u32(image, (uint32_t)column->fk_on_delete);
            encode_u32(image, (uint32_t)column->fk_on_update);
            encode_text(image, column->constraint_name);
        }
        /* Composite primary key and unique constraints. */
        encode_u32(image, (uint32_t)table->pk_col_count);
        for (int k = 0; k < table->pk_col_count; k++) encode_u32(image, (uint32_t)table->pk_cols[k]);
        encode_text(image, table->pk_name);
        encode_u32(image, (uint32_t)table->uq_count);
        for (int u = 0; u < table->uq_count; u++) {
            encode_u32(image, (uint32_t)table->uq_col_counts[u]);
            for (int k = 0; k < table->uq_col_counts[u]; k++) encode_u32(image, (uint32_t)table->uq_cols[u][k]);
            encode_text(image, table->uq_names[u]);
        }
        for (int r = 0; r < table->row_count; r++)
            for (int c = 0; c < table->col_count; c++) {
                const Value* value = &table->rows[r].values[c];
                encode_u32(image, value->is_null ? 1u : 0u);
                encode_text(image, value->is_null ? "" : value->text);
            }
    }
    if (image->ok) {
        append_snapshot_bytes(image, "USRCAT01", 8);
        encode_u32(image, (uint32_t)db->user_count);
        for (int i = 0; i < db->user_count; i++) {
            const UserAccount* user = &db->users[i];
            encode_text(image, user->name);
            append_snapshot_bytes(image, user->salt, sizeof(user->salt));
            append_snapshot_bytes(image, user->verifier, sizeof(user->verifier));
            encode_u32(image, user->kdf_id);
            encode_u32(image, user->iterations);
            encode_u32(image, (uint32_t)user->role);
            encode_u32(image, user->disabled ? 1u : 0u);
        }
    }
    /* Secondary indexes are part of the durable schema. */
    if (image->ok) {
        append_snapshot_bytes(image, "SECIDX01", 8);
        encode_u32(image, (uint32_t)db->index_count);
        for (int i = 0; i < db->index_count; i++) {
            const SecondaryIndex* idx = &db->indexes[i];
            encode_text(image, idx->name);
            encode_text(image, idx->table_name);
            encode_u32(image, (uint32_t)idx->col_idx);
            encode_u32(image, idx->is_unique ? 1u : 0u);
        }
    }
    /* Per-table grants (optional section; older snapshots have none). */
    if (image->ok) {
        append_snapshot_bytes(image, "TGRANT01", 8);
        encode_u32(image, (uint32_t)db->grant_count);
        for (int i = 0; i < db->grant_count; i++) {
            encode_text(image, db->grants[i].user);
            encode_text(image, db->grants[i].table);
            encode_u32(image, db->grants[i].privileges);
        }
    }
    /* Table partitioning & engine metadata (emitted only when non-default). */
    if (image->ok) {
        bool has_part_or_engine = false;
        for (int t = 0; t < db->table_count; t++) {
            if (db->tables[t].part_type != PART_NONE || db->tables[t].engine_type != ENGINE_MEMORY) {
                has_part_or_engine = true;
                break;
            }
        }
        if (has_part_or_engine) {
            append_snapshot_bytes(image, "PARTIT01", 8);
            for (int t = 0; t < db->table_count; t++) {
                const Table* tbl = &db->tables[t];
                encode_u32(image, (uint32_t)tbl->engine_type);
                encode_u32(image, (uint32_t)tbl->part_type);
                encode_u32(image, (uint32_t)(tbl->part_col_idx >= 0 ? tbl->part_col_idx : 0));
                encode_u32(image, (uint32_t)(tbl->part_count >= 0 ? tbl->part_count : 0));
                for (int p = 0; p < tbl->part_count && p < MAX_PARTITIONS; p++) {
                    encode_text(image, tbl->partitions[p].name);
                    encode_text(image, tbl->partitions[p].upper_bound);
                    encode_u32(image, tbl->partitions[p].is_maxvalue ? 1u : 0u);
                }
            }
        }
    }
    if (image->ok) encode_u32(image, calculate_crc32(image->data, image->size));
    if (image->ok && g_tde_enabled) tde_encrypt_image(image);
    return image->ok;
}

Database* decode_database_snapshot(Image* image, uint64_t* identity, uint64_t* generation) {
    if (image->size < 28) return NULL;
    if (image->size >= 28 && memcmp(image->data, "PCODEN01", 8) == 0) {
        if (!tde_decrypt_image(image)) return NULL;
    }
    if (image->size < 32) return NULL;
    if (memcmp(image->data, "PCOD0001", 8) != 0 &&
        memcmp(image->data, "MSQLMDF1", 8) != 0 &&
        memcmp(image->data, "ORCLDBF1", 8) != 0) {
        return NULL;
    }
    image->offset = image->size - 4; image->ok = true;
    uint32_t stored_crc = decode_u32(image);
    if (stored_crc != calculate_crc32(image->data, image->size - 4)) return NULL;
    image->offset = 8;
    *identity = decode_u64(image); *generation = decode_u64(image);
    uint32_t tables = decode_u32(image);
    if (!*identity || tables > MAX_TABLES) return NULL;
    Database* db = calloc(1, sizeof(*db));
    if (!db) return NULL;
    uint32_t total_rows = 0;
    for (uint32_t t = 0; t < tables && image->ok; t++) {
        Table* table = &db->tables[t];
        db->table_count++;
        decode_text(image, table->name, sizeof(table->name));
        uint32_t columns = decode_u32(image), rows = decode_u32(image);
        if (columns < 1 || columns > MAX_COLUMNS || rows > MAX_ROWS ||
            rows > (uint32_t)MAX_TOTAL_ROWS - total_rows) { image->ok = false; break; }
        total_rows += rows;
        table->col_count = (int)columns;
        for (int c = 0; c < table->col_count && image->ok; c++) {
            ColumnDef* col = &table->columns[c];
            decode_text(image, col->name, sizeof(col->name));
            uint32_t type = decode_u32(image), size = decode_u32(image), flags = decode_u32(image);
            if (type > TYPE_VECTOR || size < 1 || size >= MAX_VAL_LEN || flags > 7) { image->ok = false; break; }
            col->type = (DataType)type; col->size = (int)size;
            col->nullable = (flags & 1) != 0; col->unique = (flags & 2) != 0; col->primary = (flags & 4) != 0;
            if (col->primary && table->pk_col_count < MAX_COLUMNS) {
                table->pk_cols[table->pk_col_count++] = c;
            }
            decode_text(image, col->ref_table, sizeof(col->ref_table));
            decode_text(image, col->ref_column, sizeof(col->ref_column));
            uint32_t fk_del = decode_u32(image), fk_upd = decode_u32(image);
            if (fk_del > FK_ACTION_SET_NULL || fk_upd > FK_ACTION_SET_NULL) { image->ok = false; break; }
            col->fk_on_delete = (ForeignKeyAction)fk_del;
            col->fk_on_update = (ForeignKeyAction)fk_upd;
            decode_text(image, col->constraint_name, sizeof(col->constraint_name));
        }
        if (!image->ok) break;
        /* Composite primary key and unique constraints. */
        {
            uint32_t pk_count = decode_u32(image);
            if (pk_count > MAX_COLUMNS) { image->ok = false; break; }
            table->pk_col_count = 0;
            for (uint32_t k = 0; k < pk_count && image->ok; k++) {
                uint32_t ci = decode_u32(image);
                if (ci >= (uint32_t)table->col_count) { image->ok = false; break; }
                bool dup = false;
                for (int j = 0; j < table->pk_col_count; j++) if (table->pk_cols[j] == (int)ci) dup = true;
                if (dup) { image->ok = false; break; }
                table->pk_cols[table->pk_col_count++] = (int)ci;
            }
            if (!decode_text(image, table->pk_name, sizeof(table->pk_name))) break;
            uint32_t uq_count = decode_u32(image);
            if (uq_count > MAX_COLUMNS) { image->ok = false; break; }
            table->uq_count = 0;
            for (uint32_t u = 0; u < uq_count && image->ok; u++) {
                uint32_t cnt = decode_u32(image);
                if (cnt > MAX_COLUMNS) { image->ok = false; break; }
                table->uq_col_counts[u] = 0;
                for (uint32_t k = 0; k < cnt && image->ok; k++) {
                    uint32_t ci = decode_u32(image);
                    if (ci >= (uint32_t)table->col_count) { image->ok = false; break; }
                    bool dup = false;
                    for (int j = 0; j < table->uq_col_counts[u]; j++) if (table->uq_cols[u][j] == (int)ci) dup = true;
                    if (dup) { image->ok = false; break; }
                    table->uq_cols[u][table->uq_col_counts[u]++] = (int)ci;
                }
                if (!decode_text(image, table->uq_names[u], sizeof(table->uq_names[u]))) break;
                table->uq_count++;
            }
            if (!image->ok) break;
        }
        if ((size_t)rows * columns * 8 > image->size - image->offset) { image->ok = false; break; }
        if (rows) {
            if (!allocate_table_storage(table, (int)rows)) { image->ok = false; break; }
            table->row_count = (int)rows;
        }
        for (int r = 0; r < table->row_count && image->ok; r++)
            for (int c = 0; c < table->col_count && image->ok; c++) {
                Value* value = &table->rows[r].values[c];
                uint32_t flag = decode_u32(image);
                if (flag > 1) { image->ok = false; break; }
                value->is_null = flag == 1;
                decode_text(image, value->text, sizeof(value->text));
                if (value->is_null && value->text[0]) image->ok = false;
            }
    }
    if (image->ok && image->offset < image->size - 4) {
        unsigned char marker[8];
        if (!decode_snapshot_bytes(image, marker, sizeof(marker)) ||
            memcmp(marker, "USRCAT01", sizeof(marker)) != 0) {
            image->ok = false;
        } else {
            uint32_t users = decode_u32(image);
            if (users > MAX_USERS) image->ok = false;
            for (uint32_t i = 0; i < users && image->ok; i++) {
                UserAccount* account = &db->users[i];
                db->user_count++;
                decode_text(image, account->name, sizeof(account->name));
                decode_snapshot_bytes(image, account->salt, sizeof(account->salt));
                decode_snapshot_bytes(image, account->verifier, sizeof(account->verifier));
                account->kdf_id = decode_u32(image);
                account->iterations = decode_u32(image);
                uint32_t role = decode_u32(image);
                uint32_t disabled = decode_u32(image);
                if (!validate_user_name(account->name) ||
                    account->kdf_id != USER_KDF_PBKDF2_SHA256 ||
                    account->iterations < 10000u || account->iterations > 10000000u ||
                    (role != USER_ROLE_ADMIN && role != USER_ROLE_READ_ONLY) || disabled > 1) {
                    image->ok = false;
                    break;
                }
                account->role = (UserRole)role;
                account->disabled = disabled != 0;
                for (uint32_t j = 0; j < i; j++) {
                    if (strcmp(db->users[j].name, account->name) == 0) image->ok = false;
                }
            }
        }
    }
    /* Secondary indexes (optional section for backward compatibility). */
    if (image->ok && image->offset < image->size - 4) {
        unsigned char marker[8];
        if (!decode_snapshot_bytes(image, marker, sizeof(marker)) ||
            memcmp(marker, "SECIDX01", sizeof(marker)) != 0) {
            image->ok = false;
        } else {
            uint32_t indexes = decode_u32(image);
            if (indexes > MAX_SECONDARY_INDEXES) image->ok = false;
            for (uint32_t i = 0; i < indexes && image->ok; i++) {
                char idx_name[MAX_NAME_LEN], tbl_name[MAX_NAME_LEN];
                if (!decode_text(image, idx_name, sizeof(idx_name)) ||
                    !decode_text(image, tbl_name, sizeof(tbl_name))) break;
                uint32_t col_idx = decode_u32(image);
                uint32_t is_unique = decode_u32(image);
                if (is_unique > 1) { image->ok = false; break; }
                int t_idx = find_table_index(db, tbl_name);
                if (t_idx < 0 || col_idx >= (uint32_t)db->tables[t_idx].col_count) {
                    image->ok = false;
                    break;
                }
                if (!execute_create_index_in_db(db, idx_name, tbl_name,
                                                db->tables[t_idx].columns[col_idx].name,
                                                is_unique != 0)) {
                    image->ok = false;
                    break;
                }
            }
        }
    }
    /* Per-table grants (optional section for backward compatibility). */
    if (image->ok && image->offset < image->size - 4) {
        unsigned char marker[8];
        if (!decode_snapshot_bytes(image, marker, sizeof(marker)) ||
            memcmp(marker, "TGRANT01", sizeof(marker)) != 0) {
            image->ok = false;
        } else {
            uint32_t grants = decode_u32(image);
            if (grants > MAX_GRANTS) image->ok = false;
            for (uint32_t i = 0; i < grants && image->ok; i++) {
                TableGrant* grant = &db->grants[db->grant_count];
                if (!decode_text(image, grant->user, sizeof(grant->user)) ||
                    !decode_text(image, grant->table, sizeof(grant->table))) break;
                uint32_t privileges = decode_u32(image);
                if (!image->ok) break;
                if (!validate_user_name(grant->user) ||
                    !is_valid_identifier(grant->table) ||
                    privileges == 0 || privileges > 15u ||
                    find_user_index(db, grant->user) < 0 ||
                    find_table_index(db, grant->table) < 0) {
                    image->ok = false;
                    break;
                }
                for (uint32_t j = 0; j < i; j++) {
                    if (!strcmp(db->grants[j].user, grant->user) &&
                        !strcmp(db->grants[j].table, grant->table)) image->ok = false;
                }
                if (!image->ok) break;
                grant->privileges = privileges;
                db->grant_count++;
            }
        }
    }
    /* Table partitioning & engine metadata (optional section). */
    if (image->ok && image->offset < image->size - 4) {
        unsigned char marker[8];
        if (!decode_snapshot_bytes(image, marker, sizeof(marker)) ||
            memcmp(marker, "PARTIT01", sizeof(marker)) != 0) {
            image->ok = false;
        } else {
            for (int t = 0; t < db->table_count && image->ok; t++) {
                Table* tbl = &db->tables[t];
                uint32_t eng = decode_u32(image);
                uint32_t ptype = decode_u32(image);
                uint32_t pcol = decode_u32(image);
                uint32_t pcnt = decode_u32(image);
                if (eng > ENGINE_COLUMNAR || ptype > PART_RANGE || pcnt > MAX_PARTITIONS) {
                    image->ok = false;
                    break;
                }
                if (ptype != PART_NONE && (pcol >= (uint32_t)tbl->col_count || pcnt == 0)) {
                    image->ok = false;
                    break;
                }
                tbl->engine_type = (StorageEngineType)eng;
                tbl->part_type = (PartitionType)ptype;
                tbl->part_col_idx = (int)pcol;
                tbl->part_count = (int)pcnt;
                for (uint32_t p = 0; p < pcnt && image->ok; p++) {
                    if (!decode_text(image, tbl->partitions[p].name, sizeof(tbl->partitions[p].name)) ||
                        !decode_text(image, tbl->partitions[p].upper_bound, sizeof(tbl->partitions[p].upper_bound))) {
                        image->ok = false;
                        break;
                    }
                    uint32_t maxv = decode_u32(image);
                    if (maxv > 1) { image->ok = false; break; }
                    tbl->partitions[p].is_maxvalue = (maxv == 1);
                }
            }
        }
    }
    if (!image->ok || image->offset != image->size - 4 || !validate_relational_integrity(db)) {
        destroy_database(db); return NULL;
    }
    return db;
}
