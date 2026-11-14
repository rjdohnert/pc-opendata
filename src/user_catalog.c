#include "database_engine.h"

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define MIN_PASSWORD_LENGTH 16u
#define MAX_PASSWORD_LENGTH 255u

static void erase_secret(void* data, size_t length) {
#ifdef _WIN32
    SecureZeroMemory(data, length);
#else
    OPENSSL_cleanse(data, length);
#endif
}

bool validate_user_name(const char* name) {
    if (!name || !name[0] || strlen(name) >= MAX_NAME_LEN) return false;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '\'') return false;
    return true;
}

int find_user_index(const Database* db, const char* name) {
    if (!db || !name) return -1;
    for (int i = 0; i < db->user_count; i++)
        if (strcmp(db->users[i].name, name) == 0) return i;
    return -1;
}

static bool derive_verifier(const char* password, const unsigned char* salt,
                            uint32_t iterations, unsigned char* output) {
    size_t password_length = strlen(password);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                   NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status >= 0) {
        status = BCryptDeriveKeyPBKDF2(algorithm, (PUCHAR)password,
            (ULONG)password_length, (PUCHAR)salt, USER_SALT_SIZE,
            iterations, output, USER_VERIFIER_SIZE, 0);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return status >= 0;
#else
    return PKCS5_PBKDF2_HMAC(password, (int)password_length, salt, USER_SALT_SIZE,
                             (int)iterations, EVP_sha256(), USER_VERIFIER_SIZE,
                             output) == 1;
#endif
}

static bool random_salt(unsigned char* salt) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, salt, USER_SALT_SIZE,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    return RAND_bytes(salt, USER_SALT_SIZE) == 1;
#endif
}

static bool secure_equal(const unsigned char* left, const unsigned char* right, size_t length) {
#ifdef _WIN32
    unsigned char difference = 0;
    for (size_t i = 0; i < length; i++) difference |= left[i] ^ right[i];
    return difference == 0;
#else
    return CRYPTO_memcmp(left, right, length) == 0;
#endif
}

bool set_user_password(UserAccount* account, const char* password) {
    if (!account || !password) return report_database_error("password is required");
    size_t length = strlen(password);
    if (length < MIN_PASSWORD_LENGTH || length > MAX_PASSWORD_LENGTH)
        return report_database_error("password must be 16-255 bytes");
    unsigned char salt[USER_SALT_SIZE], verifier[USER_VERIFIER_SIZE];
    if (!random_salt(salt) || !derive_verifier(password, salt, USER_KDF_ITERATIONS, verifier)) {
        erase_secret(salt, sizeof(salt));
        erase_secret(verifier, sizeof(verifier));
        return report_database_error("password verifier generation failed");
    }
    memcpy(account->salt, salt, sizeof(salt));
    memcpy(account->verifier, verifier, sizeof(verifier));
    account->kdf_id = USER_KDF_PBKDF2_SHA256;
    account->iterations = USER_KDF_ITERATIONS;
    erase_secret(salt, sizeof(salt));
    erase_secret(verifier, sizeof(verifier));
    return true;
}

bool verify_user_password(const UserAccount* account, const char* password) {
    if (!account || !password) return false;
    size_t length = strlen(password);
    bool length_ok = length >= MIN_PASSWORD_LENGTH && length <= MAX_PASSWORD_LENGTH;
    /* Run the KDF regardless of length so the response time does not reveal
       whether the supplied password met the minimum length. */
    if (account->kdf_id != USER_KDF_PBKDF2_SHA256 ||
        account->iterations < 10000u || account->iterations > 10000000u)
        return false;
    unsigned char verifier[USER_VERIFIER_SIZE] = {0};
    bool derived = derive_verifier(password, account->salt, account->iterations, verifier);
    bool matches = length_ok && derived && secure_equal(verifier, account->verifier, sizeof(verifier));
    erase_secret(verifier, sizeof(verifier));
    return matches;
}

bool authenticate_database_user(const Database* db, const char* name, const char* password,
                                UserRole* role) {
    static const unsigned char dummy_salt[USER_SALT_SIZE] = {
        0x72, 0x4f, 0x91, 0x08, 0x3c, 0xb7, 0x5d, 0xe2,
        0x19, 0x66, 0xa0, 0x43, 0xd8, 0x2b, 0x7e, 0xf1
    };
    UserAccount dummy = {0};
    memcpy(dummy.salt, dummy_salt, sizeof(dummy.salt));
    dummy.kdf_id = USER_KDF_PBKDF2_SHA256;
    dummy.iterations = USER_KDF_ITERATIONS;
    int index = find_user_index(db, name);
    const UserAccount* account = index >= 0 ? &db->users[index] : &dummy;
    bool password_ok = verify_user_password(account, password);
    bool accepted = index >= 0 && password_ok && !account->disabled;
    if (accepted && role) *role = account->role;
    erase_secret(&dummy, sizeof(dummy));
    return accepted;
}

