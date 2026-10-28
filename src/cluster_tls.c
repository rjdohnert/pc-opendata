#include "database_engine.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#ifdef OPENDATA_TEST_TLS_FIXTURES
#include <ncrypt.h>
#include "../tests/cluster_tls_test_fixture.h"
#endif
typedef SOCKET cluster_socket_t;
#define CLUSTER_INVALID_SOCKET INVALID_SOCKET
#define CLUSTER_CLOSE_SOCKET(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(OPENDATA_TLS)
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif
typedef int cluster_socket_t;
#define CLUSTER_INVALID_SOCKET (-1)
#define CLUSTER_CLOSE_SOCKET(s) close(s)
#endif

#define CLUSTER_TLS_IO_TIMEOUT_MS 5000
#define CLUSTER_TLS_BUFFER_SIZE 65536

#ifdef _WIN32
struct ClusterTlsContext {
    CredHandle inbound;
    CredHandle outbound;
    bool inbound_valid;
    bool outbound_valid;
    PCCERT_CONTEXT certificate;
#ifdef OPENDATA_TEST_TLS_FIXTURES
    OpendataTestTlsFixture fixture;
#endif
};

struct ClusterTlsStream {
    cluster_socket_t socket;
    CtxtHandle context;
    bool context_valid;
    SecPkgContext_StreamSizes sizes;
    PCCERT_CONTEXT peer_certificate;
    unsigned char encrypted[CLUSTER_TLS_BUFFER_SIZE];
    size_t encrypted_len;
    unsigned char plaintext[CLUSTER_TLS_BUFFER_SIZE];
    size_t plaintext_len;
    size_t plaintext_offset;
};

static bool cluster_set_socket_timeout(cluster_socket_t socket) {
    DWORD timeout = CLUSTER_TLS_IO_TIMEOUT_MS;
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == 0;
}

#ifndef OPENDATA_TEST_TLS_FIXTURES
static PCCERT_CONTEXT cluster_find_windows_certificate(const char* setting) {
    if (!setting || strncmp(setting, "store:", 6) || strlen(setting + 6) != 40) return NULL;
    BYTE thumbprint[20];
    for (size_t i = 0; i < sizeof(thumbprint); i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            char ch = uppercase_ascii(setting[6 + i * 2 + j]);
            if (!is_ascii_digit(ch) && (ch < 'A' || ch > 'F')) return NULL;
            value = value * 16u + (unsigned)(is_ascii_digit(ch) ? ch - '0' : ch - 'A' + 10);
        }
        thumbprint[i] = (BYTE)value;
    }
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG,
        L"MY");
    if (!store) return NULL;
    CRYPT_HASH_BLOB hash = {(DWORD)sizeof(thumbprint), thumbprint};
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING, 0, CERT_FIND_HASH, &hash, NULL);
    CertCloseStore(store, 0);
    if (certificate && CertVerifyTimeValidity(NULL, certificate->pCertInfo) != 0) {
        CertFreeCertificateContext(certificate);
        return NULL;
    }
    return certificate;
}
#endif

#ifdef OPENDATA_TEST_TLS_FIXTURES
static bool cluster_fixture_read_file(const char* path, BYTE** contents, DWORD* length) {
    FILE* file = path ? fopen(path, "rb") : NULL;
    if (!file) return false;
    bool success = false;
    if (_fseeki64(file, 0, SEEK_END) == 0) {
        __int64 size = _ftelli64(file);
        if (size > 0 && size <= MAXDWORD && _fseeki64(file, 0, SEEK_SET) == 0) {
            BYTE* buffer = malloc((size_t)size);
            if (buffer && fread(buffer, 1, (size_t)size, file) == (size_t)size) {
                *contents = buffer;
                *length = (DWORD)size;
                success = true;
            } else {
                free(buffer);
            }
        }
    }
    fclose(file);
    return success;
}

static bool cluster_fixture_record_key_name(PCCERT_CONTEXT certificate) {
    const char* log_path = getenv("OPENDATA_TEST_CLUSTER_KEY_LOG");
    if (!log_path || !*log_path) return false;
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID,
                                           NULL, &size) || !size) return false;
    CRYPT_KEY_PROV_INFO* info = malloc(size);
    if (!info) return false;
    bool recorded = false;
    if (CertGetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID,
                                          info, &size) &&
        info->pwszContainerName && info->pwszProvName) {
        int provider_size = WideCharToMultiByte(CP_UTF8, 0, info->pwszProvName, -1,
                                                NULL, 0, NULL, NULL);
        int container_size = WideCharToMultiByte(CP_UTF8, 0, info->pwszContainerName, -1,
                                                 NULL, 0, NULL, NULL);
        if (provider_size > 1 && container_size > 1) {
            size_t line_size = (size_t)provider_size + (size_t)container_size;
            char* line = malloc(line_size);
            if (line &&
                WideCharToMultiByte(CP_UTF8, 0, info->pwszProvName, -1,
                                    line, provider_size, NULL, NULL) &&
                WideCharToMultiByte(CP_UTF8, 0, info->pwszContainerName, -1,
                                    line + provider_size, container_size, NULL, NULL)) {
                line[provider_size - 1] = '\t';
                line[line_size - 1] = '\n';
                HANDLE file = CreateFileA(log_path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    recorded = WriteFile(file, line, (DWORD)line_size, &written, NULL) &&
                               written == line_size;
                    CloseHandle(file);
                }
            }
            free(line);
        }
    }
    free(info);
    return recorded;
}

