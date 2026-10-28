#include "database_engine.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCK(s) closesocket(s)
#define SHUT_SOCK(s) shutdown((s), SD_BOTH)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
#define CLOSE_SOCK(s) close(s)
#define SHUT_SOCK(s) shutdown((s), SHUT_RDWR)
#endif

#if defined(OPENDATA_TLS) && defined(_WIN32)
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#ifdef OPENDATA_TEST_TLS_FIXTURES
#include "../tests/cluster_tls_test_fixture.h"
#endif
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#elif defined(OPENDATA_TLS)
#include <openssl/ssl.h>
#include <openssl/crypto.h>
#endif

#define MAX_CLIENT_SESSIONS 64
#define NETWORK_OUTPUT_LIMIT (32u * 1024u * 1024u)
#define AUTH_LIMIT 256

static void log_auth_statistics(void) {
    AuthRateLimitStats stats = auth_rate_limit_stats();
    fprintf(stderr, "AUTH_STATS admitted=%llu credential_failures=%llu "
            "source_throttled=%llu pair_throttled=%llu global_throttled=%llu "
            "source_saturated=%llu pair_saturated=%llu\n",
            (unsigned long long)stats.admitted, (unsigned long long)stats.credential_failures,
            (unsigned long long)stats.source_throttled, (unsigned long long)stats.pair_throttled,
            (unsigned long long)stats.global_throttled,
            (unsigned long long)stats.source_saturated, (unsigned long long)stats.pair_saturated);
}

typedef struct {
    int id;
    bool active, killed;
    char addr[64], state[32], current_query[128];
    time_t connected_at;
    socket_t sock;
} ClientSessionEntry;

static ClientSessionEntry g_sessions[MAX_CLIENT_SESSIONS];
static int g_next_session_id = 1;

int register_client_session(const char* addr) {
    if (g_next_session_id == INT_MAX) return -1;
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            ClientSessionEntry* s = &g_sessions[i];
            memset(s, 0, sizeof(*s));
            s->id = g_next_session_id++;
            s->active = true;
            s->connected_at = time(NULL);
            s->sock = INVALID_SOCK;
            snprintf(s->addr, sizeof(s->addr), "%s", addr ? addr : "127.0.0.1");
            snprintf(s->state, sizeof(s->state), "AUTHENTICATING");
            return s->id;
        }
    }
    return -1;
}

void unregister_client_session(int id) {
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        if (g_sessions[i].id == id) {
            g_sessions[i].active = false;
            g_sessions[i].sock = INVALID_SOCK;
        }
    }
}

void update_client_session_query(int id, const char* state, const char* query) {
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        if (g_sessions[i].active && g_sessions[i].id == id) {
            if (state) snprintf(g_sessions[i].state, sizeof(g_sessions[i].state), "%s", state);
            if (query) snprintf(g_sessions[i].current_query, sizeof(g_sessions[i].current_query), "%s", query);
        }
    }
}

bool is_client_session_killed(int id) {
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++)
        if (g_sessions[i].id == id) return g_sessions[i].killed;
    return false;
}

bool kill_client_session(int id) {
    if (id == 0) return report_database_error("cannot kill local console session 0");
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        if (g_sessions[i].active && g_sessions[i].id == id) {
            g_sessions[i].killed = true;
            /* The reactor owns close(), preventing reuse/double-close races. */
            if (g_sessions[i].sock != INVALID_SOCK) SHUT_SOCK(g_sessions[i].sock);
            printf("Session %d terminated.\n", id);
            return true;
        }
    }
    return report_database_error("session %d not found or already closed", id);
}

void print_active_sessions(void) {
    time_t now = time(NULL);
    puts("SESSION_ID\tADDRESS\tSTATE\tUPTIME_S\tCURRENT_QUERY");
    puts("0\t127.0.0.1:console\tACTIVE\t0\tSHOW SESSIONS");
    int count = 1;
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        ClientSessionEntry* s = &g_sessions[i];
        if (s->active && !s->killed) {
            printf("%d\t%s\t%s\t%ld\t%s\n", s->id, s->addr, s->state,
                   (long)(now >= s->connected_at ? now - s->connected_at : 0), s->current_query);
            count++;
        }
    }
    printf("\n%d active session%s.\n", count, count == 1 ? "" : "s");
}

typedef struct {
    socket_t sock;
    int session_id;
    bool active, closing, failed, detected, pg, startup, authenticated;
    UserRole role;
    bool tls_pending, tls_ready, tls_want_write;
    bool quoted, quote_pending;
    char input[MAX_QUERY_LEN];
    size_t used;
    char user[AUTH_LIMIT];
    uint32_t source_address;
    unsigned char* output;
    size_t output_len, output_sent, output_cap;
    OutputFormat format;
    time_t accepted_at, last_activity;
#ifdef OPENDATA_TLS
#ifdef _WIN32
    CtxtHandle tls;
    bool tls_context_valid, tls_buffered;
    SecPkgContext_StreamSizes tls_sizes;
    unsigned char tls_rx[65536], tls_plain[16384], tls_tx[65536];
    size_t tls_rx_len, tls_plain_len, tls_plain_offset, tls_tx_len, tls_tx_sent;
#else
    SSL* tls;
#endif
#endif
} WireClient;

#if defined(OPENDATA_TLS) && defined(_WIN32)
typedef struct {
    CredHandle credentials;
    bool acquired;
    PCCERT_CONTEXT certificate;
#ifdef OPENDATA_TEST_TLS_FIXTURES
    OpendataTestTlsFixture fixture;
#endif
} WindowsTlsContext;

static void windows_tls_context_free(WindowsTlsContext* context) {
    if (context->acquired) FreeCredentialsHandle(&context->credentials);
    if (context->certificate) CertFreeCertificateContext(context->certificate);
#ifdef OPENDATA_TEST_TLS_FIXTURES
    opendata_test_tls_fixture_close(&context->fixture);
#endif
    memset(context, 0, sizeof(*context));
}

static bool windows_tls_context_init(WindowsTlsContext* context, const ServerConfig* config) {
    if (!config->cert_path || strncmp(config->cert_path, "store:", 6) ||
        strlen(config->cert_path + 6) != 40 || config->key_path)
        return report_database_error("Windows TLS requires --cert store:<40-hex-thumbprint> in CurrentUser\\My; omit --key");
    BYTE thumbprint[20];
    for (size_t i = 0; i < sizeof(thumbprint); i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            char ch = uppercase_ascii(config->cert_path[6 + i * 2 + j]);
            if (!is_ascii_digit(ch) && (ch < 'A' || ch > 'F'))
                return report_database_error("invalid Windows TLS certificate thumbprint");
            value = value * 16 + (unsigned)(is_ascii_digit(ch) ? ch - '0' : ch - 'A' + 10);
        }
        thumbprint[i] = (BYTE)value;
    }