static bool parse_user_name(Parser* p, char* output, size_t capacity) {
    if (p->kind != TK_STRING || !validate_user_name(p->text) ||
        strlen(p->text) >= capacity) return false;
    snprintf(output, capacity, "%s", p->text);
    scan_next_token(p);
    return true;
}

static bool parse_role(Parser* p, UserRole* role) {
    if (consume_keyword(p, "ADMIN")) *role = USER_ROLE_ADMIN;
    else if (consume_keyword(p, "READ_ONLY")) *role = USER_ROLE_READ_ONLY;
    else return false;
    return true;
}

static int enabled_admin_count(const Database* db) {
    int count = 0;
    for (int i = 0; i < db->user_count; i++)
        if (!db->users[i].disabled && db->users[i].role == USER_ROLE_ADMIN) count++;
    return count;
}

static bool capture_user_list(const Database* db) {
    if (g_result_capture) {
        QueryResultCapture* capture = g_result_capture;
        capture->col_count = 3;
        snprintf(capture->col_names[0], MAX_NAME_LEN, "USERNAME");
        snprintf(capture->col_names[1], MAX_NAME_LEN, "ROLE");
        snprintf(capture->col_names[2], MAX_NAME_LEN, "STATUS");
        capture->row_count = db->user_count;
        capture->capacity = db->user_count;
        capture->rows = calloc((size_t)db->user_count, sizeof(*capture->rows));
        if (!capture->rows && db->user_count) {
            capture->row_count = capture->capacity = 0;
            return report_database_error("cannot allocate user-list result");
        }
        for (int i = 0; i < db->user_count; i++) {
            snprintf(capture->rows[i][0].text, MAX_VAL_LEN, "%s", db->users[i].name);
            snprintf(capture->rows[i][1].text, MAX_VAL_LEN, "%s",
                     db->users[i].role == USER_ROLE_ADMIN ? "ADMIN" : "READ_ONLY");
            snprintf(capture->rows[i][2].text, MAX_VAL_LEN, "%s",
                     db->users[i].disabled ? "DISABLED" : "ENABLED");
        }
        return true;
    }
    puts("USERNAME\tROLE\tSTATUS");
    for (int i = 0; i < db->user_count; i++)
        printf("%s\t%s\t%s\n", db->users[i].name,
               db->users[i].role == USER_ROLE_ADMIN ? "ADMIN" : "READ_ONLY",
               db->users[i].disabled ? "DISABLED" : "ENABLED");
    return true;
}

static void audit_user_change(const char* operation, const char* target, const char* result) {
    const char* actor = !strcmp(operation, "BOOTSTRAP ADMIN") ? "bootstrap" :
                        g_network_user[0] ? g_network_user : "local-console";
    fprintf(stderr, "AUDIT timestamp=%lld actor=%s operation=%s target=%s result=%s\n",
            (long long)time(NULL), actor, operation,
            target && target[0] ? target : "(unknown)", result);
}