static PCCERT_CONTEXT cluster_fixture_load_identity(const char* path, HCERTSTORE* store) {
    BYTE* encoded = NULL;
    DWORD encoded_length = 0;
    if (!cluster_fixture_read_file(path, &encoded, &encoded_length)) return NULL;
    CRYPT_DATA_BLOB pfx = {encoded_length, encoded};
    *store = PFXImportCertStore(&pfx, L"", PKCS12_ALWAYS_CNG_KSP);
    SecureZeroMemory(encoded, encoded_length);
    free(encoded);
    return *store ? CertEnumCertificatesInStore(*store, NULL) : NULL;
}

bool opendata_test_tls_fixture_identity_open(const char* pfx_path,
                                            OpendataTestTlsFixture* fixture) {
    if (!fixture) return false;
    memset(fixture, 0, sizeof(*fixture));
    const char* log_path = getenv("OPENDATA_TEST_CLUSTER_KEY_LOG");
    if (!log_path || !*log_path)
        return report_database_error("isolated TLS fixture requires a key cleanup ledger");
    fixture->identity_certificate = cluster_fixture_load_identity(pfx_path, &fixture->identity_store);
    if (fixture->identity_certificate) {
        DWORD size = 0;
        fixture->identity_has_key = CertGetCertificateContextProperty(
            fixture->identity_certificate, CERT_KEY_PROV_INFO_PROP_ID, NULL, &size) != FALSE;
        if (!fixture->identity_has_key && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND)
            return true;
        if (fixture->identity_has_key &&
            cluster_fixture_record_key_name(fixture->identity_certificate)) return true;
        report_database_error("cannot read or record isolated TLS fixture private-key metadata");
    } else {
        report_database_error("cannot import isolated TLS fixture PFX (Windows error 0x%08lx)",
                              (unsigned long)GetLastError());
    }
    opendata_test_tls_fixture_close(fixture);
    return false;
}

HCERTSTORE opendata_test_tls_fixture_wallet_store(OpendataTestTlsFixture* fixture) {
    const char* path = getenv("OPENDATA_TEST_WALLET_PFX");
    if (!opendata_test_tls_fixture_identity_open(path, fixture)) return NULL;
    return fixture->identity_store;
}

bool opendata_test_tls_fixture_open(const char* pfx_path, const char* root_der_path,
                                    const char* crl_der_path, OpendataTestTlsFixture* fixture) {
    if (!opendata_test_tls_fixture_identity_open(pfx_path, fixture)) return false;
    BYTE* encoded = NULL;
    DWORD encoded_length = 0;
    if (!cluster_fixture_read_file(root_der_path, &encoded, &encoded_length)) goto fail;
    PCCERT_CONTEXT root = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded, encoded_length);
    free(encoded);
    encoded = NULL;
    if (!root) goto fail;
    fixture->root_store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                        CERT_STORE_CREATE_NEW_FLAG, NULL);
    bool valid = fixture->root_store &&
                 CertAddCertificateContextToStore(fixture->root_store, root,
                                                  CERT_STORE_ADD_ALWAYS, NULL);
    CertFreeCertificateContext(root);
    if (!valid || !cluster_fixture_read_file(crl_der_path, &encoded, &encoded_length)) goto fail;

    PCCRL_CONTEXT crl = CertCreateCRLContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                             encoded, encoded_length);
    free(encoded);
    encoded = NULL;
    if (!crl) goto fail;
    valid = CertAddCRLContextToStore(fixture->identity_store, crl, CERT_STORE_ADD_ALWAYS, NULL);
    CertFreeCRLContext(crl);
    if (!valid) goto fail;

    CERT_CHAIN_ENGINE_CONFIG engine;
    memset(&engine, 0, sizeof(engine));
    engine.cbSize = sizeof(engine);
    engine.hExclusiveRoot = fixture->root_store;
    engine.dwFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA;
    if (!CertCreateCertificateChainEngine(&engine, &fixture->chain_engine)) goto fail;
    return true;

fail:
    free(encoded);
    opendata_test_tls_fixture_close(fixture);
    return false;
}

