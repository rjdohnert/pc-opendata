#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "database_engine.h"
#include <assert.h>
#include <limits.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
typedef SOCKET test_socket_t;
#define close closesocket
#define TEST_THREAD_RETURN DWORD WINAPI
#define TEST_THREAD_DONE 0
typedef HANDLE test_thread_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <openssl/ssl.h>
typedef int test_socket_t;
#define TEST_THREAD_RETURN void*
#define TEST_THREAD_DONE NULL
typedef pthread_t test_thread_t;
#endif

typedef struct {
    ClusterTlsContext* context;
    test_socket_t listener;
    bool handshake_succeeded;
} TlsPeer;

static TEST_THREAD_RETURN tls_peer_worker(void* argument) {
    TlsPeer* peer = argument;
    test_socket_t socket = accept(peer->listener, NULL, NULL);
    if (socket == (test_socket_t)-1) return TEST_THREAD_DONE;
    ClusterTlsStream* stream = cluster_tls_accept(peer->context, (intptr_t)socket);
    if (stream) {
        peer->handshake_succeeded = true;
        if (cluster_tls_peer_node_id(stream) == 1) {
            char request[5];
            if (cluster_tls_recv_all(stream, request, sizeof(request)) &&
                !memcmp(request, "hello", sizeof(request))) {
                cluster_tls_send_all(stream, "world", 5);
            }
        }
        cluster_tls_close(stream);
    }
    close(socket);
    return TEST_THREAD_DONE;
}

static void start_peer(test_thread_t* thread, TlsPeer* peer) {
#ifdef _WIN32
    *thread = CreateThread(NULL, 0, tls_peer_worker, peer, 0, NULL);
    assert(*thread);
#else
    assert(pthread_create(thread, NULL, tls_peer_worker, peer) == 0);
#endif
}

static void join_peer(test_thread_t thread) {
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
#else
    assert(pthread_join(thread, NULL) == 0);
#endif
}

static test_socket_t create_listener(struct sockaddr_in* address) {
    test_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener != (test_socket_t)-1);
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address->sin_port = 0;
    assert(bind(listener, (struct sockaddr*)address, sizeof(*address)) == 0);
    socklen_t address_length = sizeof(*address);
    assert(getsockname(listener, (struct sockaddr*)address, &address_length) == 0);
    assert(listen(listener, 1) == 0);
    return listener;
}

static void test_authenticated_tls_exchange(const char* ca, const char* client_cert,
                                            const char* client_key, const char* server_cert,
                                            const char* server_key) {
    ClusterTlsContext* server = cluster_tls_create(server_cert, server_key, ca, 2);
    ClusterTlsContext* client = cluster_tls_create(client_cert, client_key, ca, 1);
    assert(server && client);

    struct sockaddr_in address;
    test_socket_t listener = create_listener(&address);
    TlsPeer peer = {server, listener, false};
    test_thread_t thread;
    start_peer(&thread, &peer);

    test_socket_t client_socket = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_socket != (test_socket_t)-1);
    assert(connect(client_socket, (struct sockaddr*)&address, sizeof(address)) == 0);
    ClusterTlsStream* stream = cluster_tls_connect(client, (intptr_t)client_socket, "127.0.0.1", 2);
    assert(stream);
    assert(cluster_tls_peer_node_id(stream) == 2);
    assert(cluster_tls_send_all(stream, "hello", 5));
    char response[5];
    assert(cluster_tls_recv_all(stream, response, sizeof(response)));
    assert(!memcmp(response, "world", sizeof(response)));
    cluster_tls_close(stream);
    close(client_socket);

    join_peer(thread);
    assert(peer.handshake_succeeded);
    close(listener);
    cluster_tls_destroy(client);
    cluster_tls_destroy(server);
}