#ifdef OPENDATA_TEST_TLS_FIXTURES
    HCERTSTORE store = opendata_test_tls_fixture_wallet_store(&context->fixture);
#else
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, L"MY");
#endif
    if (!store) return report_database_error("cannot open Windows TLS certificate store");
    CRYPT_HASH_BLOB hash = {sizeof(thumbprint), thumbprint};
    context->certificate = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, NULL);
#ifndef OPENDATA_TEST_TLS_FIXTURES
    CertCloseStore(store, 0);
#endif
    if (!context->certificate) {
        windows_tls_context_free(context);
        return report_database_error("TLS certificate not found in CurrentUser\\My");
    }
    if (CertVerifyTimeValidity(NULL, context->certificate->pCertInfo) != 0) {
        windows_tls_context_free(context);
        return report_database_error("TLS certificate is expired or not yet valid");
    }
    /* The certificate must be usable for server authentication. */
    {
        PCERT_ENHKEY_USAGE usage = NULL;
        DWORD usage_len = 0;
        bool eku_ok = false;
        if (CertGetEnhancedKeyUsage(context->certificate, 0, NULL, &usage_len) && usage_len > 0) {
            usage = (PCERT_ENHKEY_USAGE)malloc(usage_len);
            if (usage && CertGetEnhancedKeyUsage(context->certificate, 0, usage, &usage_len)) {
                if (usage->cUsageIdentifier == 0) {
                    eku_ok = true; /* no EKU extension: valid for all purposes */
                } else {
                    for (DWORD i = 0; i < usage->cUsageIdentifier; i++) {
                        if (usage->rgpszUsageIdentifier[i] &&
                            strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0) {
                            eku_ok = true;
                            break;
                        }
                    }
                }
            }
            free(usage);
        }
        if (!eku_ok) {
            windows_tls_context_free(context);
            return report_database_error("TLS certificate lacks the Server Authentication EKU");
        }
    }
    SCHANNEL_CRED credentials;
    memset(&credentials, 0, sizeof(credentials));
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.cCreds = 1;
    credentials.paCred = &context->certificate;
    credentials.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER;
    credentials.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    TimeStamp expiry;
    SECURITY_STATUS status = AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A,
        SECPKG_CRED_INBOUND, NULL, &credentials, NULL, NULL, &context->credentials, &expiry);
    context->acquired = status == SEC_E_OK;
    if (!context->acquired) {
        windows_tls_context_free(context);
        return report_database_error("cannot acquire Schannel TLS credentials; verify certificate private-key access (status 0x%08lx)",
                                     (unsigned long)status);
    }
    return true;
}
#endif

static int g_wire_transaction_owner = 0;

static bool socket_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, (long)FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool socket_would_block(void) {
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static void close_wire_client(WireClient* c) {
    lock_engine_state();
    if (g_wire_transaction_owner == c->session_id) {
        bool quit = false;
        if (g_pending && !execute_console_statement("ROLLBACK;", &quit))
            report_database_error("cannot roll back disconnected network session");
        g_wire_transaction_owner = 0;
    }
    unlock_engine_state();
#ifdef OPENDATA_TLS
#ifdef _WIN32
    if (c->tls_context_valid) DeleteSecurityContext(&c->tls);
    SecureZeroMemory(c->tls_plain, sizeof(c->tls_plain));
#else
    SSL_free(c->tls);
#endif
#endif
    unregister_client_session(c->session_id);
    CLOSE_SOCK(c->sock);
    free(c->output);
    memset(c, 0, sizeof(*c));
}

static void wire_send(WireClient* c, const void* data, size_t length) {
    if (c->failed || length == 0) return;
    if (length > NETWORK_OUTPUT_LIMIT - c->output_len) {
        fprintf(stderr, "Network session %d exceeded output limit.\n", c->session_id);
        c->failed = true;
        return;
    }
    size_t required = c->output_len + length;
    if (required > c->output_cap) {
        size_t capacity = c->output_cap ? c->output_cap : 4096;
        while (capacity < required) {
            capacity = capacity > NETWORK_OUTPUT_LIMIT / 2 ? NETWORK_OUTPUT_LIMIT : capacity * 2;
        }
        unsigned char* resized = realloc(c->output, capacity);
        if (!resized) {
            fprintf(stderr, "Network session %d: out of memory for output.\n", c->session_id);
            c->failed = true;
            return;
        }
        c->output = resized;
        c->output_cap = capacity;
    }
    memcpy(c->output + c->output_len, data, length);
    c->output_len += length;
}

static void wire_str(WireClient* c, const char* text) {
    wire_send(c, text, strlen(text));
}

static uint32_t pg_read_u32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wire_u32(WireClient* c, uint32_t value) {
    unsigned char bytes[4] = {
        (unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value
    };
    wire_send(c, bytes, sizeof(bytes));
}

static void wire_u16(WireClient* c, unsigned value) {
    unsigned char bytes[2] = {(unsigned char)(value >> 8), (unsigned char)value};
    wire_send(c, bytes, sizeof(bytes));
}

static void pg_header(WireClient* c, char type, size_t body_length) {
    wire_send(c, &type, 1);
    wire_u32(c, (uint32_t)(body_length + 4));
}

static void pg_ready(WireClient* c) {
    pg_header(c, 'Z', 1);
    wire_send(c, g_pending ? "T" : "I", 1);
}

static void wire_error(WireClient* c, const char* message) {
    if (c->pg) {
        size_t length = strlen(message) + 1;
        pg_header(c, 'E', 1 + 6 + 1 + 6 + 1 + length + 1);
        wire_send(c, "SERROR\0C28000\0M", 15);
        wire_send(c, message, length);
        wire_send(c, "", 1);
    } else {
        wire_str(c, "ERROR: ");
        wire_str(c, message);
        wire_str(c, "\r\n");
    }
}

static void authenticate_client(WireClient* c, const char* password) {
    uint64_t now_ms;
#ifdef _WIN32
    now_ms = GetTickCount64();
#else
    struct timespec monotonic;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0) {
        report_database_error("cannot read authentication admission clock");
        wire_error(c, "authentication failed");
        c->closing = true;
        return;
    }
    now_ms = (uint64_t)monotonic.tv_sec * 1000u + (uint64_t)monotonic.tv_nsec / 1000000u;
#endif
    if (!admit_auth_attempt(c->source_address, c->user, now_ms)) {
        wire_error(c, "authentication failed");
        c->closing = true;
        return;
    }
    UserRole role = 0;
    lock_engine_state();
    bool authenticated = authenticate_database_user(g_committed, c->user, password, &role);
    unlock_engine_state();
    if (!authenticated) {
        record_auth_credential_failure();
        wire_error(c, "authentication failed");
        c->closing = true;
        return;
    }
    c->role = role;
    c->authenticated = true;
    update_client_session_query(c->session_id, "IDLE", "-");
    if (c->pg) {
        pg_header(c, 'R', 4);
        wire_u32(c, 0);
        pg_header(c, 'S', sizeof("server_version") + sizeof("15.0"));
        wire_send(c, "server_version\0" "15.0", sizeof("server_version") + sizeof("15.0"));
        pg_header(c, 'S', sizeof("client_encoding") + sizeof("UTF8"));
        wire_send(c, "client_encoding\0" "UTF8", sizeof("client_encoding") + sizeof("UTF8"));
        pg_ready(c);
    } else {
        wire_str(c, role == USER_ROLE_ADMIN ?
                 "Authenticated as database administrator.\r\n> " :
                 "Authenticated as read-only user.\r\n> ");
    }
}

static void send_escaped(WireClient* c, const char* text, bool json) {
    wire_str(c, "\"");
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (json && (*p == '"' || *p == '\\')) {
            wire_str(c, "\\");
        } else if (json && *p < 32) {
            char escape[7];
            snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)*p);
            wire_str(c, escape);
            continue;
        } else if (!json && *p == '"') {
            wire_str(c, "\"");
        }
        wire_send(c, p, 1);
    }
    wire_str(c, "\"");
}