void opendata_test_tls_fixture_close(OpendataTestTlsFixture* fixture) {
    if (!fixture) return;
    if (fixture->chain_engine) CertFreeCertificateChainEngine(fixture->chain_engine);
    if (fixture->identity_certificate && fixture->identity_has_key) {
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
        DWORD key_spec = 0;
        BOOL caller_must_free = FALSE;
        if (CryptAcquireCertificatePrivateKey(fixture->identity_certificate,
                CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
                NULL, &key, &key_spec, &caller_must_free)) {
            if (key_spec == CERT_NCRYPT_KEY_SPEC) {
                if (NCryptDeleteKey((NCRYPT_KEY_HANDLE)key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) {
                    if (caller_must_free) NCryptFreeObject((NCRYPT_HANDLE)key);
                    report_database_error("could not delete isolated Schannel fixture CNG key");
                }
            } else {
                if (caller_must_free) CryptReleaseContext((HCRYPTPROV)key, 0);
                report_database_error("isolated Schannel fixture identity was not imported as a CNG key");
            }
        } else {
            report_database_error("could not acquire isolated Schannel fixture private key for cleanup");
        }
    }
    if (fixture->identity_certificate) CertFreeCertificateContext(fixture->identity_certificate);
    if (fixture->identity_store) CertCloseStore(fixture->identity_store, 0);
    if (fixture->root_store) CertCloseStore(fixture->root_store, 0);
    memset(fixture, 0, sizeof(*fixture));
}
#endif

static int cluster_certificate_node_id(PCCERT_CONTEXT certificate);

ClusterTlsContext* cluster_tls_create(const char* cert_path, const char* key_path,
                                      const char* ca_path, int local_node_id) {
#ifdef OPENDATA_TEST_TLS_FIXTURES
    (void)ca_path;
    if (!cert_path || key_path) {
        report_database_error("isolated Windows cluster TLS fixtures require a PFX --cert and no --key");
        return NULL;
    }
    ClusterTlsContext* context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    const char* root_path = getenv("OPENDATA_TEST_CLUSTER_CA");
    const char* crl_path = getenv("OPENDATA_TEST_CLUSTER_CRL");
    if (!opendata_test_tls_fixture_open(cert_path, root_path, crl_path, &context->fixture)) {
        report_database_error("cannot load isolated cluster TLS fixture identity, root, or CRL");
        cluster_tls_destroy(context);
        return NULL;
    }
    context->certificate = context->fixture.identity_certificate;
    if (CertVerifyTimeValidity(NULL, context->certificate->pCertInfo) != 0) {
        report_database_error("isolated cluster TLS fixture certificate is expired or not yet valid");
        cluster_tls_destroy(context);
        return NULL;
    }
    if (cluster_certificate_node_id(context->certificate) != local_node_id) {
        report_database_error("Windows cluster certificate common name must be opendata-node-%d", local_node_id);
        cluster_tls_destroy(context);
        return NULL;
    }
#else
    (void)ca_path;
    if (!cert_path || key_path || strncmp(cert_path, "store:", 6)) {
        report_database_error("Windows cluster TLS requires --cert store:<40-hex-thumbprint>; omit --key and trust the CA in CurrentUser\\Root");
        return NULL;
    }
    ClusterTlsContext* context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->certificate = cluster_find_windows_certificate(cert_path);
    if (!context->certificate) {
        report_database_error("cluster TLS certificate was not found or is invalid in CurrentUser\\My");
        cluster_tls_destroy(context);
        return NULL;
    }
    if (cluster_certificate_node_id(context->certificate) != local_node_id) {
        report_database_error("Windows cluster certificate common name must be opendata-node-%d", local_node_id);
        cluster_tls_destroy(context);
        return NULL;
    }
#endif
    SCHANNEL_CRED credentials;
    memset(&credentials, 0, sizeof(credentials));
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.cCreds = 1;
    credentials.paCred = &context->certificate;
#ifdef OPENDATA_TEST_TLS_FIXTURES
    credentials.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER;
#else
    credentials.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_REVOCATION_CHECK_CHAIN;
#endif
    credentials.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_2_SERVER;
    TimeStamp expiry;
    SECURITY_STATUS status = AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A,
        SECPKG_CRED_INBOUND, NULL, &credentials, NULL, NULL, &context->inbound, &expiry);
    if (status == SEC_E_OK) context->inbound_valid = true;
    if (status == SEC_E_OK) {
#ifdef OPENDATA_TEST_TLS_FIXTURES
        credentials.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_DEFERRED_CRED_VALIDATION;
#endif
        status = AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A,
            SECPKG_CRED_OUTBOUND, NULL, &credentials, NULL, NULL, &context->outbound, &expiry);
        if (status == SEC_E_OK) context->outbound_valid = true;
    }
    if (status != SEC_E_OK) {
        report_database_error("cannot acquire Schannel mutual-TLS credentials (status 0x%08lx)",
                              (unsigned long)status);
        cluster_tls_destroy(context);
        return NULL;
    }
    return context;
}

