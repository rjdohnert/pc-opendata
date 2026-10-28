#include "database_engine.h"

#ifdef _WIN32
#include <wincrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef OPENDATA_TEST_TLS_FIXTURES
#include "../tests/cluster_tls_test_fixture.h"
#endif
#else
#include <sys/stat.h>
#ifndef S_ISVTX
#define S_ISVTX 01000
#endif
#if defined(OPENDATA_TLS)
#include <arpa/inet.h>
#include <openssl/buffer.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#endif
#endif

#define WALLET_CONFIG_NAME "opendata-wallet.conf"
#define WALLET_LINE_LIMIT 1024

static char* trim_space(char* text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = '\0';
    return text;
}

static int read_wallet_line(FILE* file, char* buffer, size_t capacity) {
    size_t length = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF && ch != '\n') {
        if (ch == '\0' || length + 1 >= capacity) return -1;
        buffer[length++] = (char)ch;
    }
    if (ch == EOF && ferror(file)) return -1;
    if (ch == EOF && length == 0) return 0;
    buffer[length] = '\0';
    return 1;
}

static bool has_parent_path_segment(const char* path) {
    const char* segment = path;
    for (const char* current = path;; current++) {
        if (*current == '/' || *current == '\\' || *current == '\0') {
            if (current - segment == 2 && segment[0] == '.' && segment[1] == '.')
                return true;
            if (!*current) return false;
            segment = current + 1;
        }
    }
}

static bool copy_wallet_path(const char* root, const char* value, char* output) {
    if (!value[0] || value[0] == '/' || value[0] == '\\' ||
        (is_ascii_digit(value[0]) && value[1] == ':') ||
        has_parent_path_segment(value) || strchr(value, ':'))
        return false;
    for (const unsigned char* p = (const unsigned char*)value; *p; p++)
        if (*p < 32 || *p == '"') return false;
    char joined[PATH_CAP];
#ifdef _WIN32
    int length = snprintf(joined, sizeof(joined), "%s\\%s", root, value);
#else
    int length = snprintf(joined, sizeof(joined), "%s/%s", root, value);
#endif
    return length > 0 && (size_t)length < sizeof(joined) &&
           resolve_absolute_path(joined, output);
}

#ifndef _WIN32
static bool wallet_trusted_owner(const struct stat* metadata) {
    return metadata->st_uid == geteuid() || metadata->st_uid == 0;
}

static int wallet_open_posix_path(const char* path, bool directory, bool private_key) {
    if (path[0] != '/' || strlen(path) >= PATH_CAP || has_parent_path_segment(path)) {
        report_database_error("wallet paths must be absolute without parent traversal");
        return -1;
    }
    int parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) {
        report_database_error("cannot open wallet filesystem root");
        return -1;
    }
    char remaining[PATH_CAP];
    memcpy(remaining, path + 1, strlen(path));
    char* state = NULL;
    char* component = strtok_r(remaining, "/", &state);
    bool valid = true;
    while (component) {
        char* next = strtok_r(NULL, "/", &state);
        bool is_directory = next || directory;
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        if (is_directory) flags |= O_DIRECTORY;
        int opened = openat(parent, component, flags);
        struct stat metadata;
        valid = opened >= 0 && fstat(opened, &metadata) == 0 &&
                wallet_trusted_owner(&metadata) &&
                (is_directory ? S_ISDIR(metadata.st_mode) : S_ISREG(metadata.st_mode));
        if (valid) {
            bool writable = (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0;
            /* A root-owned sticky ancestor (e.g. /tmp) cannot replace our owned child. */
            bool sticky_ancestor = next && metadata.st_uid == 0 &&
                                   (metadata.st_mode & S_ISVTX);
            valid = (!writable || sticky_ancestor) &&
                    (next || !private_key || !(metadata.st_mode & (S_IRWXG | S_IRWXO)));
        }
        close(parent);
        parent = opened;
        if (!valid) break;
        component = next;
    }
    if (!valid) {
        if (parent >= 0) close(parent);
        report_database_error("unsafe wallet path: %s (require trusted ownership, protected permissions, regular files, and no symlinks; private keys must be owner-only)", path);
        return -1;
    }
    return parent;
}

