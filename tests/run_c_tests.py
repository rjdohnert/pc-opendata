"""Build and run every C regression in an isolated temporary directory."""

import argparse
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", help="Run one test by stem, for example test_memory_safety")
    parser.add_argument("--sanitize", action="store_true", help="Enable AddressSanitizer and UBSan")
    parser.add_argument("--require-tls", action="store_true",
                        help="Fail if the cluster TLS runtime test cannot run")
    parser.add_argument("--timeout", type=int, default=120, help="Seconds allowed per test executable")
    args = parser.parse_args()
    configured = os.environ.get("CC")
    compiler = ([configured] if configured and Path(configured).is_file() else shlex.split(configured)) \
        if configured else [shutil.which("clang") or "cc"]
    names = (ROOT / "src" / "modules.txt").read_text().splitlines()
    modules = [str(ROOT / "src" / name) for name in names if name and name != "opendata_main.c"]
    flags = ["-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-I" + str(ROOT / "include"),
             "-I" + str(ROOT / "src")]
    if os.name != "nt":
        flags.append("-pthread")
    if args.sanitize:
        flags += ["-fsanitize=address" if os.name == "nt" else "-fsanitize=address,undefined",
                  "-fno-omit-frame-pointer"]
    flags += shlex.split(os.environ.get("OPENDATA_TEST_CFLAGS", ""))
    libraries = ["-lws2_32", "-lsecur32", "-lcrypt32", "-lbcrypt", "-lncrypt", "-ladvapi32"] \
        if os.name == "nt" else ["-lcrypto", "-lm"]
    libraries += shlex.split(os.environ.get("OPENDATA_TEST_LDFLAGS", ""))
    tests = sorted((ROOT / "tests").glob("test_*.c"))
    if args.test:
        tests = [test for test in tests if test.stem == args.test]
        if not tests:
            parser.error("unknown C test")
    failed = []
    skipped = []
    with tempfile.TemporaryDirectory(prefix="opendata-c-tests-") as directory:
        for test in tests:
            work = Path(directory) / test.stem
            work.mkdir()
            binary = work / (test.stem + (".exe" if os.name == "nt" else ""))
            test_flags = ["-DOPENDATA_TEST_CLUSTER_PLAINTEXT"] if test.stem == "test_cluster_quorum" else []
            test_libraries = []
            test_env = os.environ.copy()
            test_command = [str(binary)]
            if test.stem == "test_cluster_tls":
                if os.name == "nt":
                    test_flags += ["-DOPENDATA_TLS", "-DOPENDATA_TEST_TLS_FIXTURES"]
                    powershell = r"C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe"
                    if not powershell:
                        raise RuntimeError("PowerShell is required for isolated Windows Schannel fixtures")
                    test_command = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                    str(ROOT / "tests" / "Invoke-ClusterTlsTest.ps1"),
                                    "-Binary", str(binary)]
                else:
                    test_flags.append("-DOPENDATA_TLS")
                    openssl = shutil.which("openssl")
                    if not openssl:
                        print("test_cluster_tls: SKIP (OpenSSL command-line tool unavailable)", flush=True)
                        skipped.append(test.stem)
                        continue
                    test_libraries += ["-lssl", "-lcrypto"]
                    prepare_tls_certificates(work, test_env, openssl)
            command = compiler + flags + test_flags + [str(test), *modules, *test_libraries, *libraries, "-o", str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
                failed.append(test.stem + " (build)")
                continue
            try:
                result = subprocess.run(test_command, cwd=work, env=test_env, stdin=subprocess.DEVNULL,
                                        capture_output=True, text=True, errors="replace", timeout=args.timeout)
                passed = result.returncode == 0
                print(f"{test.stem}: {'PASS' if passed else 'FAIL'} ({result.returncode})", flush=True)
                if not passed:
                    print(result.stdout[-12000:] + result.stderr[-12000:], flush=True)
                    failed.append(test.stem)
            except subprocess.TimeoutExpired as error:
                print(f"{test.stem}: TIMEOUT", flush=True)
                for output in (error.stdout, error.stderr):
                    if output:
                        print(output[-12000:].decode(errors="replace") if isinstance(output, bytes)
                              else output[-12000:], flush=True)
                failed.append(test.stem)
    print(f"{len(tests) - len(failed) - len(skipped)} passed, {len(failed)} failed, "
          f"{len(skipped)} skipped ({len(tests)} C test executables).", flush=True)
    if args.require_tls and ("test_cluster_tls" in skipped or
                             not any(test.stem == "test_cluster_tls" for test in tests)):
        print("Required cluster TLS runtime coverage was not executed.", flush=True)
        return 1
    return int(bool(failed))


def prepare_tls_certificates(work, test_env, openssl):
    ca_key = work / "test-ca.key"
    ca_cert = work / "test-ca.pem"
    extension_file = work / "node.ext"
    extension_file.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth,clientAuth\n"
        "subjectAltName=IP:127.0.0.1\n"
    )
    subprocess.run([
        openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(ca_key), "-out", str(ca_cert), "-days", "2",
        "-subj", "/CN=OpenData Cluster Test CA",
        "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign"
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    test_env["OPENDATA_TEST_CLUSTER_CA"] = str(ca_cert)
    untrusted_ca = work / "untrusted-ca.pem"
    subprocess.run([
        openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(work / "untrusted-ca.key"), "-out", str(untrusted_ca),
        "-days", "2", "-subj", "/CN=Untrusted Cluster Test CA",
        "-addext", "basicConstraints=critical,CA:TRUE"
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    test_env["OPENDATA_TEST_CLUSTER_UNTRUSTED_CA"] = str(untrusted_ca)
    for node_id in (1, 2):
        key = work / f"node{node_id}.key"
        request = work / f"node{node_id}.csr"
        certificate = work / f"node{node_id}.pem"
        subprocess.run([
            openssl, "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(request), "-subj",
            f"/CN=opendata-node-{node_id}"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        subprocess.run([
            openssl, "x509", "-req", "-in", str(request),
            "-CA", str(ca_cert), "-CAkey", str(ca_key), "-CAcreateserial",
            "-out", str(certificate), "-days", "2", "-sha256",
            "-extfile", str(extension_file)
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        test_env[f"OPENDATA_TEST_CLUSTER_NODE{node_id}_CERT"] = str(certificate)
        test_env[f"OPENDATA_TEST_CLUSTER_NODE{node_id}_KEY"] = str(key)


if __name__ == "__main__":
    raise SystemExit(main())