static bool cluster_send_socket(cluster_socket_t socket, const void* data, size_t length) {
    const char* bytes = data;
    size_t sent = 0;
    while (sent < length) {
        int n = send(socket, bytes + sent,
                     (int)((length - sent) > INT_MAX ? INT_MAX : length - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool cluster_receive_socket(cluster_socket_t socket, void* data, size_t capacity, size_t* received) {
    int n = recv(socket, (char*)data, (int)capacity, 0);
    if (n <= 0) return false;
    *received = (size_t)n;
    return true;
}

static bool cluster_preserve_extra(ClusterTlsStream* stream, SecBuffer* buffers, size_t count) {
    size_t extra = 0;
    for (size_t i = 0; i < count; i++) {
        if (buffers[i].BufferType == SECBUFFER_EXTRA) extra = buffers[i].cbBuffer;
    }
    if (extra > stream->encrypted_len) return false;
    if (extra) {
        memmove(stream->encrypted, stream->encrypted + stream->encrypted_len - extra, extra);
    }
    stream->encrypted_len = extra;
    return true;
}

static bool cluster_receive_encrypted(ClusterTlsStream* stream) {
    if (stream->encrypted_len == sizeof(stream->encrypted)) return false;
    size_t received = 0;
    if (!cluster_receive_socket(stream->socket,
            stream->encrypted + stream->encrypted_len,
            sizeof(stream->encrypted) - stream->encrypted_len, &received)) return false;
    stream->encrypted_len += received;
    return true;
}

#ifdef OPENDATA_TEST_TLS_FIXTURES
static bool cluster_fixture_check_revocation(ClusterTlsContext* context,
                                             PCCERT_CONTEXT certificate) {
    PCCERT_CONTEXT root = CertEnumCertificatesInStore(context->fixture.root_store, NULL);
    if (!root) return false;
    bool checked = false;
    PCCRL_CONTEXT crl = NULL;
    while ((crl = CertEnumCRLsInStore(context->fixture.identity_store, crl)) != NULL) {
        if (!CertCompareCertificateName(X509_ASN_ENCODING, &root->pCertInfo->Subject,
                                        &crl->pCrlInfo->Issuer)) continue;
        checked = true;
        bool valid = CertVerifyCRLTimeValidity(NULL, crl->pCrlInfo) == 0 &&
            CryptVerifyCertificateSignatureEx((HCRYPTPROV_LEGACY)0, X509_ASN_ENCODING,
                CRYPT_VERIFY_CERT_SIGN_SUBJECT_CRL, (void*)crl,
                CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void*)root, 0, NULL) != 0;
        if (valid && CertCompareCertificateName(X509_ASN_ENCODING,
                &certificate->pCertInfo->Issuer, &crl->pCrlInfo->Issuer)) {
            for (DWORD i = 0; i < crl->pCrlInfo->cCRLEntry; i++) {
                if (CertCompareIntegerBlob(&certificate->pCertInfo->SerialNumber,
                                           &crl->pCrlInfo->rgCRLEntry[i].SerialNumber)) {
                    valid = false;
                    fprintf(stderr, "WARNING: cluster TLS fixture CRL revoked peer certificate\n");
                    break;
                }
            }
        } else {
            valid = false;
        }
        CertFreeCRLContext(crl);
        CertFreeCertificateContext(root);
        return checked && valid;
    }
    if (crl) CertFreeCRLContext(crl);
    CertFreeCertificateContext(root);
    return false;
}
#endif

static bool cluster_validate_windows_peer(ClusterTlsContext* context,
                                          PCCERT_CONTEXT certificate, bool server_side,
                                          const char* peer_ip) {
    if (!certificate) return false;
    LPWSTR server_name = NULL;
    if (!server_side) {
        int count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, peer_ip, -1, NULL, 0);
        if (!count) return false;
        server_name = malloc((size_t)count * sizeof(*server_name));
        if (!server_name || !MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, peer_ip, -1, server_name, count)) {
            free(server_name);
            return false;
        }
    }
    CERT_CHAIN_PARA chain_parameters;
    memset(&chain_parameters, 0, sizeof(chain_parameters));
    chain_parameters.cbSize = sizeof(chain_parameters);
    PCCERT_CHAIN_CONTEXT chain = NULL;
#ifdef OPENDATA_TEST_TLS_FIXTURES
    DWORD chain_flags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL;
    bool valid = CertGetCertificateChain(context->fixture.chain_engine, certificate, NULL,
        certificate->hCertStore, &chain_parameters, chain_flags, NULL, &chain) != 0;
#else
    (void)context;
    bool valid = CertGetCertificateChain(NULL, certificate, NULL, certificate->hCertStore,
        &chain_parameters, CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, NULL, &chain) != 0;
#endif
    if (valid) {
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra;
        memset(&extra, 0, sizeof(extra));
        extra.cbSize = sizeof(extra);
        extra.dwAuthType = server_side ? AUTHTYPE_CLIENT : AUTHTYPE_SERVER;
        extra.pwszServerName = server_name;
        CERT_CHAIN_POLICY_PARA policy;
        memset(&policy, 0, sizeof(policy));
        policy.cbSize = sizeof(policy);
        policy.pvExtraPolicyPara = &extra;
        CERT_CHAIN_POLICY_STATUS status;
        memset(&status, 0, sizeof(status));
        status.cbSize = sizeof(status);
        valid = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) &&
                status.dwError == 0;
        if (!valid) {
            fprintf(stderr, "WARNING: cluster TLS certificate policy rejected peer (status 0x%08lx)\n",
                    (unsigned long)status.dwError);
        }
    }
#ifdef OPENDATA_TEST_TLS_FIXTURES
    if (valid && !cluster_fixture_check_revocation(context, certificate)) {
        fprintf(stderr, "WARNING: cluster TLS fixture CRL validation rejected peer\n");
        valid = false;
    }
#endif
    if (chain) CertFreeCertificateChain(chain);
    free(server_name);
    return valid;
}