bool execute_user_catalog_statement(Parser* p, const char* command) {
    if (!g_db) return report_database_error("not connected to any database");

    if (!strcmp(command, "SHOW")) {
        if (!consume_keyword(p, "USERS") || !consume_statement_end(p))
            return report_database_error("expected SHOW USERS");
        return capture_user_list(g_db);
    }

    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: user administration requires the ADMIN role");

    if (g_readonly || g_raft_role == RAFT_FOLLOWER)
        return report_database_error("user catalog changes are unavailable in read-only mode");
    if (g_io_uncertain || atomic_load(&g_backup_failed))
        return report_database_error("writes blocked by storage/backup failure");

    Database* candidate = copy_database(g_db);
    if (!candidate) return report_database_error("unable to allocate user catalog transaction");
    char name[MAX_NAME_LEN] = {0};
    char password[MAX_VAL_LEN] = {0};
    char operation[32] = "USER CATALOG";
    int index = -1;
    bool ok = false;
    int before_errors = g_errors;

    if (!strcmp(command, "CREATE")) {
        snprintf(operation, sizeof(operation), "CREATE USER");
        UserRole role;
        if (consume_keyword(p, "USER") && parse_user_name(p, name, sizeof(name)) &&
            consume_keyword(p, "IDENTIFIED") && consume_keyword(p, "BY")) {
            bool has_pw = false;
            if (consume_keyword(p, "PROMPT")) {
                if (g_network_user[0]) {
                    destroy_database(candidate);
                    return report_database_error("password PROMPT is not supported in network sessions");
                }
                char pbuf[MAX_VAL_LEN];
                if (read_masked_password("Enter password: ", pbuf, sizeof(pbuf))) {
                    snprintf(password, sizeof(password), "%s", pbuf);
                    has_pw = true;
                }
            } else if (p->kind == TK_STRING && strlen(p->text) < sizeof(password)) {
                memcpy(password, p->text, strlen(p->text) + 1);
                scan_next_token(p);
                has_pw = true;
            }
            if (has_pw) {
                ok = consume_keyword(p, "ROLE") && parse_role(p, &role) &&
                     consume_statement_end(p);
                if (ok && find_user_index(candidate, name) >= 0) {
                    ok = report_database_error("user already exists");
                } else if (ok && candidate->user_count >= MAX_USERS) {
                    ok = report_database_error("user catalog limit reached");
                } else if (ok) {
                    UserAccount* account = &candidate->users[candidate->user_count];
                    memset(account, 0, sizeof(*account));
                    snprintf(account->name, sizeof(account->name), "%s", name);
                    account->role = role;
                    ok = set_user_password(account, password);
                    if (ok) candidate->user_count++;
                }
            }
        }
    } else if (!strcmp(command, "ALTER")) {
        snprintf(operation, sizeof(operation), "ALTER USER");
        if (consume_keyword(p, "USER") && parse_user_name(p, name, sizeof(name))) {
            index = find_user_index(candidate, name);
            if (index < 0) {
                ok = report_database_error("user does not exist");
            } else if (consume_keyword(p, "PASSWORD") || (consume_keyword(p, "IDENTIFIED") && consume_keyword(p, "BY"))) {
                snprintf(operation, sizeof(operation), "ALTER USER PASSWORD");
                bool has_pw = false;
                if (consume_keyword(p, "PROMPT")) {
                    if (g_network_user[0]) {
                        destroy_database(candidate);
                        return report_database_error("password PROMPT is not supported in network sessions");
                    }
                    char pbuf[MAX_VAL_LEN];
                    if (read_masked_password("Enter new password: ", pbuf, sizeof(pbuf))) {
                        snprintf(password, sizeof(password), "%s", pbuf);
                        has_pw = true;
                    }
                } else if (p->kind == TK_STRING && strlen(p->text) < sizeof(password)) {
                    memcpy(password, p->text, strlen(p->text) + 1);
                    scan_next_token(p);
                    has_pw = true;
                }
                if (has_pw) {
                    ok = consume_statement_end(p) &&
                         set_user_password(&candidate->users[index], password);
                }
            } else if (consume_keyword(p, "ROLE")) {
                snprintf(operation, sizeof(operation), "ALTER USER ROLE");
                UserRole role;
                ok = parse_role(p, &role) && consume_statement_end(p);
                if (ok) candidate->users[index].role = role;
            } else if (consume_keyword(p, "DISABLE")) {
                snprintf(operation, sizeof(operation), "DISABLE USER");
                ok = consume_statement_end(p);
                if (ok) candidate->users[index].disabled = true;
            } else if (consume_keyword(p, "ENABLE")) {
                snprintf(operation, sizeof(operation), "ENABLE USER");
                ok = consume_statement_end(p);
                if (ok) candidate->users[index].disabled = false;
            }
            if (ok && enabled_admin_count(candidate) == 0)
                ok = report_database_error("the last enabled administrator cannot be disabled or demoted");
        }
    } else if (!strcmp(command, "DROP")) {
        snprintf(operation, sizeof(operation), "DROP USER");
        if (consume_keyword(p, "USER") && parse_user_name(p, name, sizeof(name)) &&
            consume_statement_end(p)) {
            index = find_user_index(candidate, name);
            if (index < 0) ok = report_database_error("user does not exist");
            else {
                memmove(&candidate->users[index], &candidate->users[index + 1],
                        (size_t)(candidate->user_count - index - 1) * sizeof(UserAccount));
                candidate->user_count--;
                memset(&candidate->users[candidate->user_count], 0, sizeof(UserAccount));
                ok = enabled_admin_count(candidate) > 0;
                if (!ok) report_database_error("the last enabled administrator cannot be dropped");
            }
        }
    }

    erase_secret(password, sizeof(password));
    if (!ok) {
        audit_user_change(operation, name, "FAILED");
        erase_secret(candidate->users, sizeof(candidate->users));
        destroy_database(candidate);
        if (g_errors == before_errors)
            report_database_error("invalid or unsupported %s USER syntax", command);
        return false;
    }

    destroy_database(g_db);
    g_db = candidate;
    g_pending = true;
    printf("User catalog updated.\n");
    if (g_autocommit) {
        bool committed = commit_transaction();
        audit_user_change(operation, name, committed ? "COMMITTED" : "COMMIT_FAILED");
        return committed;
    }
    audit_user_change(operation, name, "STAGED");
    return true;
}

