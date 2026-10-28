param([string]$Binary, [switch]$Recovery, [switch]$Wallet)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$fixturePath = Join-Path $PSScriptRoot 'cluster_tls_fixtures'
if (Test-Path $fixturePath) {
    throw "Refusing to overwrite existing TLS fixture directory: $fixturePath"
}
$null = New-Item -ItemType Directory -Path $fixturePath
$savedEnvironment = @{}
$caKey = $null
$caCertificate = $null
$untrustedKey = $null
$untrustedCertificate = $null
$revokedPfx = $null
$fixtureVariables = @(
    'OPENDATA_TEST_CLUSTER_CA',
    'OPENDATA_TEST_CLUSTER_CRL',
    'OPENDATA_TEST_CLUSTER_KEY_LOG',
    'OPENDATA_TEST_CLUSTER_NODE1_CERT',
    'OPENDATA_TEST_CLUSTER_NODE1_KEY',
    'OPENDATA_TEST_CLUSTER_NODE2_CERT',
    'OPENDATA_TEST_CLUSTER_NODE2_KEY',
    'OPENDATA_TEST_CLUSTER_UNTRUSTED_CA',
    'OPENDATA_TEST_CLUSTER_UNTRUSTED_CERT',
    'OPENDATA_TEST_CLUSTER_EXPIRED_CERT',
    'OPENDATA_TEST_CLUSTER_REVOKED_CERT',
    'OPENDATA_TEST_WALLET_PFX',
    'OPENDATA_TEST_WALLET_CASES',
    'OPENDATA_TEST_BINARY'
)
foreach ($name in $fixtureVariables) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}

$fixtureSource = @'
using System;
using System.Collections.Generic;
using System.Formats.Asn1;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

public static class ClusterTlsFixtureCrypto
{
    private const string Sha256RsaOid = "1.2.840.113549.1.1.11";
    private const int NteBadKeyset = unchecked((int)0x80090016);
    private const int NteNotFound = unchecked((int)0x80090011);