static bool wallet_validate_posix_file(const char* path, bool private_key) {
    if (!path[0]) return true;
    int file = wallet_open_posix_path(path, false, private_key);
    if (file < 0) return false;
    close(file);
    return true;
}

#if defined(OPENDATA_TLS)
static int wallet_no_password(char* buffer, int size, int writing, void* context) {
    (void)buffer;
    (void)size;
    (void)writing;
    (void)context;
    return 0;
}
#endif
#endif

static bool valid_thumbprint(const char* value) {
    if (strlen(value) != 40) return false;
    for (size_t i = 0; i < 40; i++)
        if (!is_ascii_digit(value[i]) &&
            (uppercase_ascii(value[i]) < 'A' || uppercase_ascii(value[i]) > 'F'))
            return false;
    return true;
}

#ifdef _WIN32
static bool validate_store_certificate(const char* thumbprint, bool cluster_identity) {
    BYTE hash[20];
    for (size_t i = 0; i < sizeof(hash); i++) {
        unsigned char high = (unsigned char)uppercase_ascii(thumbprint[i * 2]);
        unsigned char low = (unsigned char)uppercase_ascii(thumbprint[i * 2 + 1]);
        hash[i] = (BYTE)(((high <= '9' ? high - '0' : high - 'A' + 10) << 4) |
                         (low <= '9' ? low - '0' : low - 'A' + 10));
    }
#ifdef OPENDATA_TEST_TLS_FIXTURES
    OpendataTestTlsFixture fixture = {0};
    HCERTSTORE store = opendata_test_tls_fixture_wallet_store(&fixture);
#else
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG,
                                     L"MY");
#endif
    if (!store) return report_database_error("cannot open CurrentUser\\My for wallet validation");
    CRYPT_HASH_BLOB hash_blob = {(DWORD)sizeof(hash), hash};
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HASH, &hash_blob, NULL);
    bool valid = certificate != NULL;
    if (!valid) {
#ifdef OPENDATA_TEST_TLS_FIXTURES
        opendata_test_tls_fixture_close(&fixture);
#else
        CertCloseStore(store, 0);
#endif
        return report_database_error("wallet certificate thumbprint was not found in CurrentUser\\My");
    }
    if (CertVerifyTimeValidity(NULL, certificate->pCertInfo) != 0) valid = false;
    if (valid) {
        DWORD usage_size = 0;
        bool has_usage = CertGetEnhancedKeyUsage(certificate, 0, NULL, &usage_size) != FALSE;
        if (!has_usage) valid = false;
        if (has_usage && usage_size) {
            PCERT_ENHKEY_USAGE usage = (PCERT_ENHKEY_USAGE)malloc(usage_size);
            if (!usage || !CertGetEnhancedKeyUsage(certificate, 0, usage, &usage_size)) {
                free(usage);
                valid = false;
            } else {
                bool server_auth = false, client_auth = false;
                for (DWORD i = 0; i < usage->cUsageIdentifier; i++) {
                    if (!strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH))
                        server_auth = true;
                    if (!strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_CLIENT_AUTH))
                        client_auth = true;
                }
                valid = cluster_identity ? server_auth && client_auth : server_auth;
                free(usage);
            }
        }
        if (valid) {
            DWORD key_info_size = 0;
            valid = CertGetCertificateContextProperty(
                certificate, CERT_KEY_PROV_INFO_PROP_ID, NULL, &key_info_size) != FALSE;
        }
    }
    CertFreeCertificateContext(certificate);
#ifdef OPENDATA_TEST_TLS_FIXTURES
    opendata_test_tls_fixture_close(&fixture);
#else
    CertCloseStore(store, 0);
#endif
    if (!valid)
        return report_database_error("wallet certificate is expired, lacks required EKU, or has no associated private key");
    return true;
}
#endif