static void send_results(WireClient* c, const QueryResultCapture* cap) {
    if (c->pg) {
        size_t desc_len = 2;
        for (int col = 0; col < cap->col_count; col++)
            desc_len += strlen(cap->col_names[col]) + 1 + 18;
        pg_header(c, 'T', desc_len);
        wire_u16(c, (unsigned)cap->col_count);
        for (int col = 0; col < cap->col_count; col++) {
            wire_send(c, cap->col_names[col], strlen(cap->col_names[col]) + 1);
            wire_u32(c, 0); wire_u16(c, 0); wire_u32(c, 25);
            wire_u16(c, 65535); wire_u32(c, UINT32_MAX); wire_u16(c, 0);
        }
        for (int row = 0; row < cap->row_count; row++) {
            size_t row_len = 2;
            for (int col = 0; col < cap->col_count; col++)
                row_len += 4 + (cap->rows[row][col].is_null ? 0 : strlen(cap->rows[row][col].text));
            pg_header(c, 'D', row_len);
            wire_u16(c, (unsigned)cap->col_count);
            for (int col = 0; col < cap->col_count; col++) {
                const Value* v = &cap->rows[row][col];
                wire_u32(c, v->is_null ? UINT32_MAX : (uint32_t)strlen(v->text));
                if (!v->is_null) wire_str(c, v->text);
            }
        }
        return;
    }
    if (c->format == FORMAT_JSON) wire_str(c, "[\r\n");
    else {
        for (int col = 0; col < cap->col_count; col++) {
            if (col) wire_str(c, c->format == FORMAT_CSV ? "," : "\t");
            if (c->format == FORMAT_CSV) send_escaped(c, cap->col_names[col], false);
            else wire_str(c, cap->col_names[col]);
        }
        wire_str(c, "\r\n");
    }
    for (int row = 0; row < cap->row_count; row++) {
        if (c->format == FORMAT_JSON) wire_str(c, "  {");
        for (int col = 0; col < cap->col_count; col++) {
            const Value* v = &cap->rows[row][col];
            if (col) wire_str(c, c->format == FORMAT_TABLE ? "\t" : ",");
            if (c->format == FORMAT_JSON) {
                send_escaped(c, cap->col_names[col], true);
                wire_str(c, ":");
            }
            if (v->is_null) {
                if (c->format != FORMAT_CSV) wire_str(c, c->format == FORMAT_JSON ? "null" : "NULL");
            } else if (c->format == FORMAT_TABLE) wire_str(c, v->text);
            else send_escaped(c, v->text, c->format == FORMAT_JSON);
        }
        if (c->format == FORMAT_JSON) wire_str(c, row + 1 < cap->row_count ? "}," : "}");
        wire_str(c, "\r\n");
    }
    if (c->format == FORMAT_JSON) wire_str(c, "]\r\n");
    if (c->format == FORMAT_TABLE) {
        char count[80];
        snprintf(count, sizeof(count), "\r\n%d row%s selected.\r\n", cap->row_count, cap->row_count == 1 ? "" : "s");
        wire_str(c, count);
    }
}

