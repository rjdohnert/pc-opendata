#include "database_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
#define CLOSE_SOCK(s) close(s)
#endif

bool g_sync_replication = false;
_Atomic RaftRole g_raft_role = RAFT_LEADER;
int g_raft_node_id = 1;
/* Accessed from both the cluster worker and the network/main threads. */
static _Atomic uint32_t g_current_term = 1;
static _Atomic uint32_t g_voted_for_term = 0;
static _Atomic int g_voted_for_node = 0;
static int g_peer_count = 0;
static char g_peer_addrs[4][64];
static ClusterTlsContext* g_cluster_tls;
static socket_t g_cluster_listener = INVALID_SOCK;
static atomic_bool g_cluster_running = false;
#ifdef _WIN32
static bool g_winsock_started;
#endif

#ifdef _WIN32
static HANDLE g_cluster_thread;
#else
static pthread_t g_cluster_thread;
#endif

#define RAFT_MAGIC 0x50434F44 /* 'PCOD' */
/* Cap on how long a synchronous replication connect may block the server. */
#define RAFT_PEER_CONNECT_TIMEOUT_MS 2000

/* Put the socket into non-blocking mode, attempt connect(), and wait up to
   timeout_ms for completion. Returns true when the connection is established. */
static bool raft_connect_with_timeout(socket_t s, const struct sockaddr_in* dest, int timeout_ms) {
#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(s, (long)FIONBIO, &nonblock);
    int rc = connect(s, (const struct sockaddr*)dest, sizeof(*dest));
    if (rc == 0) return true;

    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    TIMEVAL tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
int sel = select(0, NULL, &wfds, NULL, &tv);
    int err = 0; int err_len = sizeof(err); getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &err_len);
    if (sel <= 0) return false;





    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &err_len) != 0 || err != 0) return false;
    nonblock = 0;
    ioctlsocket(s, (long)FIONBIO, &nonblock);
    DWORD ms = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
    return true;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(s, (const struct sockaddr*)dest, sizeof(*dest));
    if (rc == 0) { fcntl(s, F_SETFL, flags); return true; }
    if (errno != EINPROGRESS) { fcntl(s, F_SETFL, flags); return false; }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
int sel = select(0, NULL, &wfds, NULL, &tv);
    int err = 0; int err_len = sizeof(err); getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &err_len);
    if (sel <= 0) return false;
    if (select((int)s + 1, NULL, &wfds, NULL, &tv) <= 0) { fcntl(s, F_SETFL, flags); return false; }
    
    socklen_t err_len = sizeof(err);

    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
        fcntl(s, F_SETFL, flags);
        return false;
    }
    fcntl(s, F_SETFL, flags);
    struct timeval iotv;
    iotv.tv_sec = timeout_ms / 1000;
    iotv.tv_usec = (timeout_ms % 1000) * 1000;
int sel = select(0, NULL, &wfds, NULL, &tv);
    int err = 0; int err_len = sizeof(err); getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &err_len);
    if (sel <= 0) return false;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &iotv, sizeof(iotv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &iotv, sizeof(iotv));
    return true;
#endif
}

typedef enum {
    RAFT_MSG_SYNC_WAL = 1,
    RAFT_MSG_SYNC_ACK = 2,
    RAFT_MSG_VOTE_REQ = 3,
    RAFT_MSG_VOTE_RSP = 4
} RaftMsgType;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t msg_type;
    uint32_t term;
    uint32_t node_id;
    uint32_t payload_len;
} RaftHeader;
#pragma pack(pop)

/* Wire format is little-endian; convert explicitly so mixed-endianness
   clusters cannot disagree on terms, node IDs, or payload lengths. */
static bool raft_host_is_little_endian(void) {
    const uint16_t probe = 1;
    return *((const unsigned char*)&probe) == 1;
}
static uint16_t raft_bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static uint32_t raft_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}
static uint16_t raft_le16(uint16_t v) {
    return raft_host_is_little_endian() ? v : raft_bswap16(v);
}
static uint32_t raft_le32(uint32_t v) {
    return raft_host_is_little_endian() ? v : raft_bswap32(v);
}
static RaftHeader raft_header_to_wire(RaftHeader h) {
    h.magic = raft_le32(h.magic);
    h.msg_type = raft_le16(h.msg_type);
    h.term = raft_le32(h.term);
    h.node_id = raft_le32(h.node_id);
    h.payload_len = raft_le32(h.payload_len);
    return h;
}
static RaftHeader raft_header_from_wire(RaftHeader h) {
    return raft_header_to_wire(h); /* le<->host is the same operation */
}