static bool parse_wallet_field(TlsWallet* wallet, const char* root,
                               const char* key, const char* value) {
    if (!strcmp(key, "version")) return !strcmp(value, "1");
    if (!strcmp(key, "server.cert")) return copy_wallet_path(root, value, wallet->server_cert);
    if (!strcmp(key, "server.key")) return copy_wallet_path(root, value, wallet->server_key);
    if (!strcmp(key, "cluster.cert")) return copy_wallet_path(root, value, wallet->cluster_cert);
    if (!strcmp(key, "cluster.key")) return copy_wallet_path(root, value, wallet->cluster_key);
    if (!strcmp(key, "cluster.ca")) return copy_wallet_path(root, value, wallet->cluster_ca);
    if (!strcmp(key, "server.store")) {
        if (strcmp(value, "CurrentUser\\My")) return false;
        snprintf(wallet->server_store, sizeof(wallet->server_store), "%s", value);
        return true;
    }
    if (!strcmp(key, "server.thumbprint")) {
        if (!valid_thumbprint(value)) return false;
        snprintf(wallet->server_thumbprint, sizeof(wallet->server_thumbprint), "%s", value);
        return true;
    }
    if (!strcmp(key, "cluster.store")) {
        if (strcmp(value, "CurrentUser\\My")) return false;
        snprintf(wallet->cluster_store, sizeof(wallet->cluster_store), "%s", value);
        return true;
    }
    if (!strcmp(key, "cluster.thumbprint")) {
        if (!valid_thumbprint(value)) return false;
        snprintf(wallet->cluster_thumbprint, sizeof(wallet->cluster_thumbprint), "%s", value);
        return true;
    }
    if (!strcmp(key, "cluster.trust_store")) {
        if (strcmp(value, "CurrentUser\\Root")) return false;
        snprintf(wallet->cluster_trust_store, sizeof(wallet->cluster_trust_store), "%s", value);
        return true;
    }
    return false;
}

static bool validate_identity_files(const char* cert, const char* key, const char* ca,
                                    const char* purpose) {
    if (!!cert[0] != !!key[0]) {
        return report_database_error("wallet %s identity requires both certificate and key", purpose);
    }
    if (!cert[0]) {
        if (!ca[0]) return true;
#ifdef _WIN32
        return report_database_error("PEM CA bundles are unsupported on Windows wallets");
#else
#if defined(OPENDATA_TLS)
        X509_STORE* store = X509_STORE_new();
        bool ca_valid = store && X509_STORE_load_locations(store, ca, NULL) == 1;
        X509_STORE_free(store);
        if (!ca_valid) return report_database_error("wallet %s CA bundle is missing or invalid", purpose);
#else
        FILE* ca_file = fopen(ca, "rb");
        if (!ca_file) return report_database_error("wallet %s CA bundle cannot be opened", purpose);
        fclose(ca_file);
#endif
        return true;
#endif
    }
#ifdef _WIN32
    (void)ca;
    return report_database_error("wallet PEM identity files are unsupported on Windows; use store references");
#else
#if defined(OPENDATA_TLS)
    BIO* cert_bio = BIO_new_file(cert, "r");
    BIO* key_bio = BIO_new_file(key, "r");
    X509* certificate = cert_bio ? PEM_read_bio_X509(cert_bio, NULL, NULL, NULL) : NULL;
    EVP_PKEY* private_key = key_bio ?
        PEM_read_bio_PrivateKey(key_bio, NULL, wallet_no_password, NULL) : NULL;
    int not_before = certificate ? X509_cmp_current_time(X509_get0_notBefore(certificate)) : 0;
    int not_after = certificate ? X509_cmp_current_time(X509_get0_notAfter(certificate)) : 0;
    bool valid = certificate && private_key && not_before < 0 && not_after > 0 &&
                 X509_check_private_key(certificate, private_key) == 1;
    if (valid) {
        int usage_critical = -1;
        EXTENDED_KEY_USAGE* usage = X509_get_ext_d2i(
            certificate, NID_ext_key_usage, &usage_critical, NULL);
        bool server_auth = false, client_auth = false;
        if (usage) {
            for (int i = 0; i < sk_ASN1_OBJECT_num(usage); i++) {
                int usage_nid = OBJ_obj2nid(sk_ASN1_OBJECT_value(usage, i));
                if (usage_nid == NID_server_auth) server_auth = true;
                if (usage_nid == NID_client_auth) client_auth = true;
            }
            EXTENDED_KEY_USAGE_free(usage);
        }
        if (!usage && usage_critical == -1 &&
            X509_get_ext_by_NID(certificate, NID_ext_key_usage, -1) < 0)
            server_auth = true, client_auth = true;
        valid = !strcmp(purpose, "server") ? server_auth : server_auth && client_auth;
    }
    if (valid && ca[0]) {
        X509_STORE* store = X509_STORE_new();
        X509_STORE_CTX* verification = X509_STORE_CTX_new();
        STACK_OF(X509)* intermediates = sk_X509_new_null();
        bool chain_valid = store && verification && intermediates &&
                           X509_STORE_load_locations(store, ca, NULL) == 1;
        X509* intermediate;
        while (chain_valid &&
               (intermediate = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL)) != NULL) {
            if (!sk_X509_push(intermediates, intermediate)) {
                X509_free(intermediate);
                chain_valid = false;
            }
        }
        chain_valid = chain_valid &&
                      X509_STORE_CTX_init(verification, store, certificate, intermediates) == 1 &&
                      X509_verify_cert(verification) == 1;
        sk_X509_pop_free(intermediates, X509_free);
        X509_STORE_CTX_free(verification);
        X509_STORE_free(store);
        if (!chain_valid) {
            EVP_PKEY_free(private_key);
            X509_free(certificate);
            BIO_free(key_bio);
            BIO_free(cert_bio);
            return report_database_error("wallet %s certificate does not chain to its CA bundle", purpose);
        }
    }
    EVP_PKEY_free(private_key);
    X509_free(certificate);
    BIO_free(key_bio);
    BIO_free(cert_bio);
    if (!valid)
        return report_database_error("wallet %s certificate is invalid, expired, lacks required EKU, or does not match its private key", purpose);