static void execute_wire_query(WireClient* c, const char* sql) {
    lock_engine_state();
    int user_index = find_user_index(g_committed, c->user);
    if (user_index < 0 || g_committed->users[user_index].disabled) {
        wire_error(c, "account is disabled or no longer exists");
        c->closing = true;
        if (c->pg) pg_ready(c);
        unlock_engine_state();
        return;
    }
    c->role = g_committed->users[user_index].role;
    if (g_wire_transaction_owner && g_wire_transaction_owner != c->session_id) {
        wire_error(c, "another session owns the active transaction");
        if (c->pg) pg_ready(c); else wire_str(c, "> ");
        unlock_engine_state();
        return;
    }
    /* Console commands can open files, change databases, or invoke an editor. */
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.cursor = sql;
    scan_next_token(&parser);
    bool first_select = token_is_keyword(&parser, "SELECT");
    bool first_with = token_is_keyword(&parser, "WITH");
    bool first_explain = token_is_keyword(&parser, "EXPLAIN");
    bool user_admin_command = false;
    bool permitted = false;
    if (token_is_keyword(&parser, "CREATE") || token_is_keyword(&parser, "ALTER") ||
        token_is_keyword(&parser, "DROP") || token_is_keyword(&parser, "SHOW")) {
        Parser lookahead = parser;
        scan_next_token(&lookahead);
        user_admin_command =
            ((token_is_keyword(&parser, "CREATE") || token_is_keyword(&parser, "ALTER") ||
              token_is_keyword(&parser, "DROP")) && token_is_keyword(&lookahead, "USER")) ||
            (token_is_keyword(&parser, "SHOW") && token_is_keyword(&lookahead, "USERS"));
        if (user_admin_command) permitted = true;
    }
    const char* allowed[] = {"SELECT", "WITH", "EXPLAIN", "INSERT", "UPDATE", "DELETE", "CREATE",
                            "ALTER", "DROP", "TRUNCATE", "ANALYZE", "BEGIN", "COMMIT", "ROLLBACK",
                            "SAVEPOINT", "RELEASE", "CONNECTION", "GRANT", "REVOKE"};
    permitted = user_admin_command;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (token_is_keyword(&parser, allowed[i])) permitted = true;
    /* Hot-patch restarts interrupt service: ADMIN role only. */
    if (token_is_keyword(&parser, "RESTART")) permitted = (c->role == USER_ROLE_ADMIN);
    /* Grant management changes the security catalog: ADMIN role only. */
    if (token_is_keyword(&parser, "GRANT") || token_is_keyword(&parser, "REVOKE"))
        permitted = (c->role == USER_ROLE_ADMIN);
    if (token_is_keyword(&parser, "CREATE") || token_is_keyword(&parser, "DROP")) {
        scan_next_token(&parser);
        if (token_is_keyword(&parser, "DATABASE")) permitted = false;
    }
    if (token_is_keyword(&parser, "SHOW") && !user_admin_command) {
        Parser show_lookahead = parser;
        scan_next_token(&show_lookahead);
        permitted = token_is_keyword(&show_lookahead, "GRANTS") &&
                    c->role == USER_ROLE_ADMIN;
    }
    if (c->role == USER_ROLE_READ_ONLY) {
        permitted = first_select || first_with ||
                    (first_explain && token_is_keyword(&parser, "EXPLAIN")) ||
                    token_is_keyword(&parser, "CONNECTION") ||
                    token_is_keyword(&parser, "INSERT") ||
                    token_is_keyword(&parser, "UPDATE") ||
                    token_is_keyword(&parser, "DELETE") ||
                    token_is_keyword(&parser, "TRUNCATE") ||
                    token_is_keyword(&parser, "BEGIN") ||
                    token_is_keyword(&parser, "COMMIT") ||
                    token_is_keyword(&parser, "ROLLBACK") ||
                    token_is_keyword(&parser, "SAVEPOINT") ||
                    token_is_keyword(&parser, "RELEASE");
        if (first_explain) {
            Parser explained = parser;
            scan_next_token(&explained);
            permitted = token_is_keyword(&explained, "SELECT");
        }
    }
    if (!permitted) {
        wire_error(c, c->role == USER_ROLE_READ_ONLY ?
                   "permission denied for this statement (check table grants with your administrator)" :
                   "command is not permitted over the network");
        if (c->pg) pg_ready(c); else wire_str(c, "> ");
        unlock_engine_state();
        return;
    }
    if (g_raft_role == RAFT_FOLLOWER &&
        !token_is_keyword(&parser, "SELECT") &&
        !token_is_keyword(&parser, "WITH") &&
        !token_is_keyword(&parser, "EXPLAIN")) {
        wire_error(c, "follower nodes accept read-only queries");
        if (c->pg) pg_ready(c); else wire_str(c, "> ");
        unlock_engine_state();
        return;
    }
    QueryResultCapture capture = {0};
    capture.silent = true;
    QueryResultCapture* old_capture = g_result_capture;
    g_result_capture = &capture;
    snprintf(g_network_user, sizeof(g_network_user), "%s", c->user);
#ifdef OPENDATA_TLS
#ifdef _WIN32
    g_network_tls_active = c->tls_ready;
#else
    g_network_tls_active = c->tls && SSL_is_init_finished(c->tls) == 1;
#endif
#else
    g_network_tls_active = false;
#endif
    update_client_session_query(c->session_id, "EXECUTING",
                                user_admin_command ? "<USER ADMINISTRATION>" : sql);
    bool quit = false;
    bool ok = execute_console_statement(sql, &quit);
    g_network_user[0] = '\0';
    g_network_tls_active = false;
    g_wire_transaction_owner = g_pending ? c->session_id : 0;
    g_result_capture = old_capture;
    update_client_session_query(c->session_id, "IDLE",
                                user_admin_command ? "<USER ADMINISTRATION>" : sql);
    if (!ok) wire_error(c, g_last_error[0] ? g_last_error : "SQL execution failed");
    else {
        if (capture.col_count > 0) send_results(c, &capture);
        if (c->pg) {
            char tag[64];
            if (capture.col_count > 0) snprintf(tag, sizeof(tag), "SELECT %d", capture.row_count);
            else snprintf(tag, sizeof(tag), "OK");
            pg_header(c, 'C', strlen(tag) + 1);
            wire_send(c, tag, strlen(tag) + 1);
        } else if (capture.col_count == 0) wire_str(c, "OK\r\n");
    }
    free(capture.rows);
    if (c->pg) pg_ready(c); else wire_str(c, "> ");
    unlock_engine_state();
}

static bool pg_startup(WireClient* c, const unsigned char* p, size_t length) {
    if (length < 9 || pg_read_u32(p + 4) != 196608u || p[length - 1] != 0) return false;
    size_t off = 8;
    bool user_found = false;
    while (off < length && p[off]) {
        const unsigned char* end = memchr(p + off, 0, length - off);
        if (!end) return false;
        const char* key = (const char*)p + off;
        off = (size_t)(end - p) + 1;
        if (off >= length) return false;
        end = memchr(p + off, 0, length - off);
        if (!end) return false;
        if (!strcmp(key, "user")) {
            size_t n = (size_t)(end - (p + off));
            if (user_found || n == 0 || n >= sizeof(c->user)) return false;
            memcpy(c->user, p + off, n);
            c->user[n] = 0;
            user_found = true;
        }
        off = (size_t)(end - p) + 1;
    }
    return user_found && off == length - 1;
}

static void process_pg(WireClient* c) {
    while (!c->closing && !c->failed && c->used >= (c->startup ? 5u : 4u)) {
        const unsigned char* p = (const unsigned char*)c->input;
        size_t length = c->startup ? (size_t)pg_read_u32(p + 1) + 1 : pg_read_u32(p);
        if (length < (c->startup ? 5u : 8u) || length >= sizeof(c->input)) {
            wire_error(c, "invalid packet length"); c->closing = true; break;
        }
        if (c->used < length) break;
        if (!c->startup) {
            if (length == 8 && pg_read_u32(p + 4) == 80877103u) {
                wire_str(c, "N");
            } else if (!pg_startup(c, p, length)) {
                wire_error(c, "invalid startup message"); c->closing = true;
            } else {
                c->startup = true;
                pg_header(c, 'R', 4);
                wire_u32(c, 3); /* PasswordMessage; remote plaintext binds are forbidden. */
            }
        } else if (!c->authenticated) {
            if (p[0] != 'p' || length < 7 || p[length - 1] != 0 ||
                memchr(p + 5, 0, length - 6) != NULL) {
                wire_error(c, "authentication required"); c->closing = true;
            } else authenticate_client(c, (const char*)p + 5);
        } else if (p[0] == 'X') {
            c->closing = true;
        } else if (p[0] == 'Q' && length >= 6 && p[length - 1] == 0 &&
                   memchr(p + 5, 0, length - 6) == NULL) {
            execute_wire_query(c, (const char*)p + 5);
        } else {
            wire_error(c, "unsupported or malformed protocol message"); c->closing = true;
        }
        size_t remaining = c->used - length;
        memmove(c->input, c->input + length, remaining);
        memset(c->input + remaining, 0, length);
        c->used = remaining;
    }
}