#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
static bool net_send_all(socket_t sock, const void* data, size_t len) {
    const char* ptr = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        size_t remaining = len - sent;
        int chunk = (int)(remaining > INT_MAX ? INT_MAX : remaining);
#ifdef _WIN32
        int n = send(sock, ptr + sent, chunk, 0);
#else
        ssize_t n = send(sock, ptr + sent, (size_t)chunk, 0);
#endif
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}
#endif

#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
static bool net_recv_all(socket_t sock, void* data, size_t len, int timeout_sec) {
    char* ptr = (char*)data;
    size_t received = 0;

    fd_set fds;
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    while (received < len) {
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        int sel = select((int)sock + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) return false;

        size_t remaining = len - received;
        int chunk = (int)(remaining > INT_MAX ? INT_MAX : remaining);
#ifdef _WIN32
        int n = recv(sock, ptr + received, chunk, 0);
#else
        ssize_t n = recv(sock, ptr + received, (size_t)chunk, 0);
#endif
        if (n <= 0) return false;
        received += (size_t)n;
    }
    return true;
}
#endif

static ClusterTlsStream* cluster_stream_accept(socket_t socket) {
#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
    (void)socket;
    return (ClusterTlsStream*)(uintptr_t)1;
#else
    return cluster_tls_accept(g_cluster_tls, (intptr_t)socket);
#endif
}

static ClusterTlsStream* cluster_stream_connect(socket_t socket, const char* host, int node_id) {
#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
    (void)socket; (void)host; (void)node_id;
    return (ClusterTlsStream*)(uintptr_t)1;
#else
    return cluster_tls_connect(g_cluster_tls, (intptr_t)socket, host, node_id);
#endif
}

#ifndef OPENDATA_TEST_CLUSTER_PLAINTEXT
static int cluster_stream_peer_node_id(ClusterTlsStream* stream) {
    return cluster_tls_peer_node_id(stream);
}
#endif

static bool cluster_stream_send_all(socket_t socket, ClusterTlsStream* stream,
                                    const void* data, size_t length) {
#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
    (void)stream;
    return net_send_all(socket, data, length);
#else
    (void)socket;
    return cluster_tls_send_all(stream, data, length);
#endif
}

static bool cluster_stream_recv_all(socket_t socket, ClusterTlsStream* stream,
                                    void* data, size_t length) {
#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
    (void)stream;
    return net_recv_all(socket, data, length, 2);
#else
    (void)socket;
    return cluster_tls_recv_all(stream, data, length);
#endif
}

static bool cluster_stream_auth_matches(ClusterTlsStream* stream, int node_id) {
#ifdef OPENDATA_TEST_CLUSTER_PLAINTEXT
    (void)stream; (void)node_id;
    return true;
#else
    return cluster_stream_peer_node_id(stream) == node_id;
#endif
}

static void cluster_stream_close(ClusterTlsStream* stream) {
#ifndef OPENDATA_TEST_CLUSTER_PLAINTEXT
    cluster_tls_close(stream);
#else
    (void)stream;
#endif
}

static bool parse_peer_endpoint(const char* endpoint, int* node_id, char* host,
                                size_t host_capacity, int* port) {
    if (!endpoint || !node_id || !host || !host_capacity || !port) return false;
    const char* at = strchr(endpoint, '@');
    const char* colon = strrchr(endpoint, ':');
    if (!at || !colon || at >= colon || strchr(at + 1, '@') ||
        (size_t)(colon - at - 1) >= host_capacity) return false;
    char node_text[16];
    size_t node_length = (size_t)(at - endpoint);
    if (!node_length || node_length >= sizeof(node_text)) return false;
    for (size_t i = 0; i < node_length; i++) {
        if (!is_ascii_digit(endpoint[i])) return false;
    }
    memcpy(node_text, endpoint, node_length);
    node_text[node_length] = '\0';
    char* end = NULL;
    long parsed_node = strtol(node_text, &end, 10);
    if (!end || *end || parsed_node < 1 || parsed_node > INT_MAX) return false;
    size_t host_length = (size_t)(colon - at - 1);
    if (!host_length) return false;
    memcpy(host, at + 1, host_length);
    host[host_length] = '\0';
    for (const char* digit = colon + 1; *digit; digit++) {
        if (!is_ascii_digit(*digit)) return false;
    }
    long parsed_port = strtol(colon + 1, &end, 10);
    if (!colon[1] || !end || *end || parsed_port < 1 || parsed_port > 65535) return false;
    struct in_addr peer_address;
    if (inet_pton(AF_INET, host, &peer_address) != 1) return false;
    *node_id = (int)parsed_node;
    *port = (int)parsed_port;
    return true;
}