#else
    return report_database_error("wallet certificate inspection requires a TLS-enabled build");
#endif
    if (ca[0]) {
#if defined(OPENDATA_TLS)
        X509_STORE* store = X509_STORE_new();
        bool ca_valid = store && X509_STORE_load_locations(store, ca, NULL) == 1;
        X509_STORE_free(store);
        if (!ca_valid) return report_database_error("wallet %s CA bundle is missing or invalid", purpose);
#else
        return report_database_error("wallet CA validation requires a TLS-enabled build");
#endif
    }
    return true;
#endif
}

bool load_tls_wallet(const char* directory, TlsWallet* wallet) {
    if (!directory || !wallet) return report_database_error("wallet path and output are required");
    memset(wallet, 0, sizeof(*wallet));
    if (!resolve_absolute_path(directory, wallet->directory))
        return report_database_error("invalid wallet directory path");
#ifdef _WIN32
    DWORD directory_attributes = GetFileAttributesA(wallet->directory);
    if (directory_attributes == INVALID_FILE_ATTRIBUTES ||
        !(directory_attributes & FILE_ATTRIBUTE_DIRECTORY))
        return report_database_error("wallet path is not a directory");
#else
    int directory_fd = wallet_open_posix_path(wallet->directory, true, false);
    if (directory_fd < 0) return false;
    close(directory_fd);
#endif
    char manifest[PATH_CAP];
#ifdef _WIN32
    int path_length = snprintf(manifest, sizeof(manifest), "%s\\%s",
                               wallet->directory, WALLET_CONFIG_NAME);
#else
    int path_length = snprintf(manifest, sizeof(manifest), "%s/%s",
                               wallet->directory, WALLET_CONFIG_NAME);
#endif
    if (path_length < 0 || (size_t)path_length >= sizeof(manifest))
        return report_database_error("wallet manifest path is too long");
#ifdef _WIN32
    FILE* file = fopen(manifest, "rb");
#else
    int manifest_fd = wallet_open_posix_path(manifest, false, false);
    if (manifest_fd < 0) return false;
    FILE* file = fdopen(manifest_fd, "rb");
    if (!file) close(manifest_fd);
#endif
    if (!file) return report_database_error("cannot open wallet manifest: %s", manifest);
    char line[WALLET_LINE_LIMIT];
    bool seen_version = false;
    bool seen_fields[11] = {false};
    bool ok = true;
    unsigned line_number = 0;
    int read_status;
    while ((read_status = read_wallet_line(file, line, sizeof(line))) > 0) {
        line_number++;
        char* field = trim_space(line);
        if (!field[0] || field[0] == '#') continue;
        char* equals = strchr(field, '=');
        if (!equals || equals == field || strchr(equals + 1, '=')) {
            ok = false;
            break;
        }
        *equals++ = '\0';
        char* key = trim_space(field);
        char* value = trim_space(equals);
        const char* keys[] = {"version", "server.cert", "server.key",
            "cluster.cert", "cluster.key", "cluster.ca", "server.store",
            "server.thumbprint", "cluster.store", "cluster.thumbprint",
            "cluster.trust_store"};
        size_t key_count = sizeof(keys) / sizeof(keys[0]);
        size_t index = 0;
        while (index < key_count && strcmp(keys[index], key)) index++;
        if (index == key_count || seen_fields[index] || !value[0] ||
            !parse_wallet_field(wallet, wallet->directory, key, value)) {
            ok = false;
            break;
        }
        seen_fields[index] = true;
        if (index == 0) seen_version = true;
    }
    if (read_status < 0 || ferror(file)) ok = false;
    fclose(file);
    if (!ok || !seen_version)
        return report_database_error("invalid wallet manifest at line %u (unknown, duplicate, or malformed field)", line_number);
#ifdef _WIN32
    if ((wallet->server_cert[0] || wallet->server_key[0]) ||
        (wallet->cluster_cert[0] || wallet->cluster_key[0]) ||
        (!!wallet->server_store[0] != !!wallet->server_thumbprint[0]) ||
        (!!wallet->cluster_store[0] != !!wallet->cluster_thumbprint[0]) ||
        (!!wallet->cluster_thumbprint[0] != !!wallet->cluster_trust_store[0]))
        return report_database_error("Windows wallet requires paired store and thumbprint fields; PEM identities are unsupported");
    if (wallet->server_thumbprint[0] &&
        !validate_store_certificate(wallet->server_thumbprint, false)) return false;
    if (wallet->cluster_thumbprint[0] &&
        !validate_store_certificate(wallet->cluster_thumbprint, true)) return false;
#else
    if (wallet->server_store[0] || wallet->cluster_store[0] ||
        wallet->server_thumbprint[0] || wallet->cluster_thumbprint[0] ||
        wallet->cluster_trust_store[0])
        return report_database_error("Windows certificate-store fields are invalid on this platform");
    if (!wallet_validate_posix_file(wallet->server_cert, false) ||
        !wallet_validate_posix_file(wallet->server_key, true) ||
        !wallet_validate_posix_file(wallet->cluster_cert, false) ||
        !wallet_validate_posix_file(wallet->cluster_key, true) ||
        !wallet_validate_posix_file(wallet->cluster_ca, false))
        return false;
#endif
    if (!validate_identity_files(wallet->server_cert, wallet->server_key,
                                 "", "server") ||
        !validate_identity_files(wallet->cluster_cert, wallet->cluster_key,
                                 wallet->cluster_ca, "cluster"))
        return false;
    if (!wallet->server_cert[0] && !wallet->server_thumbprint[0] &&
        !wallet->cluster_cert[0] && !wallet->cluster_thumbprint[0])
        return report_database_error("wallet contains no TLS identity");
    return true;
}