static void process_text(WireClient* c) {
    if (!c->authenticated) {
        if (strncmp(c->input, "AUTH ", 5) != 0) {
            wire_error(c, "authentication required: AUTH <user> <password>");
            c->closing = true;
            return;
        }
        char* separator = strchr(c->input + 5, ' ');
        if (!separator || (size_t)(separator - c->input - 5) >= sizeof(c->user)) {
            wire_error(c, "authentication failed"); c->closing = true; return;
        }
        *separator = 0;
        snprintf(c->user, sizeof(c->user), "%s", c->input + 5);
        authenticate_client(c, separator + 1);
        return;
    }
    char* cmd = c->input;
    while (*cmd && is_sql_whitespace(*cmd)) cmd++;
    size_t length = strlen(cmd);
    while (length && (is_sql_whitespace(cmd[length - 1]) || cmd[length - 1] == ';')) cmd[--length] = 0;
    if (!length) return;
    if (!strcmp(cmd, "PING") || !strcmp(cmd, "ping")) wire_str(c, "PONG\r\n> ");
    else if (!strcmp(cmd, "EXIT") || !strcmp(cmd, "QUIT") || !strcmp(cmd, "exit") || !strcmp(cmd, "quit")) {
        wire_str(c, "BYE\r\n"); c->closing = true;
    } else if (!strcmp(cmd, ".mode json") || !strcmp(cmd, "SET FORMAT JSON")) {
        c->format = FORMAT_JSON; wire_str(c, "Format set to JSON\r\n> ");
    } else if (!strcmp(cmd, ".mode csv") || !strcmp(cmd, "SET FORMAT CSV")) {
        c->format = FORMAT_CSV; wire_str(c, "Format set to CSV\r\n> ");
    } else if (!strcmp(cmd, ".mode table") || !strcmp(cmd, "SET FORMAT TABLE")) {
        c->format = FORMAT_TABLE; wire_str(c, "Format set to TABLE\r\n> ");
    } else execute_wire_query(c, cmd);
}

static void receive_input(WireClient* c, const unsigned char* bytes, size_t length) {
    if (!c->detected && length) {
        c->detected = true;
        c->pg = bytes[0] == 0;
    }
    if (c->pg) {
        if (length >= sizeof(c->input) - c->used) {
            wire_error(c, "input limit exceeded"); c->closing = true; return;
        }
        memcpy(c->input + c->used, bytes, length);
        c->used += length;
        process_pg(c);
        return;
    }
    for (size_t i = 0; i < length && !c->closing && !c->failed; i++) {
        char ch = (char)bytes[i];
        if (!ch) { wire_error(c, "NUL bytes are not permitted"); c->closing = true; break; }
        if (c->used + 2 >= sizeof(c->input)) {
            wire_error(c, "input limit exceeded"); c->closing = true; break;
        }
        /* A pending quote closes the literal unless followed by another quote. */
        if (c->quote_pending) {
            c->quote_pending = false;
            if (ch != '\'') c->quoted = false;
        } else if (ch == '\'') {
            if (c->quoted) c->quote_pending = true;
            else c->quoted = true;
        }
        bool delimiter = c->authenticated ? (!c->quoted && (ch == ';' || ch == '\n')) : ch == '\n';
        if (ch != '\r' || c->quoted) c->input[c->used++] = ch;
        if (delimiter || (!c->authenticated && ch == '\n')) {
            if (c->used && (c->input[c->used - 1] == '\n' || c->input[c->used - 1] == ';')) c->used--;
            c->input[c->used] = 0;
            process_text(c);
            memset(c->input, 0, c->used);
            c->used = 0;
            c->quoted = c->quote_pending = false;
        }
    }
}

#if defined(OPENDATA_TLS) && defined(_WIN32)
static bool windows_tls_flush(WireClient* c) {
    if (c->tls_tx_sent == c->tls_tx_len) return true;
    int count = (int)(c->tls_tx_len - c->tls_tx_sent);
    int sent = send(c->sock, (const char*)c->tls_tx + c->tls_tx_sent, count, 0);
    if (sent <= 0) return sent < 0 && socket_would_block();
    c->tls_tx_sent += (size_t)sent;
    if (c->tls_tx_sent == c->tls_tx_len) c->tls_tx_sent = c->tls_tx_len = 0;
    return true;
}

static bool windows_tls_receive_encrypted(WireClient* c) {
    if (c->tls_rx_len == sizeof(c->tls_rx)) return false;
    int received = recv(c->sock, (char*)c->tls_rx + c->tls_rx_len,
                        (int)(sizeof(c->tls_rx) - c->tls_rx_len), 0);
    if (received <= 0) return received < 0 && socket_would_block();
    c->tls_rx_len += (size_t)received;
    return true;
}

static void windows_tls_preserve_extra(WireClient* c, const SecBuffer* buffers, size_t count) {
    size_t extra = 0;
    for (size_t i = 0; i < count; i++) {
        if (buffers[i].BufferType == SECBUFFER_EXTRA) extra = buffers[i].cbBuffer;
    }
    if (extra <= c->tls_rx_len) {
        memmove(c->tls_rx, c->tls_rx + c->tls_rx_len - extra, extra);
        c->tls_rx_len = extra;
    } else c->failed = true;
    c->tls_buffered = extra != 0;
}