static int find_grant_index(const Database* db, const char* user, const char* table) {
    for (int i = 0; i < db->grant_count; i++)
        if (!strcmp(db->grants[i].user, user) && !strcmp(db->grants[i].table, table)) return i;
    return -1;
}

bool user_has_table_privilege(const Database* db, const char* user, const char* table, uint32_t privilege) {
    if (!db || !user || !user[0] || !table) return true;
    if (g_network_user_role == USER_ROLE_ADMIN) return true;
    int idx = find_user_index(db, user);
    if (idx >= 0 && db->users[idx].role == USER_ROLE_ADMIN) return true;
    int grant = find_grant_index(db, user, table);
    return grant >= 0 && (db->grants[grant].privileges & privilege) == privilege;
}

static bool capture_grant_list(const Database* db) {
    if (g_result_capture) {
        QueryResultCapture* capture = g_result_capture;
        capture->col_count = 3;
        snprintf(capture->col_names[0], MAX_NAME_LEN, "USERNAME");
        snprintf(capture->col_names[1], MAX_NAME_LEN, "TABLE_NAME");
        snprintf(capture->col_names[2], MAX_NAME_LEN, "PRIVILEGES");
        capture->row_count = db->grant_count;
        capture->capacity = db->grant_count;
        capture->rows = calloc((size_t)(db->grant_count ? db->grant_count : 1), sizeof(*capture->rows));
        if (!capture->rows) {
            capture->row_count = capture->capacity = 0;
            return report_database_error("cannot allocate grant-list result");
        }
        for (int i = 0; i < db->grant_count; i++) {
            snprintf(capture->rows[i][0].text, MAX_VAL_LEN, "%s", db->grants[i].user);
            snprintf(capture->rows[i][1].text, MAX_VAL_LEN, "%s", db->grants[i].table);
            char privs[64] = {0};
            uint32_t p = db->grants[i].privileges;
            if (p & PRIV_SELECT) strcat(privs, "SELECT ");
            if (p & PRIV_INSERT) strcat(privs, "INSERT ");
            if (p & PRIV_UPDATE) strcat(privs, "UPDATE ");
            if (p & PRIV_DELETE) strcat(privs, "DELETE");
            size_t len = strlen(privs);
            while (len > 0 && privs[len - 1] == ' ') privs[--len] = '\0';
            snprintf(capture->rows[i][2].text, MAX_VAL_LEN, "%s", privs);
        }
        return true;
    }
    puts("USERNAME\tTABLE_NAME\tPRIVILEGES");
    for (int i = 0; i < db->grant_count; i++) {
        char privs[64] = {0};
        uint32_t p = db->grants[i].privileges;
        if (p & PRIV_SELECT) strcat(privs, "SELECT ");
        if (p & PRIV_INSERT) strcat(privs, "INSERT ");
        if (p & PRIV_UPDATE) strcat(privs, "UPDATE ");
        if (p & PRIV_DELETE) strcat(privs, "DELETE");
        size_t len = strlen(privs);
        while (len > 0 && privs[len - 1] == ' ') privs[--len] = '\0';
        printf("%s\t%s\t%s\n", db->grants[i].user, db->grants[i].table, privs);
    }
    if (db->grant_count == 0)
        puts("-- No table grants. Network READ_ONLY users have no table access until you GRANT privileges.");
    return true;
}