#if defined(_WIN32) || defined(OPENDATA_TLS)
static void print_escaped(const unsigned char* text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = text[i];
        if (ch >= 32 && ch <= 126 && ch != '\\') putchar(ch);
        else printf("\\x%02X", ch);
    }
}
#endif

#ifdef _WIN32
static bool print_wide_escaped(const wchar_t* text) {
    int byte_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                         NULL, 0, NULL, NULL);
    if (byte_count <= 0) return false;
    if (byte_count == 1) return true;
    char* utf8 = malloc((size_t)byte_count);
    if (!utf8) return false;
    bool valid = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                     utf8, byte_count, NULL, NULL) > 0;
    if (valid)
        print_escaped((const unsigned char*)utf8, strlen(utf8));
    free(utf8);
    return valid;
}

static bool inspect_store_certificate(const char* thumbprint) {
    BYTE hash[20];
    for (size_t i = 0; i < sizeof(hash); i++) {
        unsigned char high = (unsigned char)uppercase_ascii(thumbprint[i * 2]);
        unsigned char low = (unsigned char)uppercase_ascii(thumbprint[i * 2 + 1]);
        hash[i] = (BYTE)(((high <= '9' ? high - '0' : high - 'A' + 10) << 4) |
                         (low <= '9' ? low - '0' : low - 'A' + 10));
    }
#ifdef OPENDATA_TEST_TLS_FIXTURES
    OpendataTestTlsFixture fixture = {0};
    HCERTSTORE store = opendata_test_tls_fixture_wallet_store(&fixture);
#else
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG,
                                     L"MY");