static int cluster_certificate_node_id(PCCERT_CONTEXT certificate) {
    char name[128];
    DWORD length = CertGetNameStringA(certificate, CERT_NAME_ATTR_TYPE, 0,
                                      (void*)szOID_COMMON_NAME, name, sizeof(name));
    if (length <= 1 || length >= sizeof(name)) return 0;
    const char prefix[] = "opendata-node-";
    if (strncmp(name, prefix, sizeof(prefix) - 1)) return 0;
    char* end = NULL;
    long node = strtol(name + sizeof(prefix) - 1, &end, 10);
    if (!end || *end || node < 1 || node > INT_MAX) return 0;
    return (int)node;
}

static bool cluster_windows_handshake(ClusterTlsContext* tls, ClusterTlsStream* stream,
                                      bool server_side, const char* peer_ip) {
    bool have_input = false;
    bool complete = false;
    while (!complete) {
        if (!have_input && (server_side || stream->context_valid)) {
            if (!cluster_receive_encrypted(stream)) return false;
            have_input = true;
        }
        SecBuffer input[2] = {
            {(unsigned long)stream->encrypted_len, SECBUFFER_TOKEN, stream->encrypted},
            {0, SECBUFFER_EMPTY, NULL}
        };
        SecBufferDesc input_desc = {SECBUFFER_VERSION, 2, input};
        SecBuffer output = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc output_desc = {SECBUFFER_VERSION, 1, &output};
        unsigned long attributes = 0;
        TimeStamp expiry;
        SECURITY_STATUS status;
        ULONG request_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                              ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                              ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MUTUAL_AUTH;
        if (server_side) {
            status = AcceptSecurityContext(&tls->inbound,
                stream->context_valid ? &stream->context : NULL,
                &input_desc, ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH,
                SECURITY_NATIVE_DREP, &stream->context, &output_desc, &attributes, &expiry);
        } else {
            status = InitializeSecurityContextA(&tls->outbound,
                stream->context_valid ? &stream->context : NULL, (SEC_CHAR*)peer_ip,
                request_flags, 0, SECURITY_NATIVE_DREP,
                have_input ? &input_desc : NULL, 0, &stream->context, &output_desc,
                &attributes, &expiry);
        }
        if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
            if (CompleteAuthToken(&stream->context, &output_desc) != SEC_E_OK) {
                if (output.pvBuffer) FreeContextBuffer(output.pvBuffer);
                return false;
            }
            status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (output.pvBuffer) FreeContextBuffer(output.pvBuffer);
            have_input = false;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
            fprintf(stderr, "WARNING: cluster TLS %s handshake failed (Schannel status 0x%08lx)\n",
                    server_side ? "inbound" : "outbound", (unsigned long)status);
            if (output.pvBuffer) FreeContextBuffer(output.pvBuffer);
            return false;
        }
        stream->context_valid = true;
        if (output.pvBuffer) {
            bool sent = cluster_send_socket(stream->socket, output.pvBuffer, output.cbBuffer);
            FreeContextBuffer(output.pvBuffer);
            if (!sent) return false;
        }
        if (have_input && !cluster_preserve_extra(stream, input, 2)) return false;
        complete = status == SEC_E_OK;
        have_input = stream->encrypted_len != 0;
    }
    if (QueryContextAttributesA(&stream->context, SECPKG_ATTR_STREAM_SIZES,
                                &stream->sizes) != SEC_E_OK) return false;
    /* The record buffer is a fixed 64 KB stack allocation; refuse to continue
       if the negotiated record layout could overflow it. */
    if (stream->sizes.cbHeader + stream->sizes.cbMaximumMessage + stream->sizes.cbTrailer >
        CLUSTER_TLS_BUFFER_SIZE) return false;
    if (QueryContextAttributesA(&stream->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                &stream->peer_certificate) != SEC_E_OK) return false;
    if (!cluster_validate_windows_peer(tls, stream->peer_certificate, server_side, peer_ip)) return false;
    return true;
}

ClusterTlsStream* cluster_tls_accept(ClusterTlsContext* context, intptr_t socket) {
    if (!context || socket == (intptr_t)INVALID_SOCKET) return NULL;
    cluster_socket_t native_socket = (cluster_socket_t)socket;
    if (!cluster_set_socket_timeout(native_socket)) return NULL;
    ClusterTlsStream* stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->socket = native_socket;
    if (!cluster_windows_handshake(context, stream, true, NULL)) {
        cluster_tls_close(stream);
        return NULL;
    }
    return stream;
}