static bool windows_tls_handshake(WireClient* c, WindowsTlsContext* context, bool readable) {
    if (c->tls_tx_len) return windows_tls_flush(c);
    if (readable && !windows_tls_receive_encrypted(c)) return false;
    if (!c->tls_rx_len) return true;
    SecBuffer input[2] = {
        {(unsigned long)c->tls_rx_len, SECBUFFER_TOKEN, c->tls_rx},
        {0, SECBUFFER_EMPTY, NULL}
    };
    SecBuffer output = {0, SECBUFFER_TOKEN, NULL};
    SecBufferDesc in_desc = {SECBUFFER_VERSION, 2, input};
    SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, &output};
    unsigned long attributes = 0;
    TimeStamp expiry;
    SECURITY_STATUS status = AcceptSecurityContext(&context->credentials,
        c->tls_context_valid ? &c->tls : NULL, &in_desc,
        ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
        ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
        SECURITY_NATIVE_DREP, &c->tls, &out_desc, &attributes, &expiry);
    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) c->tls_context_valid = true;
    if (output.pvBuffer) {
        if (output.cbBuffer > sizeof(c->tls_tx)) {
            FreeContextBuffer(output.pvBuffer);
            return false;
        }
        memcpy(c->tls_tx, output.pvBuffer, output.cbBuffer);
        c->tls_tx_len = output.cbBuffer;
        c->tls_tx_sent = 0;
        FreeContextBuffer(output.pvBuffer);
    }
    if (status == SEC_E_INCOMPLETE_MESSAGE) return true;
    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) return false;
    windows_tls_preserve_extra(c, input, 2);
    if (status == SEC_E_OK) {
        if (QueryContextAttributesA(&c->tls, SECPKG_ATTR_STREAM_SIZES, &c->tls_sizes) != SEC_E_OK ||
            c->tls_sizes.cbMaximumMessage > sizeof(c->tls_plain) ||
            (size_t)c->tls_sizes.cbHeader + c->tls_sizes.cbMaximumMessage +
                c->tls_sizes.cbTrailer > sizeof(c->tls_tx)) return false;
        c->tls_pending = false;
        c->tls_ready = true;
    }
    return windows_tls_flush(c);
}

static int windows_tls_read(WireClient* c, unsigned char* data, size_t capacity, bool readable) {
    if (c->tls_plain_offset == c->tls_plain_len) {
        c->tls_plain_offset = c->tls_plain_len = 0;
        if (readable && !windows_tls_receive_encrypted(c)) return 0;
        if (!c->tls_rx_len) return -1;
        SecBuffer buffers[4] = {
            {(unsigned long)c->tls_rx_len, SECBUFFER_DATA, c->tls_rx},
            {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}
        };
        SecBufferDesc description = {SECBUFFER_VERSION, 4, buffers};
        SECURITY_STATUS status = DecryptMessage(&c->tls, &description, 0, NULL);
        if (status == SEC_E_INCOMPLETE_MESSAGE) { c->tls_buffered = false; return -1; }
        if (status != SEC_E_OK) return 0; /* Includes close_notify and renegotiation. */
        for (size_t i = 0; i < 4; i++) {
            if (buffers[i].BufferType == SECBUFFER_DATA) {
                if (buffers[i].cbBuffer > sizeof(c->tls_plain)) return 0;
                memcpy(c->tls_plain, buffers[i].pvBuffer, buffers[i].cbBuffer);
                c->tls_plain_len = buffers[i].cbBuffer;
            }
        }
        windows_tls_preserve_extra(c, buffers, 4);
    }
    size_t count = c->tls_plain_len - c->tls_plain_offset;
    if (count > capacity) count = capacity;
    memcpy(data, c->tls_plain + c->tls_plain_offset, count);
    SecureZeroMemory(c->tls_plain + c->tls_plain_offset, count);
    c->tls_plain_offset += count;
    return count ? (int)count : -1;
}

static bool windows_tls_write(WireClient* c, size_t* consumed) {
    *consumed = 0;
    if (c->tls_tx_len) return windows_tls_flush(c);
    size_t count = c->output_len - c->output_sent;
    if (count > c->tls_sizes.cbMaximumMessage) count = c->tls_sizes.cbMaximumMessage;
    size_t header = c->tls_sizes.cbHeader;
    memcpy(c->tls_tx + header, c->output + c->output_sent, count);
    SecBuffer buffers[4] = {
        {c->tls_sizes.cbHeader, SECBUFFER_STREAM_HEADER, c->tls_tx},
        {(unsigned long)count, SECBUFFER_DATA, c->tls_tx + header},
        {c->tls_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, c->tls_tx + header + count},
        {0, SECBUFFER_EMPTY, NULL}
    };
    SecBufferDesc description = {SECBUFFER_VERSION, 4, buffers};
    if (EncryptMessage(&c->tls, 0, &description, 0) != SEC_E_OK) return false;
    c->tls_tx_len = (size_t)buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
    c->tls_tx_sent = 0;
    *consumed = count;
    return windows_tls_flush(c);
}
#endif

static bool flush_output(WireClient* c) {
    if (c->output_sent == c->output_len) return true;
    size_t remaining = c->output_len - c->output_sent;
    int count = (int)(remaining > INT_MAX ? INT_MAX : remaining);
    int sent;
#ifdef OPENDATA_TLS
#ifdef _WIN32
    if (c->tls_ready) {
        size_t consumed = 0;
        if (!windows_tls_write(c, &consumed)) return false;
        sent = (int)consumed;
    } else
#else
    if (c->tls) {
        sent = SSL_write(c->tls, c->output + c->output_sent, count);
        if (sent <= 0) {
            int error = SSL_get_error(c->tls, sent);
            return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
        }
    } else
#endif
#endif
    {
#ifdef MSG_NOSIGNAL
        sent = (int)send(c->sock, c->output + c->output_sent, (size_t)count, MSG_NOSIGNAL);
#else
        sent = send(c->sock, (const char*)c->output + c->output_sent, count, 0);
#endif
        if (sent <= 0) return sent < 0 && socket_would_block();
    }
    c->output_sent += (size_t)sent;
    if (c->output_sent == c->output_len) c->output_sent = c->output_len = 0;
    return true;
}

bool bootstrap_server_accounts(void) {
    const char* user = getenv("OPENDATA_SERVER_USER");
    const char* password = getenv("OPENDATA_SERVER_PASSWORD");
    if (!g_committed) return report_database_error("database is not initialized");
    if (!g_committed->user_count) {
        if (g_raft_role == RAFT_FOLLOWER) {
            if (g_generation != 0 || g_committed->table_count != 0 || g_pending)
                return report_database_error("account-less followers require a provisioned leader snapshot; local bootstrap is not permitted");
            fprintf(stderr, "Empty follower awaiting account catalog from authenticated leader replication.\n");
            return true;
        }
        if (!user || !*user || strlen(user) >= AUTH_LIMIT || strpbrk(user, " \t\r\n") ||
            !password || strlen(password) < 16 || strlen(password) >= AUTH_LIMIT ||
            strpbrk(password, "\r\n"))
            return report_database_error("set OPENDATA_SERVER_USER and OPENDATA_SERVER_PASSWORD (16-255 bytes) for first-admin bootstrap");
        if (!create_bootstrap_admin(user, password)) return false;
    }
    return true;
}