#endif
    if (!store) return report_database_error("cannot open CurrentUser\\My for wallet inspection");
    CRYPT_HASH_BLOB hash_blob = {(DWORD)sizeof(hash), hash};
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HASH, &hash_blob, NULL);
#ifndef OPENDATA_TEST_TLS_FIXTURES
    CertCloseStore(store, 0);
#endif
    if (!certificate) {
#ifdef OPENDATA_TEST_TLS_FIXTURES
        opendata_test_tls_fixture_close(&fixture);
#endif
        return report_database_error("wallet certificate disappeared from CurrentUser\\My");
    }
    bool metadata_valid = true;
    char name[1024];
    printf("  Subject: ");
    DWORD name_size = CertGetNameStringA(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                         NULL, name, (DWORD)sizeof(name));
    if (name_size > 1) print_escaped((const unsigned char*)name, name_size - 1);
    if (name_size == 0) metadata_valid = false;
    printf("\n  Issuer: ");
    name_size = CertGetNameStringA(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                   CERT_NAME_ISSUER_FLAG, NULL, name, (DWORD)sizeof(name));
    if (name_size > 1) print_escaped((const unsigned char*)name, name_size - 1);
    if (name_size == 0) metadata_valid = false;
    SYSTEMTIME not_before, not_after;
    if (FileTimeToSystemTime(&certificate->pCertInfo->NotBefore, &not_before) &&
        FileTimeToSystemTime(&certificate->pCertInfo->NotAfter, &not_after)) {
        printf("\n  Validity: %04u-%02u-%02uT%02u:%02u:%02uZ to "
               "%04u-%02u-%02uT%02u:%02u:%02uZ",
               not_before.wYear, not_before.wMonth, not_before.wDay,
               not_before.wHour, not_before.wMinute, not_before.wSecond,
               not_after.wYear, not_after.wMonth, not_after.wDay,
               not_after.wHour, not_after.wMinute, not_after.wSecond);
    } else {
        metadata_valid = false;
    }
    PCERT_EXTENSION san_extension = CertFindExtension(
        szOID_SUBJECT_ALT_NAME2, certificate->pCertInfo->cExtension,
        certificate->pCertInfo->rgExtension);
    printf("\n  SANs:");
    if (san_extension) {
        DWORD alt_size = 0;
        PCERT_ALT_NAME_INFO alt_names = NULL;
        if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME,
                                san_extension->Value.pbData, san_extension->Value.cbData,
                                CRYPT_DECODE_ALLOC_FLAG, NULL, &alt_names, &alt_size)) {
            for (DWORD i = 0; i < alt_names->cAltEntry; i++) {
                PCERT_ALT_NAME_ENTRY entry = &alt_names->rgAltEntry[i];
                printf("%s", i ? ", " : " ");
                if (entry->dwAltNameChoice == CERT_ALT_NAME_DNS_NAME)
                    metadata_valid = print_wide_escaped(entry->pwszDNSName) && metadata_valid;
                else if (entry->dwAltNameChoice == CERT_ALT_NAME_RFC822_NAME)
                    metadata_valid = print_wide_escaped(entry->pwszRfc822Name) && metadata_valid;
                else if (entry->dwAltNameChoice == CERT_ALT_NAME_URL)
                    metadata_valid = print_wide_escaped(entry->pwszURL) && metadata_valid;
                else if (entry->dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS) {
                    char ip[INET6_ADDRSTRLEN];
                    int family = entry->IPAddress.cbData == 4 ? AF_INET :
                                 entry->IPAddress.cbData == 16 ? AF_INET6 : 0;
                    if (family && InetNtopA(family, entry->IPAddress.pbData,
                                            ip, sizeof(ip))) fputs(ip, stdout);
                    else {
                        fputs("(invalid IP SAN)", stdout);
                        metadata_valid = false;
                    }
                } else {
                    fputs("(other SAN)", stdout);
                }
            }
            LocalFree(alt_names);
        } else {
            fputs(" (unreadable)", stdout);
            metadata_valid = false;
        }
    } else {
        fputs(" (none)", stdout);
    }
    printf("\n  EKUs:");
    DWORD usage_size = 0;
    PCERT_ENHKEY_USAGE usage = NULL;
    if (CertGetEnhancedKeyUsage(certificate, 0, NULL, &usage_size) && usage_size) {
        usage = (PCERT_ENHKEY_USAGE)malloc(usage_size);
        if (usage && CertGetEnhancedKeyUsage(certificate, 0, usage, &usage_size)) {
            if (usage->cUsageIdentifier == 0) fputs(" (not restricted)", stdout);
            for (DWORD i = 0; i < usage->cUsageIdentifier; i++)
                printf("%s%s", i ? ", " : " ", usage->rgpszUsageIdentifier[i]);
        } else {
            metadata_valid = false;
        }
    } else {
        metadata_valid = false;
    }
    free(usage);
    putchar('\n');
    CertFreeCertificateContext(certificate);