static bool is_configured_peer_address(struct in_addr address) {
    for (int i = 0; i < g_peer_count; i++) {
        char host[64];
        int node_id = 0, port = 0;
        if (!parse_peer_endpoint(g_peer_addrs[i], &node_id, host, sizeof(host), &port)) continue;
        struct in_addr peer_address;
        if (inet_pton(AF_INET, host, &peer_address) == 1 &&
            peer_address.s_addr == address.s_addr) return true;
    }
    return false;
}

static bool is_configured_peer(struct in_addr address, int expected_node_id) {
    for (int i = 0; i < g_peer_count; i++) {
        char host[64];
        int node_id = 0, port = 0;
        if (!parse_peer_endpoint(g_peer_addrs[i], &node_id, host, sizeof(host), &port)) continue;
        struct in_addr peer_address;
        if (inet_pton(AF_INET, host, &peer_address) == 1 &&
            peer_address.s_addr == address.s_addr && node_id == expected_node_id) {
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
static DWORD WINAPI cluster_follower_worker(LPVOID unused) {
#else
static void* cluster_follower_worker(void* unused) {
#endif
    (void)unused;
    while (atomic_load(&g_cluster_running)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_cluster_listener, &fds);
        struct timeval tv = {0, 200000};
        int sel = select((int)g_cluster_listener + 1, &fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(g_cluster_listener, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = (socklen_t)sizeof(client_addr);
            socket_t s = accept(g_cluster_listener, (struct sockaddr*)&client_addr, &addr_len);
            if (s != INVALID_SOCK) {
                if (!is_configured_peer_address(client_addr.sin_addr)) {
                    CLOSE_SOCK(s);
                    continue;
                }
                ClusterTlsStream* secure = cluster_stream_accept(s);
                RaftHeader hdr;
                if (secure && cluster_stream_recv_all(s, secure, &hdr, sizeof(hdr))) {
                    hdr = raft_header_from_wire(hdr);
                    int expected_node_id = (int)hdr.node_id;
                    if (hdr.magic == RAFT_MAGIC && hdr.msg_type == RAFT_MSG_SYNC_WAL &&
                        g_raft_role == RAFT_FOLLOWER && hdr.node_id != (uint32_t)g_raft_node_id &&
                        is_configured_peer(client_addr.sin_addr, expected_node_id) &&
                        cluster_stream_auth_matches(secure, expected_node_id)) {
                        if (hdr.payload_len > 0 && hdr.payload_len <= MAX_IMAGE) {
                            unsigned char* payload = malloc(hdr.payload_len);
                            if (payload && cluster_stream_recv_all(s, secure, payload, hdr.payload_len)) {
                                Image image = { payload, hdr.payload_len, hdr.payload_len, 0, true };
                                Image validation = image;
                                uint64_t identity = 0, generation = 0;
                                Database* snapshot = decode_database_snapshot(&validation, &identity, &generation);
                                Database* working = snapshot ? copy_database(snapshot) : NULL;
                                bool accepted = false;
                                if (snapshot && working && hdr.term >= atomic_load(&g_current_term) && g_path[0]) {
                                    lock_engine_state();
                                    bool same_database = g_generation > 0
                                        ? identity == g_identity
                                        : (g_committed && g_committed->table_count == 0 && !g_pending);
                                    if (identity == g_identity && generation == g_generation) {
                                        lock_snapshot_io();
                                        accepted = snapshot_file_matches(&image, g_path);
                                        unlock_snapshot_io();
                                    } else if (same_database && generation > g_generation && !g_pending) {
                                        bool uncertain = false;
                                        lock_snapshot_io();
                                        accepted = publish_durable_snapshot(g_path, &image, true, &uncertain) &&
                                                   !uncertain;
                                        unlock_snapshot_io();
                                        if (accepted) {
                                            Database* old_db = g_db;
                                            Database* old_committed = g_committed;
                                            g_db = working;
                                            g_committed = snapshot;
                                            working = NULL;
                                            snapshot = NULL;
                                            g_generation = generation;
                                            g_identity = identity;
                                            g_pending = false;
                                            clear_all_savepoints();
                                            destroy_database(old_db);
                                            destroy_database(old_committed);
                                        }
                                    }
                                    if (accepted) {
                                        atomic_store(&g_current_term, hdr.term);
                                        atomic_store(&g_raft_role, RAFT_FOLLOWER);
                                    }
                                    unlock_engine_state();
                                }
                                if (snapshot) {
                                    destroy_database(snapshot);
                                }
                                if (working) {
                                    destroy_database(working);
                                }
                                if (accepted) {
                                    RaftHeader ack = raft_header_to_wire((RaftHeader){
                                        RAFT_MAGIC,
                                        RAFT_MSG_SYNC_ACK,
                                        atomic_load(&g_current_term),
                                        (uint32_t)g_raft_node_id,
                                        0
                                    });
#ifdef OPENDATA_TESTING
                                    if (!getenv("OPENDATA_TEST_CLUSTER_DROP_ACK"))
#endif
                                    cluster_stream_send_all(s, secure, &ack, sizeof(ack));
                                } else {
                                    fprintf(stderr, "WARNING: cluster node %d rejected snapshot from node %d: stale/conflicting state or durable publication failed\n",
                                                          g_raft_node_id, expected_node_id);
                                }
                            }
                            free(payload);
                        }
                    } else if (hdr.magic == RAFT_MAGIC && hdr.msg_type == RAFT_MSG_VOTE_REQ &&
                               is_configured_peer(client_addr.sin_addr, (int)hdr.node_id) &&
                               cluster_stream_auth_matches(secure, (int)hdr.node_id)) {
                        /* Grant at most one vote per term; a higher term forces
                           any leader to step down to avoid split-brain. */
                        uint32_t term = atomic_load(&g_current_term);
                        bool grant = false;
                        if (hdr.term > term) {
                            if (atomic_compare_exchange_strong(&g_current_term, &term, hdr.term)) {
                                atomic_store(&g_raft_role, RAFT_FOLLOWER);
                                atomic_store(&g_voted_for_term, hdr.term);
                                atomic_store(&g_voted_for_node, (int)hdr.node_id);
                                grant = true;
                                term = hdr.term;
                            }
                        } else if (hdr.term == term) {
                            if (atomic_load(&g_voted_for_term) != term ||
                                atomic_load(&g_voted_for_node) == (int)hdr.node_id) {
                                grant = true; /* not yet voted this term, or re-vote for same node */
                            }
                        }
                        RaftHeader rsp = raft_header_to_wire((RaftHeader){
                            RAFT_MAGIC,
                            RAFT_MSG_VOTE_RSP,
                            term,
                            (uint32_t)g_raft_node_id,
                            grant ? 1u : 0u
                        });
                        cluster_stream_send_all(s, secure, &rsp, sizeof(rsp));
                    }
                }
                cluster_stream_close(secure);
                CLOSE_SOCK(s);
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

bool cluster_init(const ClusterConfig* config) {
    if (!config || config->node_id <= 0 || config->port < 1 || config->port > 65535 ||
        config->peer_count < 1 || config->peer_count > 4 ||
        (config->sync_replication && !config->is_leader)) {
        return report_database_error("invalid cluster configuration");
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return report_database_error("cannot initialize cluster networking");
    }
    g_winsock_started = true;
#endif
    g_raft_node_id = config->node_id;
    g_sync_replication = config->sync_replication;
    g_peer_count = config->peer_count;
    memset(g_peer_addrs, 0, sizeof(g_peer_addrs));
    for (int i = 0; i < g_peer_count; i++) {
        int peer_node_id = 0, peer_port = 0;
        char peer_host[64];
        if (!config->peer_addrs[i][0] ||
            strlen(config->peer_addrs[i]) >= sizeof(g_peer_addrs[i])) {
            return report_database_error("invalid cluster peer address");
        }
        snprintf(g_peer_addrs[i], sizeof(g_peer_addrs[i]), "%s", config->peer_addrs[i]);
        if (!parse_peer_endpoint(g_peer_addrs[i], &peer_node_id, peer_host,
                                 sizeof(peer_host), &peer_port) ||
            peer_node_id == config->node_id) {
            return report_database_error("peer must use node-id@IPv4:port and identify another node");
        }
        for (int j = 0; j < i; j++) {
            if (!strcmp(g_peer_addrs[i], g_peer_addrs[j])) {
                return report_database_error("duplicate cluster peer address: %s", g_peer_addrs[i]);
            }
            int previous_node_id = 0, previous_port = 0;
            char previous_host[64];
            if (parse_peer_endpoint(g_peer_addrs[j], &previous_node_id, previous_host,
                                    sizeof(previous_host), &previous_port) &&
                previous_node_id == peer_node_id) {
                return report_database_error("duplicate cluster peer node ID: %d", peer_node_id);
            }
        }
    }
    if (!g_cluster_tls) {
#ifndef OPENDATA_TEST_CLUSTER_PLAINTEXT
        g_cluster_tls = cluster_tls_create(config->cert_path, config->key_path,
                                           config->ca_path, config->node_id);
        if (!g_cluster_tls) return false;
#endif
    }

    if (config->is_leader) {
        atomic_store(&g_raft_role, RAFT_LEADER);
    } else {
        atomic_store(&g_raft_role, RAFT_FOLLOWER);
    }

    if (config->port > 0) {
        g_cluster_listener = socket(AF_INET, SOCK_STREAM, 0);
        if (g_cluster_listener == INVALID_SOCK) {
            return report_database_error("cannot create cluster listener socket");
        }
        int opt = 1;
#ifdef _WIN32
        setsockopt(g_cluster_listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
        setsockopt(g_cluster_listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)config->port);
        sin.sin_addr.s_addr = INADDR_ANY;
        if (bind(g_cluster_listener, (struct sockaddr*)&sin, sizeof(sin)) != 0 ||
            listen(g_cluster_listener, 8) != 0) {
            CLOSE_SOCK(g_cluster_listener);
            g_cluster_listener = INVALID_SOCK;
            return report_database_error("cannot bind cluster listener on port %d", config->port);
        }
        atomic_store(&g_cluster_running, true);
#ifdef _WIN32
        g_cluster_thread = CreateThread(NULL, 0, cluster_follower_worker, NULL, 0, NULL);
        if (!g_cluster_thread) {
#else
        if (pthread_create(&g_cluster_thread, NULL, cluster_follower_worker, NULL) != 0) {
#endif
            atomic_store(&g_cluster_running, false);
            CLOSE_SOCK(g_cluster_listener);
            g_cluster_listener = INVALID_SOCK;
            return report_database_error("cannot start cluster listener thread");
        }
    }
    return true;
}

bool raft_ship_wal_sync(const void* data, size_t len) {
    if (!g_sync_replication || g_peer_count == 0) {
        return true;
    }
    if (!data || len == 0 || len > MAX_IMAGE || len > UINT32_MAX) {
        return report_database_error("invalid replicated snapshot payload");
    }

    int acks = 1; /* Leader counts as 1 ack */
    uint32_t acked_nodes[5] = { (uint32_t)g_raft_node_id, 0, 0, 0, 0 };
    int total_nodes = g_peer_count + 1;
    int quorum_required = (total_nodes / 2) + 1;

    RaftHeader hdr = raft_header_to_wire((RaftHeader){
        RAFT_MAGIC,
        RAFT_MSG_SYNC_WAL,
        atomic_load(&g_current_term),
        (uint32_t)g_raft_node_id,
        (uint32_t)len
    });

    for (int i = 0; i < g_peer_count; i++) {
        char host[64];
        int expected_node_id = 0, port = 0;
        if (!parse_peer_endpoint(g_peer_addrs[i], &expected_node_id, host, sizeof(host), &port)) continue;

        socket_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCK) continue;

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, host, &dest.sin_addr);

        /* Bound the connect so one blackholed peer cannot stall the entire
           single-threaded server for minutes. */
        if (raft_connect_with_timeout(s, &dest, RAFT_PEER_CONNECT_TIMEOUT_MS)) {
            ClusterTlsStream* secure = cluster_stream_connect(s, host, expected_node_id);
            if (secure && cluster_stream_send_all(s, secure, &hdr, sizeof(hdr)) &&
                cluster_stream_send_all(s, secure, data, len)) {
                RaftHeader ack_rsp;
                if (cluster_stream_recv_all(s, secure, &ack_rsp, sizeof(ack_rsp))) {
                    ack_rsp = raft_header_from_wire(ack_rsp);
                    if (ack_rsp.magic == RAFT_MAGIC && ack_rsp.msg_type == RAFT_MSG_SYNC_ACK &&
                        ack_rsp.term == atomic_load(&g_current_term) &&
                        ack_rsp.node_id == (uint32_t)expected_node_id &&
                        ack_rsp.node_id != (uint32_t)g_raft_node_id) {
                        bool duplicate = false;
                        for (int node = 0; node < acks; node++) {
                            if (acked_nodes[node] == ack_rsp.node_id) duplicate = true;
                        }
                        if (!duplicate) acked_nodes[acks++] = ack_rsp.node_id;
                    } else {
                        fprintf(stderr, "WARNING: cluster node %d rejected invalid acknowledgment from peer %d\n",
                                              g_raft_node_id, expected_node_id);
                    }
                } else {
                    fprintf(stderr, "WARNING: cluster node %d did not receive acknowledgment from peer %d: timeout or disconnected transport\n",
                                          g_raft_node_id, expected_node_id);
                }
            } else {
                fprintf(stderr, "WARNING: cluster node %d could not authenticate or send snapshot to peer %d\n",
                                      g_raft_node_id, expected_node_id);
            }
            cluster_stream_close(secure);
        } else {
            fprintf(stderr, "WARNING: cluster node %d could not connect to peer %d at %s:%d\n",
                                  g_raft_node_id, expected_node_id, host, port);
        }
        CLOSE_SOCK(s);
    }

    if (acks < quorum_required) {
        return report_database_error("synchronous replication quorum failed: %d/%d nodes acknowledged", acks, quorum_required);
    }
    return true;
}

void cluster_shutdown(void) {
    g_sync_replication = false;
    g_peer_count = 0;
    atomic_store(&g_raft_role, RAFT_LEADER);
    if (atomic_load(&g_cluster_running)) {
        atomic_store(&g_cluster_running, false);
        if (g_cluster_listener != INVALID_SOCK) {
            CLOSE_SOCK(g_cluster_listener);
            g_cluster_listener = INVALID_SOCK;
        }
#ifdef _WIN32
        if (g_cluster_thread) {
            WaitForSingleObject(g_cluster_thread, 1000);
            CloseHandle(g_cluster_thread);
            g_cluster_thread = NULL;
        }
#else
        pthread_join(g_cluster_thread, NULL);
#endif
    }
    cluster_tls_destroy(g_cluster_tls);
    g_cluster_tls = NULL;
#ifdef _WIN32
    if (g_winsock_started) {
        WSACleanup();
        g_winsock_started = false;
    }
#endif
}

void print_cluster_status(void) {
    const char* role_str = (g_raft_role == RAFT_LEADER) ? "LEADER" :
                           (g_raft_role == RAFT_CANDIDATE) ? "CANDIDATE" : "FOLLOWER";
    int total_nodes = g_peer_count + 1;
    int quorum = (total_nodes / 2) + 1;
    printf("Cluster Node ID:         %d\n", g_raft_node_id);
    printf("Raft Role:               %s\n", role_str);
    printf("Current Term:            %u\n", (unsigned)g_current_term);
    printf("Sync WAL Replication:    %s\n", g_sync_replication ? "ENABLED (Zero Data Loss)" : "DISABLED (Standalone)");
    printf("Configured Peers:        %d\n", g_peer_count);
    for (int i = 0; i < g_peer_count; i++) {
        printf("  Peer %d:                %s\n", i + 1, g_peer_addrs[i]);
    }
    printf("Quorum Requirement:      %d/%d nodes\n", quorum, total_nodes);
    printf("Listener Daemon:         %s\n", atomic_load(&g_cluster_running) ? "ACTIVE" : "INACTIVE");
}