bool run_nonblocking_wire_server(const ServerConfig* config) {
    if (!config) return report_database_error("server configuration is required");
    reset_auth_rate_limit();
    const char* bind_ip = config->bind_addr && *config->bind_addr ? config->bind_addr : "127.0.0.1";
    int port = config->port ? config->port : (config->tcps ? 7338 : 7337);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (port < 1 || port > 65535 || inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1)
        return report_database_error("invalid server address or port");
    address.sin_port = htons((uint16_t)port);
    if (!config->tcps && (ntohl(address.sin_addr.s_addr) >> 24) != 127)
        return report_database_error("non-loopback binds require --tcps with a certificate and key");
    lock_engine_state();
    bool accounts_ready = bootstrap_server_accounts();
    unlock_engine_state();
    if (!accounts_ready) return false;
    if (!config->tcps && (config->cert_path || config->key_path))
        return report_database_error("--cert and --key require --tcps");
#ifdef OPENDATA_TLS
#ifdef _WIN32
    WindowsTlsContext context = {0};
    if (config->tcps && !windows_tls_context_init(&context, config)) return false;
#else
    SSL_CTX* context = NULL;
    if (config->tcps) {
        context = SSL_CTX_new(TLS_server_method());
        if (!context) return report_database_error("cannot initialize TLS context");
        if (!config->cert_path || !config->key_path ||
            SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
            SSL_CTX_use_certificate_chain_file(context, config->cert_path) != 1 ||
            SSL_CTX_use_PrivateKey_file(context, config->key_path, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context) != 1) {
            SSL_CTX_free(context);
            return report_database_error("invalid TLS certificate or private key");
        }
        SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    }
#endif
#else
    if (config->tcps) return report_database_error("TLS unavailable: rebuild with OPENDATA_TLS using the OS TLS provider");
#endif
    bool success = false;
#ifdef _WIN32
    WSADATA wsa;
    bool winsock_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if (!winsock_started) {
#ifdef OPENDATA_TLS
        windows_tls_context_free(&context);
#endif
        return report_database_error("failed to initialize Winsock");
    }
#endif
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    socket_t listener = INVALID_SOCK;
    WireClient* clients = calloc(MAX_CLIENT_SESSIONS, sizeof(*clients));
    if (!clients) {
        report_database_error("cannot allocate network server"); goto cleanup;
    }
    if (g_hotpatch_mode) {
        /* Zero-downtime handoff: adopt the predecessor's listener; its accept
           backlog held new connections while this process started. */
        intptr_t adopted = hotpatch_adopt_listener(port, bind_ip);
        if (adopted == (intptr_t)-1) goto cleanup;
        listener = (socket_t)adopted;
        if (!socket_nonblocking(listener)) {
            report_database_error("cannot configure the handed-off listener"); goto cleanup;
        }
#ifdef _WIN32
        unsigned long self_pid = (unsigned long)GetCurrentProcessId();
#else
        unsigned long self_pid = (unsigned long)getpid();
#endif
        fprintf(stderr, "PC/OpenData hot-patch successor pid=%lu adopted the %s:%d listener\n",
                self_pid, bind_ip, port);
    } else {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener == INVALID_SOCK) {
            report_database_error("cannot allocate network server"); goto cleanup;
        }
#ifdef _WIN32
        int exclusive = 1;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
#else
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (!socket_nonblocking(listener) ||
            bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
            listen(listener, MAX_CLIENT_SESSIONS) != 0) {
            report_database_error("cannot bind/listen on %s:%d", bind_ip, port); goto cleanup;
        }
    }
    fprintf(stderr, "PC/OpenData authenticated %s server listening on %s:%d\n",
            config->tcps ? "TLS" : "loopback TCP", bind_ip, port);
    uint64_t last_auth_report = 0;
    bool draining = false;
    double drain_deadline = 0.0;
    while (!atomic_load(&g_stop)) {
        if (!draining && atomic_load(&g_restart_requested)) {
            if (g_cluster_active) {
                atomic_store(&g_restart_requested, false);
                report_database_error("hot-patch restart is unavailable in cluster mode");
            } else {
                char spawn_error[256];
                if (hotpatch_spawn_successor((intptr_t)listener, spawn_error, sizeof(spawn_error))) {
                    draining = true;
                    drain_deadline = monotonic_seconds() + 5.0;
                    fprintf(stderr, "AUDIT timestamp=%lld actor=%s operation=RESTART target=server result=successor-launched\n",
                            (long long)time(NULL),
                            g_network_user[0] ? g_network_user : "signal-or-console");
                    fprintf(stderr, "hot-patch successor launched; draining sessions on %s:%d\n",
                            bind_ip, port);
                } else {
                    atomic_store(&g_restart_requested, false);
                    report_database_error("hot-patch restart aborted: %s", spawn_error);
                }
            }
        }
        uint64_t report_now;
#ifdef _WIN32
        report_now = GetTickCount64() / 1000u;
#else
        struct timespec report_time;
        if (clock_gettime(CLOCK_MONOTONIC, &report_time) != 0) {
            report_database_error("cannot read authentication reporting clock"); goto cleanup;
        }
        report_now = (uint64_t)report_time.tv_sec;
#endif
        if (!last_auth_report || report_now - last_auth_report >= 60) {
            log_auth_statistics();
            last_auth_report = report_now;
        }
        struct pollfd descriptors[MAX_CLIENT_SESSIONS + 1];
        int map[MAX_CLIENT_SESSIONS + 1];
        int count = 1;
        descriptors[0].fd = listener;
        descriptors[0].events = draining ? 0 : POLLIN;
        descriptors[0].revents = 0;
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
            WireClient* c = &clients[i];
            if (!c->active) continue;
            bool pending_output = c->output_len != 0;
#if defined(OPENDATA_TLS) && defined(_WIN32)
            pending_output = pending_output || c->tls_tx_len != 0;
#endif
            if (c->failed || is_client_session_killed(c->session_id) ||
                (!c->authenticated && now - c->accepted_at >= 10) ||
                now - c->last_activity >= 300 || (c->closing && !pending_output)) {
                close_wire_client(c); continue;
            }
            descriptors[count].fd = c->sock;
            descriptors[count].events = POLLIN;
            if (pending_output || c->tls_want_write) descriptors[count].events |= POLLOUT;
            descriptors[count].revents = 0;
            map[count++] = i;
        }
#ifdef _WIN32
        int result = WSAPoll(descriptors, (ULONG)count, 50);
#else
        int result = poll(descriptors, (nfds_t)count, 50);