#ifdef OPENDATA_TEST_TLS_FIXTURES
    opendata_test_tls_fixture_close(&fixture);
#endif
    if (!metadata_valid)
        return report_database_error("cannot decode Windows wallet certificate metadata");
    return true;
}
#else
#if defined(OPENDATA_TLS)
static bool print_bio_value(const char* label, BIO* bio) {
    BUF_MEM* memory = NULL;
    BIO_get_mem_ptr(bio, &memory);
    if (!memory) return report_database_error("cannot inspect wallet certificate metadata");
    printf("  %s: ", label);
    print_escaped((const unsigned char*)memory->data, memory->length);
    putchar('\n');
    return true;
}

static bool inspect_pem_certificate(const char* label, const char* path) {
    BIO* input = BIO_new_file(path, "r");
    X509* certificate = input ? PEM_read_bio_X509(input, NULL, NULL, NULL) : NULL;
    BIO_free(input);
    if (!certificate) return report_database_error("cannot inspect %s wallet certificate", label);
    printf("%s certificate metadata:\n", label);
    BIO* output = BIO_new(BIO_s_mem());
    bool ok = output &&
        X509_NAME_print_ex(output, X509_get_subject_name(certificate), 0,
                           XN_FLAG_RFC2253) >= 0 &&
        print_bio_value("Subject", output);
    if (output) BIO_free(output);
    output = BIO_new(BIO_s_mem());
    ok = ok && output &&
         X509_NAME_print_ex(output, X509_get_issuer_name(certificate), 0,
                            XN_FLAG_RFC2253) >= 0 &&
         print_bio_value("Issuer", output);
    if (output) BIO_free(output);
    const ASN1_TIME* times[] = {X509_get0_notBefore(certificate),
                                X509_get0_notAfter(certificate)};
    const char* labels[] = {"Not before", "Not after"};
    for (size_t i = 0; ok && i < 2; i++) {
        output = BIO_new(BIO_s_mem());
        ok = output && ASN1_TIME_print(output, times[i]) == 1 &&
             print_bio_value(labels[i], output);
        BIO_free(output);
    }
    GENERAL_NAMES* names = X509_get_ext_d2i(certificate, NID_subject_alt_name, NULL, NULL);
    printf("  SANs:");
    if (names) {
        for (int i = 0; i < sk_GENERAL_NAME_num(names); i++) {
            GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
            printf("%s", i ? ", " : " ");
            if (name->type == GEN_DNS || name->type == GEN_EMAIL ||
                name->type == GEN_URI) {
                ASN1_STRING* value = name->d.ia5;
                print_escaped(ASN1_STRING_get0_data(value),
                              (size_t)ASN1_STRING_length(value));
            } else if (name->type == GEN_IPADD) {
                char ip[INET6_ADDRSTRLEN];
                int family = ASN1_STRING_length(name->d.iPAddress) == 4 ? AF_INET :
                             ASN1_STRING_length(name->d.iPAddress) == 16 ? AF_INET6 : 0;
                if (family && inet_ntop(family, ASN1_STRING_get0_data(name->d.iPAddress),
                                        ip, sizeof(ip))) fputs(ip, stdout);
                else fputs("(invalid IP SAN)", stdout);
            } else {
                fputs("(other SAN)", stdout);
            }
        }
        GENERAL_NAMES_free(names);
    } else {
        fputs(" (none)", stdout);
    }
    putchar('\n');
    EXTENDED_KEY_USAGE* usage = X509_get_ext_d2i(certificate, NID_ext_key_usage, NULL, NULL);
    printf("  EKUs:");
    if (usage) {
        for (int i = 0; i < sk_ASN1_OBJECT_num(usage); i++) {
            char oid[128];
            int length = OBJ_obj2txt(oid, sizeof(oid),
                                     sk_ASN1_OBJECT_value(usage, i), 1);
            if (length > 0) printf("%s%.*s", i ? ", " : " ", length, oid);
        }
        EXTENDED_KEY_USAGE_free(usage);
    } else {
        fputs(" (not restricted)", stdout);
    }
    X509_free(certificate);
    return ok;
}
#endif
#endif

