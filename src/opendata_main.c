#include "database_engine.h"

#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
#pragma comment(lib, "advapi32.lib")

static SERVICE_STATUS g_service_status;
static SERVICE_STATUS_HANDLE g_status_handle = NULL;
static const char* g_svc_database = NULL;
static int g_svc_port = 7350;

static void WINAPI ServiceCtrlHandler(DWORD request) {
    switch (request) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_service_status.dwWin32ExitCode = 0;
        g_service_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_status_handle, &g_service_status);
        exit(0);
        break;
    default:
        break;
    }
    SetServiceStatus(g_status_handle, &g_service_status);
}

static void WINAPI ServiceMain(DWORD dwArgc, LPSTR* lpszArgv) {
    (void)dwArgc;
    (void)lpszArgv;
    g_status_handle = RegisterServiceCtrlHandlerA("OpenDataSQL", ServiceCtrlHandler);
    if (!g_status_handle) return;

    memset(&g_service_status, 0, sizeof(g_service_status));
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_status_handle, &g_service_status);

    const char* db_file = g_svc_database ? g_svc_database : "opendata.pcod";
    resolve_absolute_path(db_file, g_path);
    initialize_snapshot_io();
    if (!open_or_restore_database(NULL)) {
        g_service_status.dwCurrentState = SERVICE_STOPPED;
        g_service_status.dwWin32ExitCode = ERROR_FILE_NOT_FOUND;
        SetServiceStatus(g_status_handle, &g_service_status);
        return;
    }

    g_service_status.dwCurrentState = SERVICE_RUNNING;
    g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_status_handle, &g_service_status);

    ServerConfig cfg = { g_svc_port ? g_svc_port : 7350, "0.0.0.0", false, NULL, NULL };
    g_server_mode = true;
    install_server_signal_handlers();
    run_headless_server(&cfg);

    g_service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_status_handle, &g_service_status);
}