static void test_peer_rejected(const char* ca, const char* client_cert,
                              const char* client_key, const char* server_cert,
                              const char* server_key, const char* client_ca,
                              const char* peer_ip, int peer_id,
                              bool rejected_on_server) {
    ClusterTlsContext* server = cluster_tls_create(server_cert, server_key, ca, 2);
    ClusterTlsContext* client = cluster_tls_create(client_cert, client_key, client_ca, 1);
    assert(server && client);

    struct sockaddr_in address;
    test_socket_t listener = create_listener(&address);
    TlsPeer peer = {server, listener, false};
    test_thread_t thread;
    start_peer(&thread, &peer);

    test_socket_t client_socket = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_socket != (test_socket_t)-1);
    assert(connect(client_socket, (struct sockaddr*)&address, sizeof(address)) == 0);
    ClusterTlsStream* stream = cluster_tls_connect(client, (intptr_t)client_socket, peer_ip, peer_id);
    if (rejected_on_server) {
        if (stream) cluster_tls_close(stream);
    } else {
        assert(!stream);
    }
    close(client_socket);

    join_peer(thread);
    /* Client-side rejection timing is protocol-dependent: TLS 1.3 aborts before the server
       finishes, while Schannel's deferred TLS 1.2 validation rejects after it. */
    if (rejected_on_server) assert(!peer.handshake_succeeded);
    close(listener);
    cluster_tls_destroy(client);
    cluster_tls_destroy(server);
}

#ifndef _WIN32
static void test_missing_client_certificate_rejected(const char* ca,
                                                     const char* server_cert,
                                                     const char* server_key) {
    ClusterTlsContext* server = cluster_tls_create(server_cert, server_key, ca, 2);
    assert(server);
    SSL_CTX* client = SSL_CTX_new(TLS_client_method());
    assert(client);
    assert(SSL_CTX_load_verify_locations(client, ca, NULL) == 1);
    SSL_CTX_set_verify(client, SSL_VERIFY_PEER, NULL);

    struct sockaddr_in address;
    int listener = create_listener(&address);
    TlsPeer peer = {server, listener, false};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, tls_peer_worker, &peer) == 0);
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_socket >= 0);
    assert(connect(client_socket, (struct sockaddr*)&address, sizeof(address)) == 0);
    SSL* stream = SSL_new(client);
    assert(stream);
    assert(SSL_set_fd(stream, client_socket) == 1);
    assert(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(stream), "127.0.0.1") == 1);
    if (SSL_connect(stream) == 1) {
        /* TLS 1.3 clients can finish before receiving the server's certificate-required alert. */
        SSL_write(stream, "hello", 5);
        char response[5];
        assert(SSL_read(stream, response, sizeof(response)) <= 0);
    }
    SSL_free(stream);
    close(client_socket);
    assert(pthread_join(thread, NULL) == 0);
    assert(!peer.handshake_succeeded);
    close(listener);
    SSL_CTX_free(client);
    cluster_tls_destroy(server);
}
#endif

#ifdef _WIN32
static bool socket_send_all(test_socket_t socket, const void* data, size_t length) {
    const char* bytes = data;
    while (length) {
        int sent = send(socket, bytes, (int)(length > INT_MAX ? INT_MAX : length), 0);
        if (sent <= 0) return false;
        bytes += sent;
        length -= (size_t)sent;
    }
    return true;
}