bool inspect_tls_wallet(const char* directory) {
    TlsWallet wallet;
    if (!load_tls_wallet(directory, &wallet)) return false;
    printf("Wallet: %s\nFormat version: 1\n", wallet.directory);
    if (wallet.server_cert[0]) {
        printf("Server certificate: %s\nServer private key: %s (owner-only permissions)\n",
               wallet.server_cert, wallet.server_key);
#if defined(OPENDATA_TLS) && !defined(_WIN32)
        if (!inspect_pem_certificate("Server", wallet.server_cert)) return false;
#endif
    }
    if (wallet.server_thumbprint[0]) {
        printf("Server identity: %s thumbprint=%s (private key remains in OS store)\n",
               wallet.server_store, wallet.server_thumbprint);
#ifdef _WIN32
        if (!inspect_store_certificate(wallet.server_thumbprint)) return false;
#endif
    }
    if (wallet.cluster_cert[0]) {
        printf("Cluster certificate: %s\nCluster private key: %s (owner-only permissions)\n",
               wallet.cluster_cert, wallet.cluster_key);
        printf("Cluster CA bundle: %s\n", wallet.cluster_ca[0] ? wallet.cluster_ca : "(missing)");
#if defined(OPENDATA_TLS) && !defined(_WIN32)
        if (!inspect_pem_certificate("Cluster", wallet.cluster_cert)) return false;
#endif
    }
    if (wallet.cluster_thumbprint[0]) {
        printf("Cluster identity: %s thumbprint=%s trust=%s\n",
               wallet.cluster_store, wallet.cluster_thumbprint,
               wallet.cluster_trust_store[0] ? wallet.cluster_trust_store : "(CurrentUser\\Root)");
#ifdef _WIN32
        if (!inspect_store_certificate(wallet.cluster_thumbprint)) return false;
#endif
    }
    return true;
}