#endif
        if (result < 0) {
            if (socket_would_block()) continue;
            report_database_error("network poll failed"); goto cleanup;
        }
        if (!draining && descriptors[0].revents & POLLIN) {
            struct sockaddr_in peer;
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            socket_t accepted = accept(listener, (struct sockaddr*)&peer, &peer_len);
            if (accepted != INVALID_SOCK) {
                int slot = 0;
                while (slot < MAX_CLIENT_SESSIONS && clients[slot].active) slot++;
                if (slot == MAX_CLIENT_SESSIONS || !socket_nonblocking(accepted)) CLOSE_SOCK(accepted);
                else {
                    WireClient* c = &clients[slot];
                    c->sock = accepted;
                    c->active = true;
                    c->format = g_output_format;
                    c->accepted_at = c->last_activity = time(NULL);
                    c->tls_pending = config->tcps;
                    c->source_address = peer.sin_addr.s_addr;
                    char addr[64];
                    snprintf(addr, sizeof(addr), "%s:%u", inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port));
                    c->session_id = register_client_session(addr);
                    if (c->session_id < 0) c->failed = true;
                    for (int j = 0; j < MAX_CLIENT_SESSIONS; j++)
                        if (g_sessions[j].active && g_sessions[j].id == c->session_id) g_sessions[j].sock = accepted;
                }
            }
        }
        for (int i = 1; i < count; i++) {
            WireClient* c = &clients[map[i]];
            short events = descriptors[i].revents;
            if (events & (POLLERR | POLLHUP | POLLNVAL)) { c->failed = true; continue; }
#ifdef OPENDATA_TLS
            if (c->tls_pending) {
                if (draining) { close_wire_client(c); continue; }
#ifdef _WIN32
                bool tls_started = c->tls_context_valid || c->tls_rx_len != 0;
                if (!tls_started && (events & POLLIN)) {
#else
                if (!c->tls && (events & POLLIN)) {
#endif
                    unsigned char peek[8];
                    int n = (int)recv(c->sock, (char*)peek, sizeof(peek), MSG_PEEK);
                    if (n <= 0) { if (!socket_would_block()) c->failed = true; continue; }
                    if (peek[0] == 0) {
                        if (n < 8) {
                            /* A partial SSLRequest would spin poll() at 100% CPU
                               since the bytes are never consumed; drop the client. */
                            c->failed = true;
                            continue;
                        }
                        if (pg_read_u32(peek) != 8 || pg_read_u32(peek + 4) != 80877103u) {
                            c->failed = true; continue;
                        }
                        if (recv(c->sock, (char*)peek, 8, 0) != 8 ||
                            send(c->sock, "S", 1, 0) != 1) { c->failed = true; continue; }
                        c->pg = c->detected = true;
                    } else if (peek[0] != 0x16) { c->failed = true; continue; }
#ifndef _WIN32
                    c->tls = SSL_new(context);
                    if (!c->tls || SSL_set_fd(c->tls, (int)c->sock) != 1) { c->failed = true; continue; }
                    SSL_set_accept_state(c->tls);
                    SSL_set_mode(c->tls, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
#ifdef _WIN32
                    tls_started = true;
#endif
                }
#ifdef _WIN32
                if (!tls_started) continue;
                if (!windows_tls_handshake(c, &context, (events & POLLIN) != 0)) c->failed = true;
                continue;
#else
                if (!c->tls) continue;
                int handshake = SSL_accept(c->tls);
                if (handshake != 1) {
                    int error = SSL_get_error(c->tls, handshake);
                    c->tls_want_write = error == SSL_ERROR_WANT_WRITE;
                    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) c->failed = true;
                    continue;
                }
                c->tls_pending = false;
                c->tls_ready = true;
                c->tls_want_write = false;
#endif
            }
#endif
            if (c->tls_pending) continue;
#if defined(OPENDATA_TLS) && defined(_WIN32)
            if (c->tls_tx_len) {
                if (!windows_tls_flush(c)) c->failed = true;
                continue;
            }
#endif
            /* Do not read more requests while a reply awaits delivery. */
            if (c->output_len) {
                if (!flush_output(c)) c->failed = true;
                continue;
            }
            /* Draining for a hot-patch restart: replies are flushed above;
               idle sessions are closed without reading new requests. */
            if (draining) { close_wire_client(c); continue; }
            if (!c->closing && ((events & POLLIN)
#ifdef OPENDATA_TLS
#ifdef _WIN32
                || (c->tls_ready && (c->tls_plain_offset < c->tls_plain_len || c->tls_buffered))
#else
                || (c->tls && SSL_pending(c->tls) > 0)
#endif
#endif
                )) {
                unsigned char buffer[2048];
                int n;
#ifdef OPENDATA_TLS
#ifdef _WIN32
                if (c->tls_ready) {
                    n = windows_tls_read(c, buffer, sizeof(buffer), (events & POLLIN) != 0);
                    if (n <= 0) { if (n == 0) c->failed = true; continue; }
                } else
#else
                if (c->tls) {
                    n = SSL_read(c->tls, buffer, sizeof(buffer));
                    if (n <= 0) {
                        int error = SSL_get_error(c->tls, n);
                        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) c->failed = true;
                        continue;
                    }
                } else
#endif
#endif
                {
                    n = (int)recv(c->sock, (char*)buffer, sizeof(buffer), 0);
                    if (n <= 0) { if (n == 0 || !socket_would_block()) c->failed = true; continue; }
                }
                c->last_activity = time(NULL);
                receive_input(c, buffer, (size_t)n);
            } else if (!c->detected && (!config->tcps || c->tls_ready) && now > c->accepted_at) {
                c->detected = true;
                wire_str(c, "PC/OpenData " SERVER_VERSION " Authentication required: AUTH <user> <password>\r\n> ");
            }
        }
        if (draining) {
            bool replies_pending = false;
            for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
                if (!clients[i].active) continue;
                if (clients[i].output_len != 0
#if defined(OPENDATA_TLS) && defined(_WIN32)
                    || clients[i].tls_tx_len != 0
#endif
                    ) {
                    replies_pending = true;
                    break;
                }
            }
            if (!replies_pending || monotonic_seconds() >= drain_deadline) break;
        }
    }
    success = true;
cleanup:
    log_auth_statistics();
    if (clients) {
        for (int i = 0; i < MAX_CLIENT_SESSIONS; i++)
            if (clients[i].active) close_wire_client(&clients[i]);
    }
    free(clients);
    if (listener != INVALID_SOCK) CLOSE_SOCK(listener);
#ifdef _WIN32
    WSACleanup();
#endif
#ifdef OPENDATA_TLS
#ifdef _WIN32
    windows_tls_context_free(&context);
#else
    SSL_CTX_free(context);
#endif
#endif
    return success;
}

bool run_headless_server(const ServerConfig* config) {
    /* The engine and result capture are process-global: execute SQL serially. */
    return run_nonblocking_wire_server(config);
}