bool install_windows_service(const char* service_name, const char* display_name, const char* db_path, int port) {
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) == 0) return false;

    const char* target_db = (db_path && *db_path) ? db_path : "opendata.pcod";
    const char* sname = (service_name && *service_name) ? service_name : "OpenDataSQL";
    const char* dname = (display_name && *display_name) ? display_name : "PC/OpenData SQL Database Server";
    if (strpbrk(target_db, "\"\r\n") || strpbrk(sname, "\"\r\n") || strpbrk(dname, "\"\r\n")) {
        printf("ERROR: Service parameters cannot contain quotes or newline characters.\n");
        return false;
    }

    char cmd[MAX_PATH * 2 + 128];
    int n = snprintf(cmd, sizeof(cmd), "\"%s\" --service-worker --database \"%s\" --port %d",
                     exe_path, target_db, (port > 0 && port <= 65535) ? port : 7350);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return false;

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("Failed to open Service Control Manager (run as Administrator).\n");
        return false;
    }

    SC_HANDLE svc = CreateServiceA(
        scm, sname, dname,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmd, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            printf("Service '%s' already exists.\n", sname);
        } else {
            printf("CreateService failed (error %lu).\n", err);
        }
        CloseServiceHandle(scm);
        return false;
    }

    printf("Service '%s' successfully installed.\n", sname);
    printf("Binary path: %s\n", cmd);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool uninstall_windows_service(const char* service_name) {
    const char* sname = (service_name && *service_name) ? service_name : "OpenDataSQL";
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("Failed to open Service Control Manager (run as Administrator).\n");
        return false;
    }

    SC_HANDLE svc = OpenServiceA(scm, sname, SERVICE_STOP | DELETE);
    if (!svc) {
        printf("Service '%s' not found or cannot be opened.\n", sname);
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    if (!DeleteService(svc)) {
        printf("Failed to delete service '%s' (error %lu).\n", sname, GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    printf("Service '%s' successfully uninstalled.\n", sname);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool start_windows_service(const char* service_name) {
    const char* sname = (service_name && *service_name) ? service_name : "OpenDataSQL";
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, sname, SERVICE_START);
    if (!svc) { CloseServiceHandle(scm); return false; }
    bool ok = StartServiceA(svc, 0, NULL) != 0;
    if (ok) printf("Service '%s' started.\n", sname);
    else printf("Failed to start service '%s' (error %lu).\n", sname, GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool stop_windows_service(const char* service_name) {
    const char* sname = (service_name && *service_name) ? service_name : "OpenDataSQL";
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, sname, SERVICE_STOP);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_STATUS status;
    bool ok = ControlService(svc, SERVICE_CONTROL_STOP, &status) != 0;
    if (ok) printf("Service '%s' stopped.\n", sname);
    else printf("Failed to stop service '%s' (error %lu).\n", sname, GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}
#endif

static bool parse_cli_number(const char* text, int minimum, int maximum, int* value) {
    char* end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !*text || *end || parsed < minimum || parsed > maximum)
        return report_database_error("invalid numeric option value: %s", text);
    *value = (int)parsed;
    return true;
}

/* Semicolon-optional heuristic: a statement looks complete when quotes and
   parentheses are balanced and it ends with a value-like token rather than a
   continuation keyword or operator. False negatives only mean the user still
   needs a semicolon (the old behavior); continuation keywords and trailing
   operators keep multiline entry working. */
static bool looks_like_complete_statement(const char* str) {
    bool in_quote = false;
    int depth = 0;
    const char* s = str;
    for (; *s; s++) {
        if (in_quote) {
            if (*s == '\'') {
                if (*(s + 1) == '\'') s++;
                else in_quote = false;
            }
            continue;
        }
        if (*s == '\'') in_quote = true;
        else if (*s == '(') depth++;
        else if (*s == ')' && depth > 0) depth--;
    }
    if (in_quote || depth != 0) return false;
    while (s > str && is_sql_whitespace(*(s - 1))) s--;
    if (s == str) return false;
    char last = *(s - 1);
    if (!(is_ascii_digit(last) || is_identifier_start(last) || last == '$' ||
          last == '#' || last == ')' || last == '\'')) return false;
    char word[MAX_NAME_LEN];
    size_t wlen = 0;
    const char* w = s;
    while (w > str) {
        char c = *(w - 1);
        if (!(is_identifier_start(c) || is_ascii_digit(c) || c == '$' || c == '#')) break;
        if (wlen + 1 >= sizeof(word)) return true;
        word[wlen++] = uppercase_ascii(c);
        w--;
    }
    for (size_t i = 0; i < wlen / 2; i++) {
        char tmp = word[i];
        word[i] = word[wlen - 1 - i];
        word[wlen - 1 - i] = tmp;
    }
    word[wlen] = '\0';
    static const char* const continuation[] = {
        "SELECT", "FROM", "WHERE", "SET", "VALUES", "INTO", "ON", "BY", "AND",
        "OR", "NOT", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "CROSS", "GROUP",
        "ORDER", "HAVING", "AS", "TO", "CREATE", "ALTER", "DROP", "INSERT",
        "UPDATE", "DELETE", "TRUNCATE", "TABLE", "IMPORT", "EXPORT", "BETWEEN",
        "IN", "IS", "LIKE", "EXISTS", "UNION", "INTERSECT", "EXCEPT", "LIMIT",
        "OFFSET", "WHEN", "THEN", "ELSE", "CASE", "OVER", "PARTITION",
        "REFERENCES", "PRIMARY", "KEY", "UNIQUE", "CONSTRAINT", "CASCADE",
        "RESTRICT", "ADD", "COLUMN", "IF", "WITH", "EXPLAIN", "DO", "CONFLICT",
        "DUPLICATE", "SAVEPOINT", "RELEASE", "HEADER", "USING"
    };
    for (size_t i = 0; i < sizeof(continuation) / sizeof(continuation[0]); i++)
        if (!strcmp(word, continuation[i])) return false;
    return true;
}

static bool is_statement_complete(const char* str) {
    while (is_sql_whitespace(*str)) str++;
    if (!*str) return false;
    char word[MAX_NAME_LEN];
    size_t wlen = 0;
    const char* s = str;
    if (is_identifier_start(*s)) {
        while ((is_identifier_start(*s) || is_ascii_digit(*s) || *s == '$' || *s == '#') && wlen + 1 < sizeof(word)) {
            word[wlen++] = uppercase_ascii(*s++);
        }
    }
    word[wlen] = '\0';
    if (!strcmp(word, "EXIT") || !strcmp(word, "QUIT") || !strcmp(word, "HELP") ||
        !strcmp(word, "CLEAR") || !strcmp(word, "CLS") || !strcmp(word, "COMMIT") ||
        !strcmp(word, "ROLLBACK") || !strcmp(word, "BEGIN") || !strcmp(word, "BACKUP") ||
        !strcmp(word, "RESTART") || !strcmp(word, "USE") || !strcmp(word, "OPEN") ||
        !strcmp(word, "STATUS") || !strcmp(word, "SET") || !strcmp(word, "SHOW") ||
        !strcmp(word, "DESC") || !strcmp(word, "DESCRIBE") || !strcmp(word, "DISCONNECT") ||
        !strcmp(word, "EDIT") || !strcmp(word, "ED") || !strcmp(word, "IRIS") ||
        !strcmp(word, "HISTORY") || !strcmp(word, "VACUUM") || !strcmp(word, "COMPACT") ||
        !strcmp(word, "SOURCE") || !strcmp(word, "SPOOL") || !strcmp(word, "CHECK") ||
        !strcmp(word, "PRAGMA") || !strcmp(word, "SAVEPOINT") || !strcmp(word, "RELEASE") ||
        !strcmp(word, "BROWSE") || !strcmp(word, "SUPPORT") ||
        !strcmp(word, "CONNECTION") || !strcmp(word, "LOAD") ||
        !strcmp(word, "GRANT") || !strcmp(word, "REVOKE") || !strcmp(word, "VERIFY") ||
        !strcmp(word, "GUIDE") || !strcmp(word, "SELFTEST") || !strcmp(word, "SELF_TEST") ||
        !strcmp(word, "SELF") ||
        *str == '?' || *str == '.' || *str == '@') {
        return true;
    }
    bool in_quote = false;
    for (s = str; *s; s++) {
        if (*s == '\'') in_quote = !in_quote;
        else if (!in_quote && *s == ';') return true;
    }
    if (g_semicolon_optional) return looks_like_complete_statement(str);
    return false;
}

int main(int argc, char** argv) {
    hotpatch_capture_launch(argc, argv);
    if (!hotpatch_child_bootstrap()) return 1;
    /* Create the user config and error report on first launch, then load
       saved defaults. */
    ensure_user_config_exists();
    load_user_config();
    log_error_report_event("PC/OpenData " SERVER_VERSION " started");
    const char* database = NULL; const char* backup = NULL; const char* restore = NULL;
    bool is_server = false, is_tcps = false, is_subsystem = false, do_ping = false;
    bool no_default_db = false;
    bool is_dev = false;
    uint32_t pitr_ts = 0;
    int server_port = 0;
    const char* bind_ip = "127.0.0.1";
    const char* execute_sql = NULL;
    const char* cert_path = NULL;
    const char* key_path = NULL;
    const char* cluster_ca_path = NULL;
    const char* wallet_path = NULL;
    const char* inspect_wallet_path = NULL;
#ifdef _WIN32
    char wallet_server_store_cert[48] = {0};
    char wallet_cluster_store_cert[48] = {0};
#endif
    int cluster_node = 1;
    int cluster_port = 0;
    bool sync_replication = false;
    bool is_follower = false;
    char cluster_peers[4][64];
    int peer_count = 0;
    bool cluster_enabled = false;
    const char* ad_server = NULL;
    const char* ad_domain = NULL;
    const char* ad_admin_group = NULL;
    const char* ldap_server = NULL;
    int ldap_port = 0;
    bool ldap_ssl = false;
    const char* ldap_base_dn = NULL;
    const char* ldap_admin_group = NULL;
    const char* krb_realm = NULL;
    const char* krb_service = NULL;
    const char* krb_keytab = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { print_command_help(); puts("\nGet started: opendata mydata.pcod"); return 0; }
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) { puts("PC/OpenData " SERVER_VERSION); puts("Get started: opendata mydata.pcod"); return 0; }
        if (!strcmp(argv[i], "self-test") || !strcmp(argv[i], "selftest") ||
            !strcmp(argv[i], "--self-test") || !strcmp(argv[i], "--selftest")) {
            const char* filter = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : NULL;
            return execute_self_test(filter) ? 0 : 1;
        }
        if (!strcmp(argv[i], "--inspect-wallet")) {
            if (i + 1 >= argc) { report_database_error("--inspect-wallet requires a wallet directory"); return 1; }
            inspect_wallet_path = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--readonly")) { g_readonly = true; continue; }
        if (!strcmp(argv[i], "--server") || !strcmp(argv[i], "-s")) { is_server = true; continue; }
        if (!strcmp(argv[i], "--tcps")) { is_server = true; is_tcps = true; continue; }
        if (!strcmp(argv[i], "--subsystem")) { is_subsystem = true; continue; }
        if (!strcmp(argv[i], "--ping")) { do_ping = true; continue; }
        if (!strcmp(argv[i], "--no-default-db")) { no_default_db = true; continue; }
        if (!strcmp(argv[i], "--check-config")) {
            if (argc != 2) { report_database_error("--check-config cannot be combined with other options"); return 1; }
            return validate_user_config() ? 0 : 1;
        }
#ifdef _WIN32
        if (!strcmp(argv[i], "--install-service")) {
            const char* sname = "OpenDataSQL";
            if (i + 1 < argc && argv[i + 1][0] != '-') sname = argv[++i];
            return install_windows_service(sname, "PC/OpenData SQL Database Server", database ? database : "opendata.pcod", server_port ? server_port : 7350) ? 0 : 1;
        }
        if (!strcmp(argv[i], "--uninstall-service")) {
            const char* sname = "OpenDataSQL";
            if (i + 1 < argc && argv[i + 1][0] != '-') sname = argv[++i];
            return uninstall_windows_service(sname) ? 0 : 1;
        }
        if (!strcmp(argv[i], "--start-service")) {
            const char* sname = "OpenDataSQL";
            if (i + 1 < argc && argv[i + 1][0] != '-') sname = argv[++i];
            return start_windows_service(sname) ? 0 : 1;
        }
        if (!strcmp(argv[i], "--stop-service")) {
            const char* sname = "OpenDataSQL";
            if (i + 1 < argc && argv[i + 1][0] != '-') sname = argv[++i];
            return stop_windows_service(sname) ? 0 : 1;
        }
        if (!strcmp(argv[i], "--service-worker")) {
            if (database) g_svc_database = database;
            if (server_port) g_svc_port = server_port;
            SERVICE_TABLE_ENTRYA DispatchTable[] = {
                { (LPSTR)"OpenDataSQL", (LPSERVICE_MAIN_FUNCTIONA)ServiceMain },
                { NULL, NULL }
            };
            StartServiceCtrlDispatcherA(DispatchTable);
            return 0;
        }
#endif
        if (!strcmp(argv[i], "--pitr")) {
            if (i + 1 >= argc) { report_database_error("--pitr requires a timestamp value"); return 1; }
            pitr_ts = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
                if (!strcmp(argv[i], "--dev")) {
            is_server = true;
            server_port = 7350;
            bind_ip = "127.0.0.1";
            if (!database) database = "dev.pcod";
            is_dev = true;
            continue;
        }
        if (argv[i][0] != '-') {
            if (!database) {
                database = argv[i];
                continue;
            } else if (!execute_sql) {
                execute_sql = argv[i];
                continue;
            } else {
                report_database_error("unexpected argument: %s", argv[i]);
                return 1;
            }
        }
        if (!strcmp(argv[i], "--sync-replication")) { sync_replication = true; cluster_enabled = true; continue; }
        if (!strcmp(argv[i], "--cluster-follower")) { is_follower = true; cluster_enabled = true; continue; }
        if (i + 1 >= argc) { report_database_error("missing option value"); return 1; }
        if (!strcmp(argv[i], "--database")) database = argv[++i];
        else if (!strcmp(argv[i], "--backup-dir")) backup = argv[++i];
        else if (!strcmp(argv[i], "--restore")) restore = argv[++i];
        else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")) {
            if (!parse_cli_number(argv[++i], 1, 65535, &server_port)) return 1;
        }
        else if (!strcmp(argv[i], "--bind")) bind_ip = argv[++i];
        else if (!strcmp(argv[i], "--cert")) cert_path = argv[++i];
        else if (!strcmp(argv[i], "--key")) key_path = argv[++i];
        else if (!strcmp(argv[i], "--cluster-ca")) cluster_ca_path = argv[++i];
        else if (!strcmp(argv[i], "--wallet")) wallet_path = argv[++i];
        else if (!strcmp(argv[i], "--cluster-node")) {
            if (!parse_cli_number(argv[++i], 1, INT_MAX, &cluster_node)) return 1;
            cluster_enabled = true;
        }
        else if (!strcmp(argv[i], "--cluster-port")) {
            if (!parse_cli_number(argv[++i], 1, 65535, &cluster_port)) return 1;
            cluster_enabled = true;
        }
        else if (!strcmp(argv[i], "--cluster-peers")) {
            const char* peers = argv[++i];
            cluster_enabled = true;
            const char* p = peers;
            while (*p) {
                if (peer_count == 4) {
                    report_database_error("at most four cluster peers may be configured");
                    return 1;
                }
                const char* comma = strchr(p, ',');
                size_t plen = comma ? (size_t)(comma - p) : strlen(p);
                if (plen == 0 || plen >= sizeof(cluster_peers[peer_count]) ||
                    !memchr(p, '@', plen)) {
                    report_database_error("invalid cluster peer address");
                    return 1;
                }
                memcpy(cluster_peers[peer_count], p, plen);
                cluster_peers[peer_count][plen] = '\0';
                peer_count++;
                if (!comma) break;
                p = comma + 1;
                if (!*p) {
                    report_database_error("invalid cluster peer list");
                    return 1;
                }
            }
        }
        else if (!strcmp(argv[i], "--format") || !strcmp(argv[i], "-f")) {
            const char* fmt = argv[++i];
            if (!strcmp(fmt, "csv") || !strcmp(fmt, "CSV")) g_output_format = FORMAT_CSV;
            else if (!strcmp(fmt, "json") || !strcmp(fmt, "JSON")) g_output_format = FORMAT_JSON;
            else g_output_format = FORMAT_TABLE;
        }
        else if (!strcmp(argv[i], "--execute") || !strcmp(argv[i], "-e")) execute_sql = argv[++i];
        else if (!strcmp(argv[i], "--backup-interval")) {
            char* end; errno = 0; unsigned long seconds = strtoul(argv[++i], &end, 10);
            if (errno || !argv[i][0] || *end || seconds < 1 || seconds > 86400) { report_database_error("invalid backup interval"); return 1; }
            g_backup_interval = (unsigned)seconds;
        }
        else if (!strcmp(argv[i], "--ad-server")) ad_server = argv[++i];
        else if (!strcmp(argv[i], "--ad-domain")) ad_domain = argv[++i];
        else if (!strcmp(argv[i], "--ad-admin-group")) ad_admin_group = argv[++i];
        else if (!strcmp(argv[i], "--ldap-server")) ldap_server = argv[++i];
        else if (!strcmp(argv[i], "--ldap-port")) {
            if (!parse_cli_number(argv[++i], 1, 65535, &ldap_port)) return 1;
        }
        else if (!strcmp(argv[i], "--ldap-ssl")) { ldap_ssl = true; continue; }
        else if (!strcmp(argv[i], "--ldap-base-dn")) ldap_base_dn = argv[++i];
        else if (!strcmp(argv[i], "--ldap-admin-group")) ldap_admin_group = argv[++i];
        else if (!strcmp(argv[i], "--statement-timeout")) {
            int to_ms = 0;
            if (!parse_cli_number(argv[++i], 0, 86400000, &to_ms)) return 1;
            g_statement_timeout_ms = (uint32_t)to_ms;
        }
        else if (!strcmp(argv[i], "--auto-vacuum")) { g_autovacuum_enabled = true; continue; }
        else if (!strcmp(argv[i], "--no-auto-vacuum")) { g_autovacuum_enabled = false; continue; }
        else if (!strcmp(argv[i], "--krb-realm")) krb_realm = argv[++i];
        else if (!strcmp(argv[i], "--krb-service")) krb_service = argv[++i];
        else if (!strcmp(argv[i], "--krb-keytab")) krb_keytab = argv[++i];
        else if (!strcmp(argv[i], "--buffer-pool-size") || !strcmp(argv[i], "--buffer-pool") || !strcmp(argv[i], "-B")) {
            int pool_mb = 0;
            if (!parse_cli_number(argv[++i], 1, 1048576, &pool_mb)) return 1;
            g_buffer_pool_size_mb = (uint32_t)pool_mb;
        }
        else { report_database_error("unknown option: %s", argv[i]); return 1; }
    }
    if (inspect_wallet_path) {
        if (argc != 3 || strcmp(argv[1], "--inspect-wallet")) {
            report_database_error("--inspect-wallet cannot be combined with other options");
            return 1;
        }
        return inspect_tls_wallet(inspect_wallet_path) ? 0 : 1;
    }
    TlsWallet wallet;
    memset(&wallet, 0, sizeof(wallet));
    if (wallet_path) {
        if (!is_tcps && !cluster_enabled) {
            report_database_error("--wallet requires --tcps or cluster mode");
            return 1;
        }
        if (cert_path || key_path || cluster_ca_path) {
            report_database_error("--wallet is mutually exclusive with --cert, --key, and --cluster-ca");
            return 1;
        }
        if (!load_tls_wallet(wallet_path, &wallet)) return 1;
#ifdef _WIN32
        if (wallet.server_thumbprint[0]) {
            snprintf(wallet_server_store_cert, sizeof(wallet_server_store_cert),
                     "store:%s", wallet.server_thumbprint);
            cert_path = wallet_server_store_cert;
        }
        if (wallet.cluster_thumbprint[0]) {
            snprintf(wallet_cluster_store_cert, sizeof(wallet_cluster_store_cert),
                     "store:%s", wallet.cluster_thumbprint);
        }
#else
        cert_path = wallet.server_cert[0] ? wallet.server_cert : NULL;
        key_path = wallet.server_key[0] ? wallet.server_key : NULL;
        cluster_ca_path = wallet.cluster_ca[0] ? wallet.cluster_ca : NULL;
#endif
#ifdef _WIN32
        if (is_tcps && !cert_path) {
#else
        if (is_tcps && (!cert_path || !key_path)) {
#endif
            report_database_error("TLS server wallet requires a server identity");
            return 1;
        }
    }
    if (cluster_enabled && !is_server) {
        report_database_error("cluster nodes must run in --server mode");
        return 1;
    }
    if (ad_server && ad_domain) {
        configure_directory_ad(ad_server, ad_domain, ad_admin_group);
    }
    if (ldap_server && ldap_base_dn) {
        configure_directory_ldap(ldap_server, (int)ldap_port, ldap_ssl, ldap_base_dn, "uid", ldap_admin_group);
    }
    if (krb_realm) {
        configure_kerberos(krb_service ? krb_service : "opendata", krb_realm, krb_keytab);
    }
    /* Zero-arg interactive launch: open the configured default database,
       creating it if missing. --no-default-db opts out. */
    if (!database && !is_server && !execute_sql && !do_ping && !no_default_db &&
        g_default_database[0]) {
        database = g_default_database;
    }
    bool ready = false, worker_started = false;
    if (database && *database) {
        if (!resolve_absolute_path(database, g_path)) {
            report_database_error("invalid database path: %s", database); return 1;
        }
        if (!g_readonly) {
            if (backup && *backup) {
                if (!resolve_absolute_path(backup, g_backup_dir)) {
                    report_database_error("invalid backup directory: %s", backup); return 1;
                }
            } else {
                g_backup_dir[0] = '\0';
                g_backup_interval = 0;
            }
        }
        initialize_snapshot_io();
        ready = open_or_restore_database(restore);
        if (ready && pitr_ts > 0) {
            printf("Performing Point-In-Time Recovery (PITR) to timestamp %u...\n", pitr_ts);
            wal_pitr_restore(g_path, g_backup_dir[0] ? g_backup_dir : NULL, pitr_ts);
        }
        /* Bootstrap is a local provisioning commit, before follower/quorum enforcement. */
        if (ready && cluster_enabled && !is_follower && !bootstrap_server_accounts()) ready = false;
        if (ready && !g_readonly && g_backup_dir[0] != '\0' && g_backup_interval > 0) {
            worker_started = start_periodic_backups();
            if (!worker_started) { report_database_error("cannot start periodic backup worker"); ready = false; }
        }
        if (ready && cluster_enabled) {
            ClusterConfig ccfg;
            memset(&ccfg, 0, sizeof(ccfg));
            ccfg.node_id = cluster_node;
            ccfg.port = cluster_port;
            ccfg.is_leader = !is_follower;
            ccfg.sync_replication = sync_replication;
            ccfg.peer_count = peer_count;
            ccfg.cert_path = wallet_path ?
#ifdef _WIN32
                (wallet_cluster_store_cert[0] ? wallet_cluster_store_cert : NULL)
#else
                (wallet.cluster_cert[0] ? wallet.cluster_cert : NULL)
#endif
                : cert_path;
            ccfg.key_path = wallet_path ?
#ifdef _WIN32
                NULL
#else
                (wallet.cluster_key[0] ? wallet.cluster_key : NULL)
#endif
                : key_path;
            ccfg.ca_path = cluster_ca_path;
            if (wallet_path) {
#ifdef _WIN32
                if (!wallet_cluster_store_cert[0] ||
                    !wallet.cluster_trust_store[0]) {
                    report_database_error("Windows cluster wallet requires cluster identity and cluster.trust_store=CurrentUser\\Root");
                    ready = false;
                }
#else
                if (!wallet.cluster_cert[0] || !wallet.cluster_key[0] || !wallet.cluster_ca[0]) {
                    report_database_error("cluster wallet requires cluster.cert, cluster.key, and cluster.ca");
                    ready = false;
                }
#endif
            }
            for (int p = 0; p < peer_count; p++) {
                snprintf(ccfg.peer_addrs[p], sizeof(ccfg.peer_addrs[p]), "%s", cluster_peers[p]);
            }
            if (ready && !cluster_init(&ccfg)) ready = false;
        }
    } else {
        if (is_server) {
            report_database_error("--database is required for server mode"); return 1;
        }
        if (execute_sql) {
            initialize_snapshot_io();
            g_committed = calloc(1, sizeof(*g_committed));
            if (g_committed) g_db = copy_database(g_committed);
        }
        ready = true;
    }
    if (ready) {
        if (do_ping) {
            puts("PONG");
        } else if (execute_sql) {
            bool quit = false;
            execute_console_statement(execute_sql, &quit);
        } else if (is_server) {
            ServerConfig cfg = { server_port, bind_ip, is_tcps,
                cluster_enabled && !is_tcps ? NULL : cert_path,
                cluster_enabled && !is_tcps ? NULL : key_path };
            g_server_mode = true;
            g_cluster_active = cluster_enabled;
                        if (is_dev) {
                if (g_committed && g_committed->user_count == 0) {
                    execute_console_statement("CREATE USER 'dev' IDENTIFIED BY 'devpassword12345' ROLE ADMIN;", &ready);
                }
                printf("================================================================================\n");
                printf("                    OpenData Development Server (--dev)\n");
                printf("  Listening on: %s:%d\n", bind_ip, server_port);
                printf("  Database:     %s\n", database ? database : "(memory)");
                printf("  User:         dev\n");
                printf("  Password:     devpassword12345\n");
                printf("  Connect:      opendata --port %d -u dev (or connect via VS Code / Studio)\n", server_port);
                printf("================================================================================\n");
            }
            install_server_signal_handlers();
            if (!run_headless_server(&cfg)) ready = false;
        } else {
#ifdef _WIN32
            bool is_term = _isatty(_fileno(stdin)) != 0 && !is_subsystem;
#else
            bool is_term = isatty(fileno(stdin)) != 0 && !is_subsystem;
#endif
            g_interactive_shell = is_term;
            /* The interactive terminal shell defaults to autocommit ON unless
               the user configured otherwise; piped input, scripts, the
               embedded API, and server mode keep manual commit control. */
            if (is_term && !g_autocommit_configured) g_autocommit = true;
            /* Interactive terminals also default to semicolon-optional entry;
               scripts and piped input keep strict semicolon framing. */
            if (is_term && !g_semicolon_optional_configured) g_semicolon_optional = true;
            if (is_term) {
                puts("PC/OpenData SQL Interactive Shell (" SERVER_VERSION ")");
                if (database && *database) {
                    printf("Connected to: %s\n", g_path);
                    if (g_autocommit)
                        puts("Autocommit is ON (SET AUTOCOMMIT OFF; to change).");
                    else
                        puts("Autocommit is OFF — type COMMIT; to save (SET AUTOCOMMIT ON; to change).");
                    puts("Type \"help;\" for instructions, \"exit;\" to quit.\n");
                    if (g_show_tutorial) {
                        puts("Quick tasks: LOAD SAMPLE; loads practice data — IMPORT WIZARD; loads a CSV spreadsheet.");
                        if (g_semicolon_optional)
                            puts("Tip: press Enter to run a complete statement — semicolons are optional here.\n");
                    }
                } else if (g_show_tutorial) {
                    puts("No database connected. Get started in 30 seconds:");
                    puts("  1. CREATE DATABASE 'mydata.pcod';");
                    puts("  2. CREATE TABLE friends (id NUMBER PRIMARY KEY, name VARCHAR2(50));");
                    puts("  3. INSERT INTO friends VALUES (1, 'Ada');");
                    puts("  4. COMMIT;");
                    puts("  5. SELECT * FROM friends;");
                    puts("Type \"help;\" for the full reference, \"exit;\" to quit.\n");
                } else {
                    puts("No database connected. Type \"help;\" for instructions, \"exit;\" to quit.\n");
                }
            } else if (!is_subsystem && database) {
                fprintf(stderr, "PC/OpenData %s. Local durable commits; remote backups every %u seconds.\n"
                        "WARNING: verify --backup-dir is off-machine. Recovery window extends to the last completed backup.\n",
                        SERVER_VERSION, g_backup_interval);
            }
            load_history_file();
            char* buf = malloc(MAX_QUERY_LEN);
            char* line_in = malloc(MAX_QUERY_LEN);
            char* expanded = malloc(MAX_QUERY_LEN);
            if (!buf || !line_in || !expanded) {
                report_database_error("cannot allocate input buffer");
                free(buf);
                free(line_in);
                free(expanded);
            } else {
                bool quit = false;
                size_t buf_used = 0;
                while (!quit) {
                    char prompt[128];
                    if (is_term) {
                        if (buf_used == 0) {
                            if (g_path[0]) {
                                const char* base = g_path;
                                for (const char* p = g_path; *p; p++) {
                                    if (*p == '/' || *p == '\\') base = p + 1;
                                }
                                snprintf(prompt, sizeof(prompt), "opendata (%s%s)> ",
                                         base, g_readonly ? " RO" : (g_pending ? " *" : ""));
                            } else {
                                snprintf(prompt, sizeof(prompt), "opendata> ");
                            }
                        } else {
                            snprintf(prompt, sizeof(prompt), "   ...> ");
                        }
                    } else {
                        prompt[0] = '\0';
                    }

                    if (!read_interactive_line(prompt, line_in, MAX_QUERY_LEN)) {
                        break;
                    }
                    history_expand(line_in, expanded, MAX_QUERY_LEN);

                    const char* line_ptr = expanded;
                    while (is_sql_whitespace(*line_ptr)) line_ptr++;
                    if (!*line_ptr && buf_used == 0) continue;
                    if (!*line_ptr && buf_used > 0) {
                        execute_console_statement(buf, &quit);
                        buf_used = 0;
                        buf[0] = '\0';
                        continue;
                    }

                    char line_cmd[16] = {0};
                    size_t cmd_i = 0;
                    while (*line_ptr && !is_sql_whitespace(*line_ptr) && *line_ptr != ';' && cmd_i + 1 < sizeof(line_cmd)) {
                        line_cmd[cmd_i++] = uppercase_ascii(*line_ptr++);
                    }
                    line_cmd[cmd_i] = '\0';
                    if (!strcmp(line_cmd, "\\C") || !strcmp(line_cmd, "CANCEL")) {
                        buf_used = 0;
                        buf[0] = '\0';
                        puts("Query buffer cleared.");
                        continue;
                    }

                    size_t elen = strlen(expanded);
                    if (buf_used + elen + 2 >= MAX_QUERY_LEN) {
                        report_database_error("statement exceeds input limit");
                        buf_used = 0;
                        buf[0] = '\0';
                        continue;
                    }

                    if (buf_used > 0) buf[buf_used++] = ' ';
                    memcpy(buf + buf_used, expanded, elen);
                    buf_used += elen;
                    buf[buf_used] = '\0';

                    if (is_statement_complete(buf)) {
                        execute_console_statement(buf, &quit);
                        buf_used = 0;
                        buf[0] = '\0';
                        if (fflush(stdout) == EOF) { report_database_error("output write failed"); break; }
                    }
                }
                save_history_file();
                if (is_term && quit) {
                    puts("Bye!");
                }
                free(buf);
                free(line_in);
                free(expanded);
            }
            g_interactive_shell = false;
        }
    }
    if (worker_started) {
        stop_periodic_backups();
        /* On a hot-patch handoff the successor is already waiting on the
           database lock; release it before the final backup so the successor's
           30-second acquisition budget is not consumed by an unbounded write. */
        if (atomic_load(&g_restart_requested)) release_database_file_lock();
        backup_committed_snapshot();
    }
    if (cluster_enabled) {
        cluster_shutdown();
    }
    if (g_spool_file) {
        fclose(g_spool_file);
        g_spool_file = NULL;
    }
    if (g_pending && !g_silent_mode) fprintf(stderr, "NOTICE: uncommitted transaction discarded at exit.\n");
    clear_all_savepoints();
    undo_log_clear();
    destroy_database(g_db); destroy_database(g_committed);
    release_database_file_lock();
    shutdown_snapshot_io();
    log_error_report_event("PC/OpenData " SERVER_VERSION " stopped");
    /* A completed hot-patch handoff is a clean exit regardless of how many
       client statements failed during the server's lifetime. */
    if (g_server_mode && ready) return 0;
    return !ready || g_errors || atomic_load(&g_backup_failed) ? 1 : 0;
}
