# PC/OpenData SQL Server (pcodSQL) — Version 27.0 Release Notes

**Release Version:** 27.0.0 (Enterprise Enhancement & Hardening Release)  
**Release Date:** November 2026  
**Product:** PC/OpenData SQL Server  
**Vendor:** PC/OpenSystems LLC  
**Target Platforms:** Windows Server / Windows 10 & 11, Enterprise Linux (RHEL, Rocky, CentOS Stream, Fedora), Sun Solaris, Alpine Linux (Containers)  
**Implementation Standard:** ISO C11 (Strict Standards Compliance, Zero External Dependencies)

---

## Executive Summary

PC/OpenData SQL Server 27.0 is a major enterprise release introducing high-dimensional vector similarity search (HNSW), PostgreSQL v3 wire protocol compatibility, continuous Write-Ahead Log (WAL) archiving with Point-In-Time Recovery (PITR), ISO SQL window analytical functions, native Active Directory / LDAP and Kerberos SPNEGO authentication, and turnkey micro-container packaging.

The engine compiles into an ultra-compact standalone binary (~1.38 MB on Windows x64, ~4.7 MB compressed OCI container) while delivering full ACID compliance, multi-version concurrency capabilities, and zero external runtime dependencies.

---

## Major New Subsystems & Capabilities

### 1. PostgreSQL v3 Wire Protocol Compatibility
- **Seamless Drop-In Connectivity:** Added automatic protocol sniffing to the asynchronous network reactor, allowing standard PostgreSQL drivers and tools (`psql`, DBeaver, pgAdmin, Python `psycopg2`/`asyncpg`, Node.js `pg`, Go `pgx`) to connect directly to PC/OpenData on port `7338` without code changes.
- **Protocol Implementation:** Supports SSLRequest negotiation, StartupMessage handshake, MD5/SCRAM authentication flow, Simple Query (`'Q'`) pipeline, and Extended Query protocol (`Parse`, `Bind`, `Describe`, `Execute`, `Sync`).

### 2. High-Dimensional Vector Search (HNSW Indexing)
- **Hierarchical Navigable Small World (HNSW) Graphs:** Implemented native multi-layer HNSW indexing for dense embeddings (e.g. OpenAI, Hugging Face, Cohere models).
- **Fast Similarity Search:** Accelerates nearest-neighbor retrieval from $O(N)$ brute-force scans to $O(\log N)$ logarithmic retrieval.
- **Vector Distance Metrics:** Native support for Cosine Distance (`<=>`), Euclidean L2 Distance (`<->`), and Inner Product (`<#>`).
- **Syntax:**
  ```sql
  CREATE TABLE items (id INT PRIMARY KEY, title TEXT, embedding VECTOR(128));
  CREATE INDEX idx_items_hnsw ON items USING HNSW (embedding);
  SELECT title, embedding <=> '[0.1, 0.2, ...]' AS dist FROM items ORDER BY dist LIMIT 5;
  ```

### 3. Native Windows Service Integration
- **Service Control Manager (SCM) Support:** Direct command-line lifecycle management for running as a managed background Windows Service with automatic startup and clean shutdown:
  ```cmd
  opendata --install-service [service_name]
  opendata --start-service   [service_name]
  opendata --stop-service    [service_name]
  opendata --uninstall-service [service_name]
  ```

### 4. Continuous WAL Archiving & Point-In-Time Recovery (PITR)
- **Zero Data Loss Archiving:** Automated continuous differential Write-Ahead Log rotation and archiving to secondary storage or backup volumes.
- **Exact-Second Historical Replay:** Restore a database to any historical second:
  ```bash
  opendata --database prod.pcod --backup-dir /backups --pitr 1794285600
  ```
  Locates the base snapshot, sorts archive segments chronologically, and replays deltas up to the requested epoch second.

### 5. ISO SQL Window Analytical Functions & JSON Extraction
- **Window Functions:** Full support for `ROW_NUMBER()`, `RANK()`, and `DENSE_RANK()` with `OVER (PARTITION BY ... ORDER BY ...)`.
- **JSON Path Extraction:** Native operators for semi-structured documents:
  - `col->'path'`: Extracts JSON object/array while preserving JSON formatting.
  - `col->>'path'`: Extracts raw unquoted scalar text.
  - `JSON_VALUE(col, 'path')` and `JSON_EXTRACT(col, 'path')` ANSI functions.

### 6. Enterprise Identity & Single Sign-On (SSO)
- **Active Directory & OpenLDAP:** Authenticate users against organizational directory services using simple or TLS-encrypted binds with automatic group-to-role mappings (`ADMIN`, `USER`, `READ_ONLY`).
- **Kerberos v5 / SPNEGO Single Sign-On:** Native ticket verification for enterprise single sign-on across Windows SSPI and POSIX GSSAPI.