ClusterTlsStream* cluster_tls_connect(ClusterTlsContext* context, intptr_t socket,
                                      const char* peer_ip, int peer_node_id) {
    if (!context || socket == (intptr_t)INVALID_SOCKET || !peer_ip) return NULL;
    cluster_socket_t native_socket = (cluster_socket_t)socket;
    if (!cluster_set_socket_timeout(native_socket)) return NULL;
    ClusterTlsStream* stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->socket = native_socket;
    if (!cluster_windows_handshake(context, stream, false, peer_ip)) {
        cluster_tls_close(stream);
        return NULL;
    }
    if (cluster_tls_peer_node_id(stream) != peer_node_id) {
        fprintf(stderr, "WARNING: cluster TLS peer identity mismatch: expected node %d, received node %d\n",
                peer_node_id, cluster_tls_peer_node_id(stream));
        cluster_tls_close(stream);
        return NULL;
    }
    return stream;
}

int cluster_tls_peer_node_id(ClusterTlsStream* stream) {
    return stream ? cluster_certificate_node_id(stream->peer_certificate) : 0;
}

bool cluster_tls_send_all(ClusterTlsStream* stream, const void* data, size_t length) {
    if (!stream || !data) return false;
    /* Defense in depth: never let header + payload + trailer exceed the
       fixed-size record buffer, even if the handshake check was bypassed. */
    if (stream->sizes.cbHeader + stream->sizes.cbMaximumMessage + stream->sizes.cbTrailer >
        CLUSTER_TLS_BUFFER_SIZE) return false;
    const unsigned char* input = data;
    size_t sent = 0;
    while (sent < length) {
        size_t chunk = length - sent;
        if (chunk > stream->sizes.cbMaximumMessage) chunk = stream->sizes.cbMaximumMessage;
        unsigned char record[CLUSTER_TLS_BUFFER_SIZE];
        size_t header = stream->sizes.cbHeader;
        memcpy(record + header, input + sent, chunk);
        SecBuffer buffers[4] = {
            {(unsigned long)header, SECBUFFER_STREAM_HEADER, record},
            {(unsigned long)chunk, SECBUFFER_DATA, record + header},
            {stream->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record + header + chunk},
            {0, SECBUFFER_EMPTY, NULL}
        };
        SecBufferDesc description = {SECBUFFER_VERSION, 4, buffers};
        if (EncryptMessage(&stream->context, 0, &description, 0) != SEC_E_OK ||
            !cluster_send_socket(stream->socket, record,
                buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) return false;
        sent += chunk;
    }
    return true;
}

static bool cluster_decrypt_record(ClusterTlsStream* stream) {
    while (!stream->plaintext_len) {
        if (!stream->encrypted_len && !cluster_receive_encrypted(stream)) return false;
        SecBuffer buffers[4] = {
            {(unsigned long)stream->encrypted_len, SECBUFFER_DATA, stream->encrypted},
            {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}
        };
        SecBufferDesc description = {SECBUFFER_VERSION, 4, buffers};
        SECURITY_STATUS status = DecryptMessage(&stream->context, &description, 0, NULL);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (!cluster_receive_encrypted(stream)) return false;
            continue;
        }
        if (status != SEC_E_OK) return false;
        for (size_t i = 0; i < 4; i++) {
            if (buffers[i].BufferType == SECBUFFER_DATA && buffers[i].cbBuffer) {
                if (buffers[i].cbBuffer > sizeof(stream->plaintext)) return false;
                memcpy(stream->plaintext, buffers[i].pvBuffer, buffers[i].cbBuffer);
                stream->plaintext_len = buffers[i].cbBuffer;
                stream->plaintext_offset = 0;
                break;
            }
        }
        if (!cluster_preserve_extra(stream, buffers, 4)) return false;
        if (!stream->plaintext_len && !stream->encrypted_len) return false;
    }
    return true;
}

bool cluster_tls_recv_all(ClusterTlsStream* stream, void* data, size_t length) {
    if (!stream || (!data && length)) return false;
    unsigned char* output = data;
    size_t received = 0;
    while (received < length) {
        if (stream->plaintext_offset == stream->plaintext_len) {
            stream->plaintext_offset = stream->plaintext_len = 0;
            if (!cluster_decrypt_record(stream)) return false;
        }
        size_t available = stream->plaintext_len - stream->plaintext_offset;
        size_t count = length - received < available ? length - received : available;
        memcpy(output + received, stream->plaintext + stream->plaintext_offset, count);
        SecureZeroMemory(stream->plaintext + stream->plaintext_offset, count);
        stream->plaintext_offset += count;
        received += count;
    }
    return true;
}