bool execute_grant_statement(Parser* p, bool granting) {
    if (!g_db) return report_database_error("not connected to any database");
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: GRANT/REVOKE requires the ADMIN role");
    if (g_readonly || g_raft_role == RAFT_FOLLOWER)
        return report_database_error("grant changes are unavailable in read-only mode");
    if (g_io_uncertain || atomic_load(&g_backup_failed))
        return report_database_error("writes blocked by storage/backup failure");

    uint32_t privileges = 0;
    if (granting) {
        if (consume_keyword(p, "ALL")) {
            privileges = PRIV_SELECT | PRIV_INSERT | PRIV_UPDATE | PRIV_DELETE;
        } else {
            do {
                if (consume_keyword(p, "SELECT")) privileges |= PRIV_SELECT;
                else if (consume_keyword(p, "INSERT")) privileges |= PRIV_INSERT;
                else if (consume_keyword(p, "UPDATE")) privileges |= PRIV_UPDATE;
                else if (consume_keyword(p, "DELETE")) privileges |= PRIV_DELETE;
                else return report_database_error("expected SELECT, INSERT, UPDATE, DELETE, or ALL after GRANT");
            } while (consume_token(p, TK_COMMA));
        }
        if (!consume_keyword(p, "ON")) return report_database_error("expected ON <table> in GRANT");
    } else {
        if (!consume_keyword(p, "ON")) return report_database_error("expected REVOKE ON <table> FROM <user>");
    }

    char table[MAX_NAME_LEN], user[MAX_NAME_LEN];
    if (!parse_identifier(p, table)) return report_database_error("expected table name after ON");
    if (find_table_index(g_db, table) < 0)
        return report_database_error("table does not exist: %s", table);
    if (!consume_keyword(p, granting ? "TO" : "FROM"))
        return report_database_error("expected %s '<user>'", granting ? "TO" : "FROM");
    if (!parse_user_name(p, user, sizeof(user)))
        return report_database_error("expected user name string after %s", granting ? "TO" : "FROM");
    if (!consume_statement_end(p)) return false;
    int user_idx = find_user_index(g_db, user);
    if (user_idx < 0) return report_database_error("user does not exist: %s", user);
    if (g_db->users[user_idx].role == USER_ROLE_ADMIN)
        return report_database_error("ADMIN users already have full table access; grants apply to READ_ONLY users");

    Database* candidate = copy_database(g_db);
    if (!candidate) return report_database_error("unable to allocate grant transaction");
    int grant_idx = find_grant_index(candidate, user, table);
    if (granting) {
        if (grant_idx < 0) {
            if (candidate->grant_count >= MAX_GRANTS) {
                destroy_database(candidate);
                return report_database_error("grant limit reached (%d)", MAX_GRANTS);
            }
            grant_idx = candidate->grant_count++;
            snprintf(candidate->grants[grant_idx].user, MAX_NAME_LEN, "%s", user);
            snprintf(candidate->grants[grant_idx].table, MAX_NAME_LEN, "%s", table);
            candidate->grants[grant_idx].privileges = 0;
        }
        candidate->grants[grant_idx].privileges |= privileges;
    } else {
        if (grant_idx < 0) {
            destroy_database(candidate);
            return report_database_error("no grant exists for %s on %s", user, table);
        }
        memmove(&candidate->grants[grant_idx], &candidate->grants[grant_idx + 1],
                (size_t)(candidate->grant_count - grant_idx - 1) * sizeof(TableGrant));
        candidate->grant_count--;
        memset(&candidate->grants[candidate->grant_count], 0, sizeof(TableGrant));
    }

    destroy_database(g_db);
    g_db = candidate;
    g_pending = true;
    printf(granting ? "Privileges granted.\n" : "Privileges revoked.\n");
    audit_user_change(granting ? "GRANT" : "REVOKE", user, "STAGED");
    if (g_autocommit) {
        bool committed = commit_transaction();
        audit_user_change(granting ? "GRANT" : "REVOKE", user,
                          committed ? "COMMITTED" : "COMMIT_FAILED");
        return committed;
    }
    return true;
}

bool execute_show_grants(void) {
    if (!g_db) return report_database_error("not connected to any database");
    return capture_grant_list(g_db);
}

bool create_bootstrap_admin(const char* name, const char* password) {
    if (!g_db || !g_committed) return report_database_error("database is not initialized");
    if (g_committed->user_count) return true;
    if (!validate_user_name(name)) return report_database_error("invalid bootstrap administrator name");
    if (g_readonly) return report_database_error("cannot bootstrap users in read-only mode");
    Database* candidate = copy_database(g_db);
    if (!candidate) return report_database_error("unable to allocate bootstrap account");
    UserAccount* account = &candidate->users[0];
    memset(account, 0, sizeof(*account));
    snprintf(account->name, sizeof(account->name), "%s", name);
    account->role = USER_ROLE_ADMIN;
    if (!set_user_password(account, password)) {
        audit_user_change("BOOTSTRAP ADMIN", name, "FAILED");
        erase_secret(candidate->users, sizeof(candidate->users));
        destroy_database(candidate);
        return false;
    }
    candidate->user_count = 1;
    destroy_database(g_db);
    g_db = candidate;
    g_pending = true;
    if (!commit_transaction()) {
        audit_user_change("BOOTSTRAP ADMIN", name, "COMMIT_FAILED");
        rollback_transaction();
        return false;
    }
    audit_user_change("BOOTSTRAP ADMIN", name, "COMMITTED");
    return true;
}