static void test_missing_client_certificate_rejected(const char* server_cert) {
    ClusterTlsContext* server = cluster_tls_create(server_cert, NULL, NULL, 2);
    assert(server);
    struct sockaddr_in address;
    test_socket_t listener = create_listener(&address);
    TlsPeer peer = {server, listener, false};
    test_thread_t thread;
    start_peer(&thread, &peer);
    test_socket_t client_socket = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_socket != INVALID_SOCKET);
    assert(connect(client_socket, (struct sockaddr*)&address, sizeof(address)) == 0);

    SCHANNEL_CRED credentials;
    memset(&credentials, 0, sizeof(credentials));
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    credentials.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_DEFERRED_CRED_VALIDATION;
    CredHandle credential_handle;
    TimeStamp expiry;
    assert(AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
        NULL, &credentials, NULL, NULL, &credential_handle, &expiry) == SEC_E_OK);
    CtxtHandle context;
    bool context_valid = false;
    unsigned char encrypted[65536];
    size_t encrypted_length = 0;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        SecBuffer input[2] = {
            {(unsigned long)encrypted_length, SECBUFFER_TOKEN, encrypted},
            {0, SECBUFFER_EMPTY, NULL}
        };
        SecBufferDesc input_desc = {SECBUFFER_VERSION, 2, input};
        SecBuffer output = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc output_desc = {SECBUFFER_VERSION, 1, &output};
        ULONG attributes = 0;
        SECURITY_STATUS status = InitializeSecurityContextA(&credential_handle,
            context_valid ? &context : NULL, "127.0.0.1",
            ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
            0, SECURITY_NATIVE_DREP, encrypted_length ? &input_desc : NULL, 0,
            &context, &output_desc, &attributes, &expiry);
        if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
            assert(CompleteAuthToken(&context, &output_desc) == SEC_E_OK);
            status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }
        context_valid = status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED ||
                        status == SEC_E_INCOMPLETE_MESSAGE;
        if (output.pvBuffer) {
            assert(socket_send_all(client_socket, output.pvBuffer, output.cbBuffer));
            FreeContextBuffer(output.pvBuffer);
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED &&
            status != SEC_E_INCOMPLETE_MESSAGE) break;
        size_t extra = 0;
        if (encrypted_length) {
            for (size_t i = 0; i < 2; i++) {
                if (input[i].BufferType == SECBUFFER_EXTRA) extra = input[i].cbBuffer;
            }
        }
        if (extra <= encrypted_length && extra) {
            memmove(encrypted, encrypted + encrypted_length - extra, extra);
        }
        encrypted_length = extra;
        if (status == SEC_E_OK) break;
        if (encrypted_length == sizeof(encrypted)) break;
        int received = recv(client_socket, (char*)encrypted + encrypted_length,
                            (int)(sizeof(encrypted) - encrypted_length), 0);
        if (received <= 0) break;
        encrypted_length += (size_t)received;
    }
    closesocket(client_socket);
    join_peer(thread);
    assert(!peer.handshake_succeeded);
    if (context_valid) DeleteSecurityContext(&context);
    FreeCredentialsHandle(&credential_handle);
    closesocket(listener);
    cluster_tls_destroy(server);
}
#endif

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    const char* ca = getenv("OPENDATA_TEST_CLUSTER_CA");
    const char* client_cert = getenv("OPENDATA_TEST_CLUSTER_NODE1_CERT");
    const char* client_key = getenv("OPENDATA_TEST_CLUSTER_NODE1_KEY");
    const char* server_cert = getenv("OPENDATA_TEST_CLUSTER_NODE2_CERT");
    const char* server_key = getenv("OPENDATA_TEST_CLUSTER_NODE2_KEY");
    assert(client_cert && server_cert);
    test_authenticated_tls_exchange(ca, client_cert, client_key, server_cert, server_key);
    test_peer_rejected(ca, client_cert, client_key, server_cert, server_key, ca,
                       "127.0.0.1", 3, false);
    test_peer_rejected(ca, client_cert, client_key, server_cert, server_key, ca,
                       "127.0.0.2", 2, false);
#ifdef _WIN32
    const char* untrusted_cert = getenv("OPENDATA_TEST_CLUSTER_UNTRUSTED_CERT");
    const char* expired_cert = getenv("OPENDATA_TEST_CLUSTER_EXPIRED_CERT");
    const char* revoked_cert = getenv("OPENDATA_TEST_CLUSTER_REVOKED_CERT");
    assert(untrusted_cert && expired_cert && revoked_cert && ca);
    test_peer_rejected(ca, client_cert, client_key, untrusted_cert, server_key, ca,
                       "127.0.0.1", 2, false);
    test_peer_rejected(ca, revoked_cert, client_key, server_cert, server_key, ca,
                       "127.0.0.1", 2, true);
    assert(!cluster_tls_create(expired_cert, NULL, NULL, 2));
    test_missing_client_certificate_rejected(server_cert);
    assert(WSACleanup() == 0);
#else
    const char* untrusted_ca = getenv("OPENDATA_TEST_CLUSTER_UNTRUSTED_CA");
    assert(ca && client_key && server_key && untrusted_ca);
    test_peer_rejected(ca, client_cert, client_key, server_cert, server_key, untrusted_ca,
                       "127.0.0.1", 2, false);
    test_missing_client_certificate_rejected(ca, server_cert, server_key);
#endif
    puts("Cluster mutual-TLS identity and encrypted exchange tests passed.");
    return 0;
}