void cluster_tls_close(ClusterTlsStream* stream) {
    if (!stream) return;
    if (stream->context_valid) DeleteSecurityContext(&stream->context);
    if (stream->peer_certificate) CertFreeCertificateContext(stream->peer_certificate);
    SecureZeroMemory(stream->plaintext, sizeof(stream->plaintext));
    free(stream);
}

void cluster_tls_destroy(ClusterTlsContext* context) {
    if (!context) return;
    if (context->inbound_valid) FreeCredentialsHandle(&context->inbound);
    if (context->outbound_valid) FreeCredentialsHandle(&context->outbound);
#ifdef OPENDATA_TEST_TLS_FIXTURES
    opendata_test_tls_fixture_close(&context->fixture);
#else
    if (context->certificate) CertFreeCertificateContext(context->certificate);
#endif
    SecureZeroMemory(context, sizeof(*context));
    free(context);
}

#elif defined(OPENDATA_TLS)

struct ClusterTlsContext {
    SSL_CTX* server;
    SSL_CTX* client;
};

struct ClusterTlsStream {
    cluster_socket_t socket;
    SSL* ssl;
    int peer_node_id;
};

static bool cluster_set_socket_timeout(cluster_socket_t socket) {
    struct timeval timeout = {CLUSTER_TLS_IO_TIMEOUT_MS / 1000,
                              (CLUSTER_TLS_IO_TIMEOUT_MS % 1000) * 1000};
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

static bool cluster_load_identity(SSL_CTX* context, const char* cert_path, const char* key_path) {
    return cert_path && key_path &&
           SSL_CTX_use_certificate_chain_file(context, cert_path) == 1 &&
           SSL_CTX_use_PrivateKey_file(context, key_path, SSL_FILETYPE_PEM) == 1 &&
           SSL_CTX_check_private_key(context) == 1;
}

static int cluster_cn_node_id(X509* certificate);

ClusterTlsContext* cluster_tls_create(const char* cert_path, const char* key_path,
                                      const char* ca_path, int local_node_id) {
    if (!cert_path || !key_path || !ca_path) {
        report_database_error("POSIX cluster mutual TLS requires --cert, --key, and --cluster-ca");
        return NULL;
    }
    ClusterTlsContext* context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->server = SSL_CTX_new(TLS_server_method());
    context->client = SSL_CTX_new(TLS_client_method());
    if (!context->server || !context->client) goto fail;
    SSL_CTX_set_min_proto_version(context->server, TLS1_2_VERSION);
    SSL_CTX_set_min_proto_version(context->client, TLS1_2_VERSION);
    SSL_CTX_set_options(context->server, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_options(context->client, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (!cluster_load_identity(context->server, cert_path, key_path) ||
        !cluster_load_identity(context->client, cert_path, key_path) ||
        SSL_CTX_load_verify_locations(context->server, ca_path, NULL) != 1 ||
        SSL_CTX_load_verify_locations(context->client, ca_path, NULL) != 1 ||
        SSL_CTX_set_purpose(context->server, X509_PURPOSE_SSL_CLIENT) != 1 ||
        SSL_CTX_set_purpose(context->client, X509_PURPOSE_SSL_SERVER) != 1) goto fail;
    if (cluster_cn_node_id(SSL_CTX_get0_certificate(context->server)) != local_node_id) {
        report_database_error("cluster certificate common name must be opendata-node-%d", local_node_id);
        goto fail;
    }
    SSL_CTX_set_verify(context->server, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify(context->client, SSL_VERIFY_PEER, NULL);
    /* Honor CRLs published in the trust store, matching the Windows Schannel path. */
    {
        X509_STORE* server_store = SSL_CTX_get_cert_store(context->server);
        X509_STORE* client_store = SSL_CTX_get_cert_store(context->client);
        if (server_store) X509_STORE_set_flags(server_store, X509_V_FLAG_CRL_CHECK);
        if (client_store) X509_STORE_set_flags(client_store, X509_V_FLAG_CRL_CHECK);
    }
    return context;
fail:
    report_database_error("cannot initialize cluster mutual TLS certificates or trust store");
    cluster_tls_destroy(context);
    return NULL;
}

static int cluster_cn_node_id(X509* certificate) {
    char name[128];
    int length = X509_NAME_get_text_by_NID(X509_get_subject_name(certificate),
                                           NID_commonName, name, sizeof(name));
    const char prefix[] = "opendata-node-";
    if (length <= 0 || (size_t)length >= sizeof(name) ||
        strncmp(name, prefix, sizeof(prefix) - 1)) return 0;
    char* end = NULL;
    long node = strtol(name + sizeof(prefix) - 1, &end, 10);
    if (!end || *end || node < 1 || node > INT_MAX) return 0;
    return (int)node;
}

static ClusterTlsStream* cluster_tls_finish(ClusterTlsContext* context,
        cluster_socket_t socket, bool server_side, const char* peer_ip, int expected_node_id) {
    if (!cluster_set_socket_timeout(socket)) return NULL;
    ClusterTlsStream* stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->socket = socket;
    stream->ssl = SSL_new(server_side ? context->server : context->client);
    if (!stream->ssl || SSL_set_fd(stream->ssl, socket) != 1) goto fail;
    if (server_side) {
        if (SSL_accept(stream->ssl) != 1) goto fail;
    } else {
        X509_VERIFY_PARAM* parameters = SSL_get0_param(stream->ssl);
        if (!peer_ip || X509_VERIFY_PARAM_set1_ip_asc(parameters, peer_ip) != 1 ||
            SSL_connect(stream->ssl) != 1) goto fail;
    }
    if (SSL_get_verify_result(stream->ssl) != X509_V_OK) goto fail;
    X509* certificate = SSL_get1_peer_certificate(stream->ssl);
    if (!certificate) goto fail;
    stream->peer_node_id = cluster_cn_node_id(certificate);
    X509_free(certificate);
    if (!stream->peer_node_id || (!server_side && stream->peer_node_id != expected_node_id)) goto fail;
    return stream;
fail:
    if (stream->ssl) {
        long verification = SSL_get_verify_result(stream->ssl);
        fprintf(stderr, "WARNING: cluster TLS %s handshake/identity validation failed: expected node %d, received node %d, certificate status %ld (%s)\n",
                server_side ? "inbound" : "outbound", expected_node_id,
                stream->peer_node_id, verification, X509_verify_cert_error_string(verification));
    }
    cluster_tls_close(stream);
    return NULL;
}

ClusterTlsStream* cluster_tls_accept(ClusterTlsContext* context, intptr_t socket) {
    if (!context || socket == (intptr_t)CLUSTER_INVALID_SOCKET) return NULL;
    return cluster_tls_finish(context, (cluster_socket_t)socket, true, NULL, 0);
}

ClusterTlsStream* cluster_tls_connect(ClusterTlsContext* context, intptr_t socket,
                                      const char* peer_ip, int peer_node_id) {
    if (!context || socket == (intptr_t)CLUSTER_INVALID_SOCKET) return NULL;
    return cluster_tls_finish(context, (cluster_socket_t)socket, false, peer_ip, peer_node_id);
}

int cluster_tls_peer_node_id(ClusterTlsStream* stream) {
    return stream ? stream->peer_node_id : 0;
}

bool cluster_tls_send_all(ClusterTlsStream* stream, const void* data, size_t length) {
    if (!stream || (!data && length)) return false;
    const unsigned char* input = data;
    size_t sent = 0;
    while (sent < length) {
        int chunk = (int)((length - sent) > INT_MAX ? INT_MAX : length - sent);
        int n = SSL_write(stream->ssl, input + sent, chunk);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool cluster_tls_recv_all(ClusterTlsStream* stream, void* data, size_t length) {
    if (!stream || (!data && length)) return false;
    unsigned char* output = data;
    size_t received = 0;
    while (received < length) {
        int chunk = (int)((length - received) > INT_MAX ? INT_MAX : length - received);
        int n = SSL_read(stream->ssl, output + received, chunk);
        if (n <= 0) return false;
        received += (size_t)n;
    }
    return true;
}

void cluster_tls_close(ClusterTlsStream* stream) {
    if (!stream) return;
    if (stream->ssl) {
        SSL_shutdown(stream->ssl);
        SSL_free(stream->ssl);
    }
    free(stream);
}

void cluster_tls_destroy(ClusterTlsContext* context) {
    if (!context) return;
    if (context->server) SSL_CTX_free(context->server);
    if (context->client) SSL_CTX_free(context->client);
    free(context);
}

#else

struct ClusterTlsContext { int unavailable; };
struct ClusterTlsStream { int unavailable; };

ClusterTlsContext* cluster_tls_create(const char* cert_path, const char* key_path,
                                      const char* ca_path, int local_node_id) {
    (void)cert_path; (void)key_path; (void)ca_path; (void)local_node_id;
    report_database_error("cluster mutual TLS requires an OPENDATA_TLS-enabled build");
    return NULL;
}
void cluster_tls_destroy(ClusterTlsContext* context) { free(context); }
ClusterTlsStream* cluster_tls_accept(ClusterTlsContext* context, intptr_t socket) {
    (void)context; (void)socket; return NULL;
}
ClusterTlsStream* cluster_tls_connect(ClusterTlsContext* context, intptr_t socket,
                                      const char* peer_ip, int peer_node_id) {
    (void)context; (void)socket; (void)peer_ip; (void)peer_node_id; return NULL;
}
int cluster_tls_peer_node_id(ClusterTlsStream* stream) { (void)stream; return 0; }
bool cluster_tls_send_all(ClusterTlsStream* stream, const void* data, size_t len) {
    (void)stream; (void)data; (void)len; return false;
}
bool cluster_tls_recv_all(ClusterTlsStream* stream, void* data, size_t len) {
    (void)stream; (void)data; (void)len; return false;
}
void cluster_tls_close(ClusterTlsStream* stream) { free(stream); }

#endif