    [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
    private static extern int NCryptOpenStorageProvider(out IntPtr provider, string name, int flags);
    [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
    private static extern int NCryptOpenKey(IntPtr provider, out IntPtr key, string name,
                                            int legacySpec, int flags);
    [DllImport("ncrypt.dll")]
    private static extern int NCryptDeleteKey(IntPtr key, int flags);
    [DllImport("ncrypt.dll")]
    private static extern int NCryptFreeObject(IntPtr handle);

    public static int DeleteCngKey(string providerName, string keyName)
    {
        IntPtr provider;
        int status = NCryptOpenStorageProvider(out provider, providerName, 0);
        if (status != 0) return status;
        IntPtr key;
        status = NCryptOpenKey(provider, out key, keyName, 0, 0x40);
        if (status == NteBadKeyset || status == NteNotFound)
        {
            NCryptFreeObject(provider);
            return 0;
        }
        if (status == 0)
        {
            status = NCryptDeleteKey(key, 0x40);
            if (status != 0) NCryptFreeObject(key);
        }
        NCryptFreeObject(provider);
        return status;
    }

    public static byte[] CreateCrl(X509Certificate2 issuer, X509Certificate2 revoked)
    {
        using RSA key = issuer.GetRSAPrivateKey();
        if (key == null) throw new InvalidOperationException("Fixture CA has no RSA private key.");
        var algorithm = new AsnWriter(AsnEncodingRules.DER);
        algorithm.PushSequence();
        algorithm.WriteObjectIdentifier(Sha256RsaOid);
        algorithm.WriteNull();
        algorithm.PopSequence();
        byte[] algorithmIdentifier = algorithm.Encode();

        var tbs = new AsnWriter(AsnEncodingRules.DER);
        tbs.PushSequence();
        tbs.WriteInteger(1);
        tbs.WriteEncodedValue(algorithmIdentifier);
        tbs.WriteEncodedValue(issuer.SubjectName.RawData);
        DateTimeOffset now = DateTimeOffset.UtcNow;
        tbs.WriteUtcTime(now.AddMinutes(-5));
        tbs.WriteUtcTime(now.AddDays(7));
        if (revoked != null)
        {
            byte[] serial = revoked.GetSerialNumber().Reverse().SkipWhile(b => b == 0).ToArray();
            if (serial.Length == 0) serial = new byte[] { 0 };
            tbs.PushSequence();
            tbs.PushSequence();
            tbs.WriteIntegerUnsigned(serial);
            tbs.WriteUtcTime(now.AddMinutes(-1));
            tbs.PopSequence();
            tbs.PopSequence();
        }
        tbs.PopSequence();
        byte[] encodedTbs = tbs.Encode();
        byte[] signature = key.SignData(encodedTbs, HashAlgorithmName.SHA256,
                                        RSASignaturePadding.Pkcs1);

        var crl = new AsnWriter(AsnEncodingRules.DER);
        crl.PushSequence();
        crl.WriteEncodedValue(encodedTbs);
        crl.WriteEncodedValue(algorithmIdentifier);
        crl.WriteBitString(signature);
        crl.PopSequence();
        return crl.Encode();
    }

    public static byte[] CreateNodePfx(X509Certificate2 issuer, int nodeId,
        DateTimeOffset notBefore, DateTimeOffset notAfter, byte[] serial,
        bool clientOnly = false, bool publicOnly = false)
    {
        using RSA key = RSA.Create(2048);
        var request = new CertificateRequest("CN=opendata-node-" + nodeId, key,
            HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
        request.CertificateExtensions.Add(new X509KeyUsageExtension(
            X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
        request.CertificateExtensions.Add(new X509SubjectKeyIdentifierExtension(request.PublicKey, false));
        var issuerKeyIdentifier = issuer.Extensions
            .OfType<X509SubjectKeyIdentifierExtension>().Single();
        var authorityKeyIdentifier = new AsnWriter(AsnEncodingRules.DER);
        authorityKeyIdentifier.PushSequence();
        authorityKeyIdentifier.WriteOctetString(
            Convert.FromHexString(issuerKeyIdentifier.SubjectKeyIdentifier),
            new Asn1Tag(TagClass.ContextSpecific, 0));
        authorityKeyIdentifier.PopSequence();
        request.CertificateExtensions.Add(new X509Extension(
            "2.5.29.35", authorityKeyIdentifier.Encode(), false));
        var usages = new OidCollection();
        if (!clientOnly) usages.Add(new Oid("1.3.6.1.5.5.7.3.1"));
        usages.Add(new Oid("1.3.6.1.5.5.7.3.2"));
        request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(usages, true));
        var san = new SubjectAlternativeNameBuilder();
        san.AddIpAddress(System.Net.IPAddress.Loopback);
        request.CertificateExtensions.Add(san.Build());
        using X509Certificate2 publicCertificate = request.Create(issuer, notBefore, notAfter, serial);
        if (publicOnly) return publicCertificate.Export(X509ContentType.Pfx, "");
        using X509Certificate2 privateCertificate = publicCertificate.CopyWithPrivateKey(key);
        return privateCertificate.Export(X509ContentType.Pfx, "");
    }
}
'@

try {
    Add-Type -TypeDefinition $fixtureSource -Language CSharp
    $now = [DateTimeOffset]::UtcNow
    $caKey = [System.Security.Cryptography.RSA]::Create(2048)
    $caRequest = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=OpenData Cluster Fixture CA', $caKey,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $caRequest.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))
    $caRequest.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign -bor
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::CrlSign, $true))
    $caRequest.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new(
            $caRequest.PublicKey, $false))
    $caCertificate = $caRequest.CreateSelfSigned($now.AddDays(-10), $now.AddDays(30))
    [IO.File]::WriteAllBytes((Join-Path $fixturePath 'root.cer'), $caCertificate.RawData)

    $untrustedKey = [System.Security.Cryptography.RSA]::Create(2048)
    $untrustedRequest = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=Untrusted Cluster Fixture CA', $untrustedKey,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $untrustedRequest.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))
    $untrustedRequest.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign -bor
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::CrlSign, $true))
    $untrustedRequest.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new(
            $untrustedRequest.PublicKey, $false))
    $untrustedCertificate = $untrustedRequest.CreateSelfSigned($now.AddDays(-2), $now.AddDays(30))

    $script:serial = 1
    $makeFixture = {
        param([string]$Name, [int]$NodeId, $Issuer, [DateTimeOffset]$Start, [DateTimeOffset]$End)
        $serialBytes = [BitConverter]::GetBytes([long]$script:serial++)
        [Array]::Reverse($serialBytes)
        $pfx = [ClusterTlsFixtureCrypto]::CreateNodePfx(
            $Issuer, $NodeId, $Start, $End, $serialBytes)
        [IO.File]::WriteAllBytes((Join-Path $fixturePath "$Name.pfx"), $pfx)
    }
    & $makeFixture 'node1' 1 $caCertificate $now.AddDays(-1) $now.AddDays(14)
    & $makeFixture 'node2' 2 $caCertificate $now.AddDays(-1) $now.AddDays(14)
    & $makeFixture 'revoked' 1 $caCertificate $now.AddDays(-1) $now.AddDays(14)
    & $makeFixture 'untrusted' 2 $untrustedCertificate $now.AddDays(-1) $now.AddDays(14)
    & $makeFixture 'expired' 2 $caCertificate $now.AddDays(-3) $now.AddDays(-2)
    if ($Wallet) {
        $walletCases = @{}
        foreach ($name in @('node1', 'expired', 'future', 'client-only', 'public-only')) {
            $path = Join-Path $fixturePath "$name.pfx"
            if ($name -in @('future', 'client-only', 'public-only')) {
                $start = if ($name -eq 'future') { $now.AddDays(1) } else { $now.AddDays(-1) }
                $serialBytes = [BitConverter]::GetBytes([long]$script:serial++)
                [Array]::Reverse($serialBytes)
                $pfx = [ClusterTlsFixtureCrypto]::CreateNodePfx(
                    $caCertificate, 1, $start, $now.AddDays(14), $serialBytes,
                    ($name -eq 'client-only'), ($name -eq 'public-only'))
                [IO.File]::WriteAllBytes($path, $pfx)
            }
            $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
                $path, '', [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
            try { $walletCases[$name] = @{ pfx = $path; thumbprint = $certificate.Thumbprint } }
            finally { $certificate.Dispose() }
        }
        [Environment]::SetEnvironmentVariable('OPENDATA_TEST_WALLET_CASES',
            (ConvertTo-Json -InputObject $walletCases -Compress))
        if ($Binary) { [Environment]::SetEnvironmentVariable('OPENDATA_TEST_BINARY', $Binary) }
    }
    $revokedPfx = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        (Join-Path $fixturePath 'revoked.pfx'), '',
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    $crl = [ClusterTlsFixtureCrypto]::CreateCrl($caCertificate, $revokedPfx)
    [IO.File]::WriteAllBytes((Join-Path $fixturePath 'root.crl'), $crl)

    $environmentPaths = @{
        OPENDATA_TEST_CLUSTER_CA = Join-Path $fixturePath 'root.cer'
        OPENDATA_TEST_CLUSTER_CRL = Join-Path $fixturePath 'root.crl'
        OPENDATA_TEST_CLUSTER_KEY_LOG = Join-Path $fixturePath 'cng-keys.txt'
        OPENDATA_TEST_CLUSTER_NODE1_CERT = Join-Path $fixturePath 'node1.pfx'
        OPENDATA_TEST_CLUSTER_NODE2_CERT = Join-Path $fixturePath 'node2.pfx'
        OPENDATA_TEST_CLUSTER_UNTRUSTED_CERT = Join-Path $fixturePath 'untrusted.pfx'
        OPENDATA_TEST_CLUSTER_EXPIRED_CERT = Join-Path $fixturePath 'expired.pfx'
        OPENDATA_TEST_CLUSTER_REVOKED_CERT = Join-Path $fixturePath 'revoked.pfx'
    }
    foreach ($name in $environmentPaths.Keys) {
        [Environment]::SetEnvironmentVariable($name, $environmentPaths[$name])
    }
    if ($Wallet) {
        & python (Join-Path $PSScriptRoot 'test_windows_wallet.py') -v
    } elseif ($Recovery) {
        & python (Join-Path $PSScriptRoot 'test_cluster_recovery.py') -v
    } else {
        if (-not $Binary) { throw '-Binary is required without -Recovery.' }
        & $Binary
    }
    if ($LASTEXITCODE -ne 0) { throw "Windows cluster TLS fixture test failed ($LASTEXITCODE)." }
}
finally {
    foreach ($name in $fixtureVariables) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name])
    }
    $keyCleanupErrors = @()
    $keyLog = Join-Path $fixturePath 'cng-keys.txt'
    if (Test-Path $keyLog) {
        foreach ($line in Get-Content -LiteralPath $keyLog) {
            $parts = $line -split "`t", 2
            if ($parts.Count -eq 2 -and $parts[0] -and $parts[1]) {
                $status = [ClusterTlsFixtureCrypto]::DeleteCngKey($parts[0], $parts[1])
                if ($status -ne 0) { $keyCleanupErrors += ('0x{0:X8}' -f ($status -band 0xffffffff)) }
            } else {
                $keyCleanupErrors += 'malformed key cleanup ledger entry (no deletion attempted)'
            }
        }
    }
    if ($revokedPfx) { $revokedPfx.Dispose() }
    if ($untrustedCertificate) { $untrustedCertificate.Dispose() }
    if ($untrustedKey) { $untrustedKey.Dispose() }
    if ($caCertificate) { $caCertificate.Dispose() }
    if ($caKey) { $caKey.Dispose() }
    if ($keyCleanupErrors.Count) {
        throw "Fixture CNG key cleanup failed: $($keyCleanupErrors -join ', '); preserving exact-key ledger at $keyLog"
    }
    Remove-Item -LiteralPath $fixturePath -Recurse -Force
}
