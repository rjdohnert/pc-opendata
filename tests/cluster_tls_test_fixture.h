#ifndef OPENDATA_CLUSTER_TLS_TEST_FIXTURE_H
#define OPENDATA_CLUSTER_TLS_TEST_FIXTURE_H

#include <stdbool.h>
#include <windows.h>
#include <wincrypt.h>

typedef struct {
    HCERTSTORE identity_store;
    HCERTSTORE root_store;
    PCCERT_CONTEXT identity_certificate;
    bool identity_has_key;
    HCERTCHAINENGINE chain_engine;
} OpendataTestTlsFixture;

bool opendata_test_tls_fixture_identity_open(const char* pfx_path,
                                            OpendataTestTlsFixture* fixture);
HCERTSTORE opendata_test_tls_fixture_wallet_store(OpendataTestTlsFixture* fixture);
bool opendata_test_tls_fixture_open(const char* pfx_path, const char* root_der_path,
                                    const char* crl_der_path, OpendataTestTlsFixture* fixture);
void opendata_test_tls_fixture_close(OpendataTestTlsFixture* fixture);

#endif