### 7. Encrypted Snapshots & Security Hardening
- **AES-256-GCM Storage Encryption:** Protect at-rest database backups and snapshots with authenticated AES-256 encryption.
- **Cryptographic Memory Cleansing:** Stack-based key material is cleansed immediately upon completion using `SecureZeroMemory` (Windows) and `OPENSSL_cleanse` (POSIX/Linux).

---

## Security & Reliability Fixes in 27.0

During the 27.0 release verification, a comprehensive security and functional audit was executed, resulting in 9 critical defect resolutions:

1. **Follower Privilege Hardening (`server_network.c`):** Fixed an edge case where an uninitialized follower node with an empty user catalog could permit unauthenticated admin bypass.
2. **Kerberos Ticket Validation (`directory_auth.c`):** Eliminated dummy authentication token fallbacks in production builds; restricted test bypass tokens strictly to the internal self-test harness.
3. **Buffer Overflow Bounds Checking (`predicate_evaluation.c`):** Added strict buffer validation when compiling SQL `LIKE` and `ILIKE` patterns into match buffers.
4. **HNSW Dimension Clamping (`hnsw_index.c`):** Guarded vector distance calculations against dimension mismatch and capped indexable vector sizes at 64 dimensions per graph layer.
5. **Network Reactor Lock Contention (`server_network.c`):** Replaced blocking lock timeouts inside the single-threaded reactor loop with non-blocking checks (`timeout = 0`), preventing event-loop stalls under heavy concurrent client traffic.
6. **Multi-Row Lock Rollback (`select_queries.c`):** Implemented an automatic rollback loop to release previously acquired row locks if a subsequent row lock fails during `SELECT ... FOR UPDATE`.
7. **Chronological WAL Segment Replay (`wal_engine.c`):** Added strict timestamp-based sorting for WAL segment replay during Point-in-Time Recovery.
8. **LDAP DN Injection Mitigation (`directory_auth.c`):** Implemented `escape_ldap_dn()` according to RFC 4514 to escape special characters (`'`, `"`, `=`, `+`, `<`, `>`, `;`, `\`, `\0`) in LDAP queries.
9. **Stack Memory Key Cleansing (`snapshot_format.c`):** Scrubbed AES-256 encryption key buffers using dedicated zeroization primitives.

---

## Packaging & Distribution Formats

PC/OpenData 27.0 is distributed in four production formats:

| Format | File Location | Footprint | Target Environment |
|---|---|---|---|
| **Windows x64 Native** | `bin/opendata.exe` | **~1.38 MB** | Windows 10/11 & Windows Server 2016–2025 |
| **Pre-Packaged Container Archive** | `docker/pc-opendata-27.0.tar.gz` | **~4.7 MB** | Offline Docker / Podman / Kubernetes |
| **Linux RPM Package** | `linux/pc-opendata-27.0-1.el10.x86_64.rpm` | **~1.2 MB** | Enterprise Linux (RHEL, CentOS Stream, Rocky, Alma) |
| **C11 Source Amalgamation** | `src/opendata.c` / `include/opendata.h` | **Single File** | In-process embedding in C, C++, Rust, Go, or Python |

---

## Verification & Self-Testing

PC/OpenData includes a self-contained test harness compiled directly into the binary. Every distribution can be self-verified with zero test dependencies:

```bash
opendata --execute "SELF-TEST;"
```

### Self-Test Results (27.0 Release Build):
- **13 Test Suites Executed:** Storage, B-Tree, Hash Index, Slotted Row, Transactions, Concurrency, WAL Durability, Vector HNSW, Full-Text Search, JSON/CSV Engine, Authentication, Replication, Network Protocol.
- **Assertions:** 3,728 / 3,728 checks passed (100% success rate, 0 failures).

---

## Getting Started

### Quick Start with Docker / Podman
```bash
# 1. Load the pre-packaged image archive
docker load -i docker/pc-opendata-27.0.tar.gz

# 2. Run an interactive shell
docker run -it --rm -v opendata_data:/data pc-opendata:27.0 /data/mydb.pcod

# 3. Start background server with Compose
docker compose -f docker/docker-compose.yml up -d
```

### Quick Start on Windows
```powershell
# Run native installer
.\install.ps1

# Launch the interactive database console
opendata production.pcod
```

---

## Support & Documentation
- **User Guide:** Detailed documentation is available in [`user_guide.md`](user_guide.md).
- **Docker Reference:** Container instructions are located in [`docker/README.md`](docker/README.md).
- **Security Audit Reference:** Technical details of all vulnerability remediations are documented in [`SECURITY_AND_FUNCTIONAL_FIXES.md`](SECURITY_AND_FUNCTIONAL_FIXES.md).
