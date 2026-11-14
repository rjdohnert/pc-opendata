# PC/OpenData SQL Server 27.0 — Official User Guide

Welcome to **PC/OpenData SQL Server**, a resilient, high-performance relational database and vector search engine written in ISO C11. The core engine is self-contained; network password authentication uses native OS cryptography (Windows CNG or POSIX OpenSSL `libcrypto`).

Originally developed for Sun Solaris in 2006 and battle-tested in enterprise production by PC/OpenSystems LLC for over two decades, PC/OpenData SQL Server combines ACID durability, Raft quorum consensus, native SIMD vector embeddings, and direct binary interoperability with Microsoft SQL Server (`.mdf`) and Oracle Database (`.dbf`) files.

> **New here?** Start with the [5-Minute Quick Start](#1-5-minute-quick-start) — you'll have a working database before you finish reading this page.

---

## Table of Contents
1. [5-Minute Quick Start](#1-5-minute-quick-start)
2. [Common Tasks Cookbook](#2-common-tasks-cookbook)
3. [Glossary](#3-glossary)
4. [Installation & Getting Started](#4-installation--getting-started)
5. [Database Lifecycle & File Management](#5-database-lifecycle--file-management)
6. [SQL Dialects, Data Types & Schema Definition](#6-sql-dialects-data-types--schema-definition)
7. [Data Manipulation & Transaction Control](#7-data-manipulation--transaction-control)
8. [Native AI Vector Embeddings & Similarity Search](#8-native-ai-vector-embeddings--similarity-search)
9. [Cross-Engine Compatibility: Microsoft SQL Server & Oracle Files](#9-cross-engine-compatibility-microsoft-sql-server--oracle-files)
10. [Data Recovery, Zero Data Loss Consensus & Backups](#10-data-recovery-zero-data-loss-consensus--backups)
11. [Shell Ergonomics, IRIS Editor & Observability](#11-shell-ergonomics-iris-editor--observability)
12. [Network Servers & Wire Protocols](#12-network-servers--wire-protocols)
13. [Visual Studio Code Extension Guide](#13-visual-studio-code-extension-guide)
14. [Embedded C API & Language Drivers (Python, Node.js)](#14-embedded-c-api--language-drivers-python-nodejs)
15. [Troubleshooting](#15-troubleshooting)
16. [Command Reference Quick Sheet](#16-command-reference-quick-sheet)
17. [Core Architecture & Highlights](#17-core-architecture--highlights)

---

## 1. 5-Minute Quick Start

This is the fastest path from zero to a working database. No database experience needed.

### Step 1 — Install

**Windows (System-Wide Installation — Requires Administrator)**:
```powershell
.\install.ps1
```
*(Installs system-wide to `C:\Program Files\OpenDataSQL\bin` and updates the System `PATH` for all users. **NOTE: PC/OpenData SQL Server is a system-wide application. THERE IS NO LOCAL USER INSTALL.**)*

**Linux / Solaris**:
```bash
cc -std=c11 -O2 -pthread src/opendata.c -lcrypto -lm -o bin/opendata
```

### Step 2 — Launch

Open a terminal and type:
```bash
opendata
```

You'll see:
```text
PC/OpenData SQL Interactive Shell (27.0 SQL Server)
Connected to: C:\Users\you\mydata.pcod
Autocommit is ON (SET AUTOCOMMIT OFF; to change).
Type "help;" for instructions, "exit;" to quit.

OpenData SQL>
```

> **Tip:** `opendata` with no arguments automatically opens (or creates) `mydata.pcod` in the current directory. To open a specific file, use `opendata yourfile.pcod`. To start without any database, use `opendata --no-default-db`.

> **Semicolons are optional in the interactive shell.** Press Enter after a complete statement and it runs. Multi-line statements still work: end intermediate lines with a comma or open parenthesis, or finish with `;`. Scripts and piped input still require semicolons. Control this with `SET SEMICOLON OPTIONAL ON|OFF;` or `semicolon_optional` in `opendata.ini`.

> **Want practice data?** Type `LOAD SAMPLE;` to create a `SAMPLE_DATA` table with five rows, then try `BROWSE SAMPLE_DATA;`.

### Step 3 — Create a table

Type this and press Enter:
```sql
CREATE TABLE friends (id NUMBER PRIMARY KEY, name VARCHAR2(50), city VARCHAR2(50));
```
You'll see: `Table created.`

### Step 4 — Add some data

```sql
INSERT INTO friends VALUES (1, 'Ada', 'London');
INSERT INTO friends VALUES (2, 'Grace', 'Arlington');
```
You'll see: `1 row created.` each time.

### Step 5 — Save your work

**Autocommit is ON by default** — your changes are saved automatically after each statement. You don't need to type `COMMIT;` unless you've turned autocommit off.

If you prefer manual control:
```sql
SET AUTOCOMMIT OFF;
-- ... make changes ...
COMMIT;
```

> **Note:** If you exit with uncommitted changes while autocommit is off, the shell will warn you and ask you to `COMMIT;`, `ROLLBACK;`, or `exit!;` to discard.

### Step 6 — Query your data

```sql
SELECT * FROM friends;
```
You'll see:
```text
ID	NAME	CITY
1	Ada	London
2	Grace	Arlington

2 rows selected.
```

### Step 7 — Exit

```sql
exit;
```

**That's it.** You now have a durable database file (`mydata.pcod`) you can reopen anytime with `opendata mydata.pcod`.

---

## 2. Common Tasks Cookbook

Quick answers to the most common questions. Each task assumes you've launched the shell with `opendata mydata.pcod`.

### How do I create a table?
The easiest way is the interactive builder — no SQL needed:
```sql
CREATE WIZARD;
```
It prompts for a table name, columns, types, and an optional primary key, shows
you the exact `CREATE TABLE` statement, and asks before running it.

Or write the SQL yourself:
```sql
CREATE TABLE tablename (id NUMBER PRIMARY KEY, column1 VARCHAR2(100), column2 NUMBER);
COMMIT;
```

### How do I practice without real data?
```sql
LOAD SAMPLE;
```
This creates a `SAMPLE_DATA` table with five rows. Try `BROWSE SAMPLE_DATA;` or
`SELECT NAME, SCORE FROM SAMPLE_DATA WHERE SCORE > 95;`. Running it again is
harmless — it tells you the sample is already loaded.

### How do I add a row?
```sql
INSERT INTO tablename VALUES (1, 'some text', 42);
```
*(Autocommit is ON by default — no COMMIT needed.)*

### How do I find a row?
```sql
SELECT * FROM tablename WHERE id = 1;
```

### How do I import a spreadsheet or CSV file?
In the interactive terminal, run:
```sql
IMPORT WIZARD;
```
The wizard previews the header and up to five rows, suggests column names and
types, lets you override each suggested type, then asks before creating the
table and importing. It checks every row and commits the table and data
together; if validation fails, it rolls back the import. Type suggestions use
only the preview rows, so review them carefully. CSV headers are matched to
table columns by name, so column order does not have to match.

For scripts and automation, create the destination table yourself and run:
```sql
IMPORT TABLE tablename FROM CSV 'people.csv' HEADER;
```
The header names must match the destination column names. To import a CSV with
no header row, omit `HEADER`; values are then matched by column position.

### How do I update a row?
```sql
UPDATE tablename SET column1 = 'new value' WHERE id = 1;
```

### How do I delete a row?
```sql
DELETE FROM tablename WHERE id = 1;
```
In the interactive terminal, dropping a table/database, truncating a table, or
deleting every row asks for a typed confirmation. Scripts, embedded API calls,
and server requests remain non-interactive; review destructive statements
carefully and take a backup first.

### How do I see all my tables?
```sql
SHOW TABLES;
```

### How do I see what's in a table?
```sql
SELECT * FROM tablename;
```
For a quick, bounded preview, use `BROWSE tablename;` (up to 20 rows).
In the interactive shell, larger tables page: press Enter for the next page or
`q` to stop. In scripts, `BROWSE` shows the first page and prints the exact
`SELECT ... LIMIT ... OFFSET ...` statement for the next page.
Use `CONNECTION STATUS;` to see the current session, transport security,
service/database availability, and storage/backup health.

### How do I back up my database?
```sql
BACKUP;
```
Or set up automatic backups when you create the database:
```sql
CREATE DATABASE 'mydata.pcod' BACKUP 'C:\Backups' INTERVAL 300;
```

### How do I restore from a backup?
```bash
opendata --database newcopy.pcod --restore C:\Backups\mydata.pcod
```
Restore to a new file first and verify it before replacing a production database.

### How do I prove a backup is restorable?
```sql
VERIFY BACKUP 'C:\Backups\mydata.pcod';
```
This decodes and integrity-checks the backup offline — it never touches the live
database. Run it after `BACKUP;` or on a schedule to catch silent corruption.

### How do I check my configuration before starting?
```bash
opendata --check-config
```
This validates `opendata.ini` strictly: unknown sections, unknown keys, and
malformed values are reported with line numbers. Exit code 0 means the file is
valid; 1 means problems were found.

### How do I keep an audit trail?
Add this to `opendata.ini`:
```ini
[audit]
enabled = on
path = C:\Logs\opendata_audit.log
```
Every statement that changes data or the security catalog is recorded with a
timestamp, the actor (network user or local shell), the command class, the
target object, and the outcome. Passwords and row values are never recorded.
The default path is `.opendata_audit` in your home directory.

### How do I share diagnostics with support?
```sql
SUPPORT REPORT TO 'support-summary.txt';
```
This creates a new file and refuses to overwrite an existing one. The summary
includes health and generation information but intentionally omits database
paths, configuration contents, account names, passwords, SQL, query history,
and row data. It does not include the detailed `.opendata_error_report`; review
that log separately before sharing it.

### How do I export my data to SQL script?
```sql
DUMP DATABASE TO 'backup.sql';
```

### How do I import a Microsoft SQL Server or Oracle file?
```sql
CONNECT 'sales_data.mdf';
-- or
CONNECT 'erp_data.dbf';
```

### How do I undo a mistake?
If autocommit is ON (the default), each statement is saved immediately — you can't undo a committed statement. To group multiple statements into one undoable unit:
```sql
SET AUTOCOMMIT OFF;
BEGIN;
-- ... your changes ...
ROLLBACK;  -- undoes everything since BEGIN
```

### How do I stop the shell?
```sql
exit;
```

---

## 3. Glossary

| Term | Meaning |
| :--- | :--- |
| **Database file (`.pcod`)** | A single file on disk that holds all your tables and data. |
| **Generation** | A counter that increases every time you `COMMIT`. It tracks how many times the database has been saved. |
| **Commit** | Save your changes permanently to the database file. Until you commit, changes exist only in memory. |
| **Snapshot** | The saved state of the database at a specific generation. |
| **Role** | A permission level: `ADMIN` can do everything; `READ_ONLY` can only query. |
| **Session** | One connection to the database (the shell, or a network client). |
| **WAL** | Write-Ahead Log — the mechanism that makes commits durable and crash-safe. |

---

## 4. Installation & Getting Started

### User Configuration File

On first launch, PC/OpenData creates `opendata.ini` in your home directory:
- **Windows**: `%USERPROFILE%\opendata.ini`
- **Linux/Solaris**: `~/.opendata.ini`

Edit this file to change defaults. Delete it to reset everything to defaults.

```ini
[shell]
autocommit = on           # Save after every statement (default: on)
semicolon_optional = on   # Run complete one-line statements on Enter (default: on for interactive shell)
format = table            # Output format: table, csv, or json
maxrows = 0               # Max rows returned by SELECT (0 = unlimited)
timing = off              # Show query execution time
null_string = NULL        # Display string for NULL values

[startup]
default_database = mydata.pcod   # Auto-open this file on zero-arg launch
show_tutorial = on        # Show quick-start banner when no database connected
```

### Error Report

PC/OpenData appends timestamped errors and lifecycle events to `.opendata_error_report` in your home directory. This is useful for debugging or sharing with support. It never contains passwords.

### Windows Installation
PC/OpenData SQL Server is an enterprise relational database engine designed to run as a system-wide application for all users and services on the machine.

> **CRITICAL REQUIREMENT: SYSTEM-WIDE INSTALL ONLY**  
> PC/OpenData SQL Server installs exclusively to `C:\Program Files\OpenDataSQL\bin` and configures the system-wide Machine `PATH`. **THERE IS NO LOCAL USER INSTALL** (no local `%LOCALAPPDATA%` installation). Installation requires administrative elevation.

#### Root Build & Install Wrappers
The repository root contains direct PowerShell and Batch wrappers:
* `build.ps1` / `build.bat`: Compile `bin\opendata.exe` (supports `-Amalgamation` and `-SkipTests`).
* `install.ps1` / `install.bat`: Automated system-wide installer.
* `amalgamate.ps1` / `amalgamate.bat`: Regenerate `src\opendata.c` from engine modules.
* `build_all_tests.ps1`: Compile and execute test suites.

#### Installing PC/OpenData SQL Server
Run the installer from PowerShell or Command Prompt:
```powershell
# Installs to C:\Program Files\OpenDataSQL\bin and updates System PATH
.\install.ps1
```
*(If PowerShell is not already running as Administrator, `install.ps1` automatically prompts for elevation via UAC to write to `C:\Program Files` and update the Machine environment variables).*

### Launching the Interactive Shell & CLI Syntax
PC/OpenData features positional arguments for instant database opening and statement execution:
```bash
# Launch interactive shell with default 'mydata.pcod'
opendata

# Open a specific database file directly (positional argument)
opendata production.pcod

# Run a query directly against a database and exit (like sqlite3)
opendata production.pcod "SELECT * FROM employees;"

# Run an in-memory calculation directly without creating a file
opendata -e "SELECT 1 + 1 AS sum, 10 * 10 AS mult;"

# Launch a database in read-only mode
opendata production.pcod --readonly

# One-command local development server
opendata --dev

# Run with Active Directory Domain Authentication
opendata --server --database prod.pcod --ad-server dc1.corp.local --ad-domain corp.local --ad-admin-group "Domain Admins"

# Run with OpenLDAP / FreeIPA Authentication
opendata --server --database prod.pcod --ldap-server ldap.company.com --ldap-port 636 --ldap-ssl --ldap-base-dn "ou=users,dc=company,dc=com" --ldap-admin-group "Directory Admins"

# Run with Kerberos / SPNEGO Single Sign-On (SSO)
opendata --server --database prod.pcod --krb-realm EXAMPLE.COM --krb-service postgres/db.example.com --krb-keytab /etc/krb5.keytab

# Configure Statement Timeout Watchdog (e.g. 5000 ms) and Auto-Vacuum Scheduler
opendata --server --database prod.pcod --statement-timeout 5000 --auto-vacuum
```

### Windows Service Management
On Windows, PC/OpenData SQL Server can be registered and controlled directly as a background Windows Service via the Service Control Manager (SCM):
```cmd
:: Install and register PC/OpenData SQL Server as a Windows Service
opendata --install-service [service_name]

:: Start the background Windows Service
opendata --start-service [service_name]

:: Stop the running Windows Service
opendata --stop-service [service_name]

:: Unregister and delete the Windows Service
opendata --uninstall-service [service_name]
```
*(Default service name is `OpenDataSQLServer` if omitted. Requires Administrative elevation).*

### Container Deployment (Docker & Podman)

PC/OpenData SQL Server provides a lightweight (~12 MB uncompressed, ~4.7 MB compressed), production-hardened multi-stage container image based on Alpine Linux 3.19. The image compiles with C11 OpenSSL TLS networking, automated PITR backups, and native OpenLDAP directory authentication, executing as a dedicated unprivileged user (`opendata`).

A pre-built, ready-to-use container image archive is packaged directly inside the repository under [`docker/pc-opendata-27.0.tar.gz`](docker/pc-opendata-27.0.tar.gz) and [`docker/pc-opendata-latest.tar.gz`](docker/pc-opendata-latest.tar.gz). Anyone can download the repository and immediately load and run the container without needing compilers or internet access to build from scratch.

#### 1. Loading the Pre-Built Container Image (Offline / Turnkey)
Load the pre-packaged archive directly into Docker or Podman:
```bash
# Using Docker
docker load -i docker/pc-opendata-27.0.tar.gz

# Using Podman (e.g. in RHEL / CentOS Stream / Fedora / Azure Linux / WSL)
podman load -i docker/pc-opendata-27.0.tar.gz
```
Once loaded, verify that the image is registered in your local image store:
```bash
docker images pc-opendata
# REPOSITORY    TAG       IMAGE ID       SIZE
# pc-opendata   27.0      788d24623c32   12.3MB
# pc-opendata   latest    788d24623c32   12.3MB
```

#### 2. Building the Container Image from Source (Optional)
If you wish to recompile or customize the container image locally from the C source tree:
```bash
# Using Docker
docker build -t pc-opendata:27.0 -f docker/Dockerfile .

# Using Podman
podman build -t pc-opendata:27.0 -f docker/Dockerfile .
```

#### 3. Running with Docker Compose
A turnkey orchestration file is provided at [`docker/docker-compose.yml`](docker/docker-compose.yml):
```bash
# Start background server
docker compose -f docker/docker-compose.yml up -d

# Check container health and status
docker compose -f docker/docker-compose.yml ps

# View real-time database logs
docker compose -f docker/docker-compose.yml logs -f

# Stop the database service
docker compose -f docker/docker-compose.yml down
```

#### 4. Standalone Server Deployment
Run the daemon in the background with persistent volume mounts:
```bash
docker run -d \
  --name opendata-server \
  -p 7338:7338 \
  -v opendata_data:/data \
  -v opendata_backups:/backups \
  -v /etc/opendata/tls:/run/tls:ro \
  -e OPENDATA_SERVER_USER=admin \
  -e OPENDATA_SERVER_PASSWORD=YourStrongPasswordHere123 \
  pc-opendata:27.0
```

#### 5. Running the Interactive SQL Shell in a Container
You can connect an interactive console session to an existing container volume or a newly created local file:
```bash
docker run -it --rm \
  -v opendata_data:/data \
  pc-opendata:27.0 /data/production.pcod
```

#### 6. Executing Non-Interactive SQL Statements
```bash
# Run a one-off query directly and exit
docker run --rm pc-opendata:27.0 --execute "SELECT 1 + 1 AS test;"

# Run multiple statements (schema creation, inserts, and selection)
docker run --rm pc-opendata:27.0 --execute "CREATE TABLE test(id INT, name TEXT); INSERT INTO test VALUES (1, 'Alpha'); SELECT * FROM test;"
```

#### 7. Running Built-in Self-Tests inside the Container
Verify engine functionality, vector indexing, transaction safety, and directory authentication directly inside the running container environment:
```bash
docker run --rm pc-opendata:27.0 --execute "SELF-TEST;"
```

#### Container Volumes & Configuration Reference

| Parameter / Path | Default | Description |
|---|---|---|
| `7338` | Port | Authenticated TLS server port (Native TLS & PostgreSQL wire protocol) |
| `/data` | Volume | Persistent database root storing `.pcod` and WAL journals |
| `/backups` | Volume | Atomic snapshot backup destination directory |
| `/run/tls` | Mount | Directory containing `server.pem`, `server-key.pem`, and `ca.pem` |
| `OPENDATA_SERVER_USER` | Environment | Bootstrap admin user created on first startup |
| `OPENDATA_SERVER_PASSWORD` | Environment | Bootstrap admin password (minimum 16 characters) |
| `OPENDATA_TLS_SERVER_NAME` | `localhost` | Server hostname used by internal TLS healthcheck probe |
| `OPENDATA_TLS_CA` | `/run/tls/ca.pem` | CA certificate file for healthcheck verification |

---

## 5. Database Lifecycle & File Management

### Creating a New Database
Create an empty, durable database snapshot using `CREATE DATABASE`:
```sql
CREATE DATABASE 'production.pcod';
```
You can configure automatic background backup scheduling right at creation:
```sql
CREATE DATABASE 'production.pcod' BACKUP 'D:\Backups\OpenData' INTERVAL 300;
```
*(Takes a verified atomic backup every 300 seconds).*

### Connecting and Disconnecting
```sql
-- Connect to an existing database
CONNECT 'production.pcod';

-- Connect in READONLY mode (prevents writes and file lock acquisition)
CONNECT 'reporting.pcod' READONLY;

-- Close active database and release file locks
DISCONNECT;
```

### Listing and Inspecting Local Databases
```sql
-- Scan current directory for all available database files (.pcod, .mdf, .dbf)
SHOW DATABASES;

-- Check engine metrics, active generation, and durability status
STATUS;
```

---

## 6. SQL Dialects, Data Types & Schema Definition

PC/OpenData SQL Server supports standard ANSI SQL syntax, along with native compatibility for Microsoft T-SQL bracketed identifiers (`[Table].[Column]`) and Oracle Database data types.

### Supported Data Types
| Data Type | Synonyms / Dialect Aliases | Description |
| :--- | :--- | :--- |
| `NUMBER` | `INT`, `INTEGER`, `BIGINT`, `SMALLINT`, `BIT`, `FLOAT`, `DECIMAL(p,s)`, `MONEY` | Arbitrary precision 38-digit BCD decimal storage. |
| `BOOLEAN` | `BOOL` | Boolean flag; accepts `TRUE` and `FALSE` literals (mapped to 1/0). |
| `SERIAL` | `BIGSERIAL`, `AUTO_INCREMENT`, `IDENTITY` | Auto-incrementing numeric primary key (calculated automatically on INSERT). |
| `VARCHAR2(n)` | `VARCHAR`, `VARCHAR(n)`, `NVARCHAR(n)`, `CHAR(n)`, `STRING`, `TEXT`, `CLOB` | UTF-8 character string (unparenthesized `VARCHAR`/`STRING`/`TEXT` default to 255 bytes). |
| `DATE` | `DATETIME`, `DATETIME2`, `TIMESTAMP` | ISO 8601 timestamps (`YYYY-MM-DD` or `YYYY-MM-DD HH:MM:SS`). |
| `VECTOR(dim)` | `VECTOR(n)` (up to 64 dimensions) | Native fixed-dimension float32 array for embeddings. |

### Creating Tables
```sql
-- Standard Relational Table
CREATE TABLE departments (
    dept_id NUMBER PRIMARY KEY,
    dept_name VARCHAR2(50) UNIQUE NOT NULL
);

-- Table with Foreign Keys and Cascade Actions
CREATE TABLE employees (
    emp_id NUMBER PRIMARY KEY,
    full_name VARCHAR2(60) NOT NULL,
    salary NUMBER,
    hire_date DATE,
    dept_id NUMBER REFERENCES departments(dept_id) 
        ON DELETE CASCADE ON UPDATE CASCADE
);

-- AI Embedding Table with Vectors
CREATE TABLE article_embeddings (
    doc_id NUMBER PRIMARY KEY,
    title VARCHAR2(100) NOT NULL,
    embedding VECTOR(3)
);
```

### SQL Dialect Tolerances & Ergonomics
PC/OpenData accepts syntax idioms from PostgreSQL, MySQL, and SQL Server:
* **Identifier Quoting**: Standard ANSI double quotes (`"my_col"`), MySQL backticks (`` `my_col` ``), and SQL Server brackets (`[my_col]`).
* **Column Defaults**: `DEFAULT <expr>` supporting literals, `TRUE`/`FALSE`, `NULL`, and `CURRENT_TIMESTAMP` / `NOW()` / `SYSDATE`.
* **Idempotent DDL**: `CREATE TABLE IF NOT EXISTS`, `DROP TABLE IF EXISTS`, and `ALTER TABLE ... ADD/DROP COLUMN IF NOT EXISTS`.
* **Table-less Queries**: `SELECT 1 + 1;` or `SELECT SYSDATE;` evaluate directly without requiring a `FROM` clause (implicit `FROM DUAL`).
* **Case-Insensitive ILIKE**: `ILIKE` and `NOT ILIKE` for case-insensitive pattern matching:
  ```sql
  SELECT * FROM employees WHERE full_name ILIKE 'a%';
  ```
* **Create Table As Select (CTAS)**:
  ```sql
  CREATE TABLE engineers AS SELECT * FROM employees WHERE dept_id = 1;
  ```

### Dynamic Constraints & Altering Tables
Tables can be altered dynamically without taking the database offline:
```sql
-- Add column with IF NOT EXISTS
ALTER TABLE employees ADD COLUMN IF NOT EXISTS nickname VARCHAR(50);

-- Modify column type or constraints
ALTER TABLE employees MODIFY COLUMN nickname VARCHAR(100);

-- Drop column with IF EXISTS
ALTER TABLE employees DROP COLUMN IF EXISTS nickname;
```

### Adding Constraints
Constraints can be dynamically added or removed from active tables without taking the database offline:
```sql
-- Add Unique Constraint
ALTER TABLE employees ADD CONSTRAINT uq_emp_name UNIQUE (full_name);

-- Add Foreign Key Constraint with Cascade Deletion
ALTER TABLE employees ADD CONSTRAINT fk_emp_dept 
    FOREIGN KEY (dept_id) REFERENCES departments(dept_id) 
    ON DELETE SET NULL;

-- Drop Constraint
ALTER TABLE employees DROP CONSTRAINT uq_emp_name;
```

### Secondary B+ Tree Indexes
While Primary Keys and Unique constraints maintain automated in-memory hash indexes, secondary columns can be indexed using ordered B+ Trees for range queries (`BETWEEN`, `>`, `<`):
```sql
CREATE INDEX idx_emp_salary ON employees(salary);
CREATE UNIQUE INDEX idx_dept_name ON departments(dept_name);

-- Drop Index
DROP INDEX idx_emp_salary;
```

### Window Functions (`OVER (PARTITION BY ... ORDER BY ...)`)
PC/OpenData supports standard SQL window functions across partitioned and sorted result sets:
* **`ROW_NUMBER() OVER ([PARTITION BY col] [ORDER BY col ASC|DESC])`**: Sequential row counter within each partition.
* **`RANK() OVER ([PARTITION BY col] [ORDER BY col ASC|DESC])`**: Rank assignment with gaps on tied values.
* **`DENSE_RANK() OVER ([PARTITION BY col] [ORDER BY col ASC|DESC])`**: Rank assignment without gaps on tied values.

```sql
SELECT dept, name, salary,
       DENSE_RANK() OVER (PARTITION BY dept ORDER BY salary DESC) AS dept_rank,
       ROW_NUMBER() OVER (ORDER BY salary DESC) AS overall_row
FROM employees;
```

### JSON Path Extraction & Operators
PC/OpenData provides native JSON querying operators and extraction functions:
* **`->`**: Extract JSON element or sub-object.
* **`->>`**: Extract JSON scalar value as unquoted plain text.
* **`JSON_VALUE(col, 'key')`**: Extract scalar string value at key.
* **`JSON_EXTRACT(col, 'key')`**: Extract JSON fragment at key.

```sql
SELECT profile->'address' AS addr_json,
       profile->>'email' AS email_text,
       JSON_VALUE(metadata, 'status') AS active_status
FROM user_accounts;
```

---

## 7. Data Manipulation & Transaction Control

### ACID Transactions
Every mutation in PC/OpenData is isolated in an in-memory working generation until committed.
```sql
BEGIN;

INSERT INTO departments VALUES (10, 'Engineering');
INSERT INTO employees VALUES (1, 'Ada Lovelace', 185000, '2026-01-15 09:00:00', 10);

-- Savepoint support
SAVEPOINT checkpoint_1;

UPDATE employees SET salary = salary + 10000 WHERE emp_id = 1;

-- Undo changes back to checkpoint
ROLLBACK TO SAVEPOINT checkpoint_1;

-- Commit durably to disk (increments durable generation counter)
COMMIT;
```

### High-Performance `UPSERT`
PC/OpenData supports atomic conflict resolution using both PostgreSQL-style `ON CONFLICT` and MySQL/Oracle-style `ON DUPLICATE KEY UPDATE`:
```sql
-- Do nothing if key already exists
INSERT INTO departments VALUES (10, 'Engineering') 
ON CONFLICT (dept_id) DO NOTHING;

-- Increment and update values using EXCLUDED values
INSERT INTO employees VALUES (1, 'Ada Lovelace', 195000, '2026-01-15 09:00:00', 10)
ON CONFLICT (emp_id) DO UPDATE SET 
    salary = EXCLUDED.salary,
    full_name = EXCLUDED.full_name;
```

### Advanced Querying, Window Functions & Joins
```sql
-- Analytical Window Functions: ROW_NUMBER() and RANK()
SELECT full_name, salary, 
       ROW_NUMBER() OVER (ORDER BY salary DESC) AS salary_rank,
       RANK() OVER (ORDER BY salary DESC) AS tie_rank
FROM employees;

-- Correlated Subqueries: WHERE [NOT] EXISTS
SELECT dept_name FROM departments d
WHERE EXISTS (
    SELECT 1 FROM employees e WHERE e.dept_id = d.dept_id AND e.salary > 150000
);

-- Inner and Outer Joins
SELECT e.full_name, e.salary, d.dept_name
FROM employees e
LEFT JOIN departments d ON e.dept_id = d.dept_id;
```

---

## 8. Native AI Vector Embeddings & Similarity Search

> **⏩ Skip if you're just getting started.** Vector search is an advanced feature for AI/semantic workloads. You can use PC/OpenData as a regular relational database without it.

PC/OpenData features native vector capabilities directly inside the database kernel. Embedding vectors are evaluated using SIMD-unrolled hardware instructions.

### Vector Syntax and Operators
* **`<=>`** : Cosine Distance (Returns `0.0` for identical vectors, up to `2.0` for opposite vectors).
* **`<->`** : Euclidean Distance (L2 Distance).
* **`COSINE_DISTANCE(col, '[x, y, ...]')`** : Functional cosine distance.
* **`L2_DISTANCE(col, '[x, y, ...]')`** : Functional Euclidean distance.

### Populating and Querying Vectors
```sql
-- 1. Create table with 3-dimensional vector embedding column
CREATE TABLE documents (
    id NUMBER PRIMARY KEY,
    title VARCHAR2(80),
    embedding VECTOR(3)
);

-- 2. Insert vectors as string literals
INSERT INTO documents VALUES (1, 'Database Architecture', '[1.0, 0.0, 0.0]');
INSERT INTO documents VALUES (2, 'Storage & B-Trees',      '[0.8, 0.6, 0.0]');
INSERT INTO documents VALUES (3, 'Culinary Recipes',       '[0.0, 0.0, 1.0]');
COMMIT;

-- 3. Nearest-Neighbor Semantic Search (Top-K Ordered by Similarity)
SELECT id, title, embedding <=> '[1.0, 0.0, 0.0]' AS distance
FROM documents
WHERE embedding <=> '[1.0, 0.0, 0.0]' < 0.50
ORDER BY distance ASC;
```

### HNSW (Hierarchical Navigable Small World) Vector Indexing
For high-dimensional embeddings and large-scale semantic datasets, PC/OpenData features native **HNSW** index graphs that accelerate nearest-neighbor queries from linear table scans to $O(\log N)$ graph traversals:
```sql
-- Create an HNSW vector index on an embedding column
CREATE INDEX idx_doc_hnsw ON documents USING HNSW (embedding);

-- High-speed approximate nearest neighbor (ANN) query
SELECT id, title, embedding <=> '[1.0, 0.0, 0.0]' AS distance
FROM documents
ORDER BY distance ASC
LIMIT 10;
```
HNSW indexes support both Cosine Distance (`<=>`) and Euclidean L2 Distance (`<->`), are automatically persisted across snapshot generations, and can be dropped via `DROP INDEX idx_name;`.

### Scalar Vector Quantization (SQ8)
For memory-constrained deployments and massive vector catalogs, PC/OpenData includes native **Scalar Quantization (SQ8)**:
* **75% Memory Reduction**: High-precision 32-bit floating-point vector dimensions are mapped into 8-bit signed quantized integers (`int8_t` values $\in [-127, 127]$) with per-vector dynamic scaling factors.
* **Fast Integer MAC Arithmetic**: Cosine similarity and Euclidean distances are computed using fast integer multiply-accumulate (MAC) instructions prior to floating-point dequantization:
  - `vector_quantize_sq8()` converts raw `VECTOR` buffers into compressed `VectorSQ8` structures.
  - `vector_sq8_cosine_distance()` and `vector_sq8_l2_distance()` provide rapid similarity evaluation with negligible loss of recall.
* Transparent integration with table scans and vector index structures ensures optimal throughput for high-concurrency vector workloads.

---

## 9. Cross-Engine Compatibility: Microsoft SQL Server & Oracle Files

> **⏩ Skip if you're just getting started.** This section covers migrating from or interoperating with Microsoft SQL Server and Oracle Database. You don't need it for normal use.

PC/OpenData can mount, import, export, and read database files from Microsoft SQL Server and Oracle Database natively.

### Native Binary File Headers
When reading or writing files, PC/OpenData identifies and verifies binary headers with CRC32 checks:
* **`.mdf`, `.ndf`, `.ldf`, `.bak`**: Microsoft SQL Server Header (`MSQLMDF1`)
* **`.dbf`, `.ora`, `.dmp`, `.ctl`**: Oracle Database Header (`ORCLDBF1`)
* **`.pcod`**: Native PC/OpenData Header (`PCOD0001`)

### Mounting Microsoft SQL Server `.mdf` Files Directly
You can open an `.mdf` file directly as your active database and query it using T-SQL bracketed identifiers:
```sql
CONNECT 'sales_data.mdf';

-- Query tables with bracketed T-SQL identifiers
SELECT [EmpID], [FullName], [Salary] 
FROM [Employees] 
WHERE [Salary] >= 100000;
```

### Exporting & Importing Across Formats
```sql
-- Export current active database into a binary Microsoft SQL Server .mdf file
EXPORT MSSQL TO 'legacy_backup.mdf';

-- Export a single table into an Oracle .dbf tablespace file
EXPORT TABLE employees TO ORACLE 'employees_ora.dbf';

-- Export table into T-SQL DDL/DML script with GO separators
EXPORT TABLE employees TO MSSQL 'employees_tsql.sql';

-- Import Microsoft SQL Server file into the currently open database
IMPORT MSSQL 'sales_data.mdf';
```

---

## 10. Data Recovery, Zero Data Loss Consensus & Backups

PC/OpenData implements an enterprise crash-recovery model guaranteeing zero silent data corruption.

### 1. Relational Integrity Checks
Run relational and physical storage diagnostics at any time:
```sql
CHECK INTEGRITY;
-- or
PRAGMA INTEGRITY_CHECK;
```
Verifies:
* Primary key uniqueness across internal hash slots.
* Foreign key reference targets and orphaned rows.
* CRC32 binary snapshot checksums.
* Column data boundary constraints.

### 2. Verified Periodic Backups
You can trigger an immediate verified snapshot backup:
```sql
BACKUP;
```
Or inspect backup health in the shell status report:
```sql
STATUS;
```
*(Displays `Storage health`, `Last backed-up generation`, and `Backup interval`).*

### 3. Compaction & Vacuuming
When tables undergo extensive updates and deletions, compact storage frames:
```sql
VACUUM;
-- or
COMPACT;
```

### 4. Zero Data Loss Quorum Replication (Raft Consensus)

> **⏩ Skip if you're just getting started.** Raft clustering is for multi-node high-availability deployments. A single-node database doesn't need it.

For multi-node high availability clusters:
* **Synchronous Dual-Write WAL Shipping**: A write is only committed once confirmed by a majority quorum of nodes.
* **Inspecting Cluster Status**:
  ```sql
  SHOW CLUSTER;
  ```
  Reports:
  * Cluster Node ID & Current Raft Term.
  * Node Role: `LEADER`, `FOLLOWER`, or `CANDIDATE`.
  * Sync WAL Status: `ENABLED (3/3 Nodes Acknowledged)`.

### 5. Continuous WAL Archiving & Point-In-Time Recovery (PITR)
PC/OpenData provides continuous write-ahead log (WAL) segment archiving and deterministic Point-In-Time Recovery (PITR) down to the exact second:

#### Continuous Archiving Workflow
When running with `--backup-dir <dir>`, the WAL subsystem continuously writes differential page records and archives sealed WAL segments into the backup repository alongside periodic snapshots.

#### Restoring via PITR (`--pitr <unix_timestamp>`)
To roll back uncommitted errors, recover from accidental table drops, or replay history up to an exact timestamp:
```bash
# Recover database to an exact Unix epoch timestamp (e.g., 1761921600)
opendata --database production.pcod --backup-dir C:\backups --pitr 1761921600
```
During PITR recovery:
1. The engine mounts the nearest verified snapshot recorded prior to the target time.
2. Archived WAL segment logs are scanned chronologically.
3. Transaction page deltas with commit timestamps $\le$ the target timestamp are replayed in atomic order.
4. Transactions committed after the target timestamp are discarded, leaving the database precisely at the designated point in history.

### 6. Automated Background Vacuum Scheduler
PC/OpenData includes an autonomous background vacuum watchdog that periodically reclaims storage and compacts table schemas without service downtime:
* **Bloat Detection**: Continuously monitors dead/deleted row metrics (`deleted_rows / total_rows`). When bloat exceeds the configurable threshold (default: 20%), a non-blocking vacuum sweep is scheduled.
* **Online Compaction**: Cooperatively runs during the background backup scheduler's idle intervals, executing `VACUUM` on eligible tables to defragment slotted data pages and reorganize B+ tree indexes.
* **Runtime Control**:
  ```sql
  -- Enable or disable auto-vacuum from the SQL shell
  SET AUTO_VACUUM ON;
  SET AUTO_VACUUM OFF;
  ```
  CLI startup flags:
  ```bash
  opendata --server --auto-vacuum       # Enable background autovacuum sweep (default)
  opendata --server --no-auto-vacuum    # Disable background autovacuum sweep
  ```

---

## 11. Shell Ergonomics, IRIS Editor & Observability

### Modern Interactive REPL
The PC/OpenData interactive shell provides a modern developer experience across Windows and Linux:
* **Cross-Platform ANSI Line-Editing**: Zero-dependency raw mode on both Windows and Linux. Navigate command history with Up/Down arrows and move the cursor horizontally with Left/Right/Home/End. History persists across sessions in `~/.opendata_history`.
* **Reverse History Search (`Ctrl+R`)**: Interactively search through command history by typing substrings. Press `Ctrl+R` to cycle through previous matches, `Enter` to select, and `Ctrl+C` to cancel.
* **Smart Tab Completion & Candidate Grid**: Press `Tab` to auto-complete SQL keywords, dot-commands (`.mode`, `.schema`, `.pager`, etc.), table names, and column names. If multiple candidates match, a formatted candidate grid is displayed.
* **Auto-Execution Intelligence**: Single-line complete statements execute immediately on `Enter`. In multi-line buffers (`   ...> `), pressing `Enter` on an empty line executes the buffered query immediately without requiring a trailing semicolon.
* **Interactive Query Results Pager**: Large result sets are automatically paginated:
  ```text
  -- More (120 remaining) [Enter: 1 row, Space: page, q: quit] --
  ```
  Toggle anytime with `.mode pager on | off` or `.pager [on | off]`.
* **Dynamic Context-Aware Prompt**: Displays the active database, uncommitted status, and read-only mode:
  ```text
  opendata (production.pcod)>          -- Clean committed state
  opendata (production.pcod *)>        -- Pending uncommitted mutations
  opendata (production.pcod RO)>       -- Read-only mode
  opendata>                            -- In-memory / no database attached
  ```
* **History Expansion**:
  * `!!`: Re-execute the most recent command.
  * `!<n>`: Re-execute command by history number.
  * `!-<n>`: Re-execute command $N$ positions back.
  * `!<prefix>`: Re-execute the most recent command starting with `<prefix>`.
* **SQLite & PostgreSQL Meta-Commands**:
  * `\dt` or `.tables`: List user tables.
  * `\d [table]` or `.schema [table]`: Describe table schema.
  * `\l`: List database files in the current directory.
  * `\du`: List user accounts and roles.
  * `\i <file>` or `.read <file>`: Execute a SQL script file.
  * `\x`: Toggle expanded / vertical display mode.
  * `\q` or `.exit`: Exit shell cleanly.

### Configurable Output Formatting
Customize how query results are rendered on screen:
```sql
SET FORMAT BOX;          -- Clean Unicode box-drawing table borders
SET FORMAT MARKDOWN;     -- GitHub Flavored Markdown table syntax (or SET FORMAT MD)
SET FORMAT VERTICAL;     -- Expanded vertical record cards (Postgres \x style)
SET FORMAT TABLE;        -- Clean ASCII aligned table (default interactive)
SET FORMAT TSV;          -- Tab-separated values (ideal for piping)
SET FORMAT JSON;         -- Pretty-printed JSON array
SET FORMAT CSV;          -- Comma-separated values
```
*(You can also use dot commands like `.mode box`, `.mode markdown`, `.mode vertical`, `.mode json`, or `.mode csv`)*.

### "Did You Mean?" Diagnostics & Syntax Carets
When a table or column name typo occurs, PC/OpenData uses Levenshtein distance matching to suggest the intended object:
```text
OpenData SQL> SELECT * FROM custmers;
ERROR: table does not exist: custmers. Did you mean 'customers'?

OpenData SQL> SELECT nam FROM employees;
ERROR: column 'nam' not found in table 'employees'. Did you mean 'name'?
```
Syntax errors display a caret (`^`) pointing directly to the offending token location.

### Human-Readable Telemetry
Database commands such as `STATUS;` and `SHOW DATABASES;` format byte counts into human-readable units (`B`, `KB`, `MB`, `GB`) and timestamps into relative times (`4 sec ago`, `2 min ago`, `3 days ago`).

### Destructive Command Safety, Confirmations & Dry-Run Impact Analysis
To protect against accidental data loss and allow developers to understand the blast radius of changes before committing them, the interactive shell provides built-in safety controls:

#### 1. Interactive Confirmation & Dry-Run Prompts
When executing destructive commands (`DROP TABLE`, `TRUNCATE TABLE`, `DROP DATABASE`, `DROP USER`, and unconditional `DELETE FROM` or `UPDATE` statements without a `WHERE` clause), the shell halts and prompts:
```text
--------------------------------------------------------------------------------
WARNING: Destructive Operation Detected!
  Operation: DROP TABLE
  Target:    customers
  Details:   This permanently removes the table and all of its rows.
--------------------------------------------------------------------------------
Proceed? Type 'yes' (or 'y') to continue, 'no' (or Enter) to cancel,
         or 'dry-run' (or 'test') to preview impact [y/N/test]: 
```
* **Confirm (`y` / `yes`)**: Proceeds with the operation.
* **Deny (`n` / `no` / Enter)**: Aborts the operation immediately; the database file is left untouched.
* **Test (`test` / `dry-run` / `d`)**: Displays a detailed **Dry-Run Impact Analysis** report showing table schema, row counts, secondary indexes, foreign key dependencies, and storage reclaimed. After the simulation, it prompts whether to proceed.

#### 2. Explicit `DRYRUN` and `TEST` Command Prefixes
You can preview the impact of any statement without being prompted by prefixing it with `DRYRUN` or `TEST`:
```sql
DRYRUN DROP TABLE customers;
TEST TRUNCATE TABLE orders;
TEST DELETE FROM log_events WHERE created_at < '2026-01-01';
DRYRUN DROP DATABASE 'legacy.pcod';
```

Example dry-run output:
```text
================================================================================
                    PC/OpenData Dry-Run Impact Analysis
================================================================================
  Operation:           DROP TABLE
  Target Object:       customers
  Database File:       data/app.pcod (Snapshot Generation: 5)
  Storage Engine:      MEMORY
  Current Rows:        1,250 row(s)
  Columns (4):         id, name, email, created_at
  Indexes to Drop:     1 index(es)
                         - idx_customers_email
  Foreign Key Impact:  Dependent tables referencing this table:
                         - orders.customer_id -> customers.id
  Estimated Storage:   ~100.0 KB of row data
  Schema Persistence:  Table schema and data will be permanently unlinked.
--------------------------------------------------------------------------------
RESULT: Dry-run test complete. Database file has NOT been modified.
================================================================================
```

#### 3. Configuration & Safe Mode
You can toggle destructive command confirmation prompts using `SET CONFIRM`:
```sql
SET CONFIRM ON;           -- Enable confirmation prompts (default in interactive shell)
SET CONFIRM OFF;          -- Disable prompts for batch testing
```
*(Automated scripts, piped inputs, and non-interactive `opendata -e` invocations automatically bypass interactive confirmation prompts).*

### Finding help quickly

`HELP <topic>;` shows focused guidance for `SELECT`, `CREATE`, `DDL`, `META`,
`SET`, `SAFETY`, `CLI`, `TRANSACTIONS`, `VECTORS`, `SERVER`, `IMPORT`, `SECURITY`,
`BACKUP`, `DIAGNOSTICS`, and `CONNECTION`. You don't need exact spelling:

- **Near-miss spellings** show the closest topic: `HELP SELEC;` shows the
  `SELECT` topic with a note about the match.
- **Keywords** find related topics: `HELP spreadsheet;` points to `HELP IMPORT;`.
- Unknown words fall back to the full `HELP;` reference.

### Built-In IRIS Full-Screen Text Editor
PC/OpenData includes **IRIS**, an internal GNU Nano-styled text editor embedded directly in the binary. You can draft, edit, and execute complex multi-line SQL scripts without leaving the database shell.

```sql
-- Open IRIS on the current SQL query buffer
IRIS;

-- Open IRIS on an external file
IRIS 'migration.sql';
```
#### Keybindings in IRIS:
* **`Ctrl+O`**: Write Out / Save buffer to disk.
* **`Ctrl+R`**: Read external file into buffer.
* **`Ctrl+W`**: Search for text string.
* **`Ctrl+K`**: Cut line to clipboard.
* **`Ctrl+U`**: Uncut / Paste line.
* **`Ctrl+J`**: Auto-format and justify SQL syntax.
* **`Ctrl+T`**: **Execute SQL buffer immediately** in the database engine.
* **`Ctrl+X`**: Exit IRIS editor.

### Diagnostics & Performance Benchmarking
```sql
-- Benchmark a query over N iterations and report latency & QPS
BENCHMARK 100 SELECT * FROM employees WHERE salary > 100000;

-- Live Watch: Execute query repeatedly at set intervals
WATCH INTERVAL 2 COUNT 5 SELECT COUNT(*) FROM orders;

-- Cost-Based Optimizer (CBO) Explain Plan
EXPLAIN SELECT * FROM employees WHERE salary > 150000;

-- Reverse-engineer schema DDL
SHOW CREATE TABLE employees;

-- Compare active schema against another database file
DIFF SCHEMA 'staging.pcod';
```

### Active Session Management
```sql
-- View all active client sessions (Console, TCP, PGv3 Wire)
SHOW PROCESSLIST;
-- or
SHOW SESSIONS;

-- Terminate a client session by ID
KILL SESSION 2;

-- Zero-downtime hot-patch restart of a running server (ADMIN over the network)
RESTART;
```

### Batch Scripting & Spooling
```sql
-- Execute batch script from disk
SOURCE 'setup_schema.sql';
-- or
@setup_schema.sql;

-- Mirror all console output to an audit transcript file
SPOOL 'audit_log.txt';
-- Turn off spooling
SPOOL OFF;
```

---

## 12. Network Servers & Wire Protocols

> **⏩ Skip if you're just getting started.** This section covers running PC/OpenData as a network server. For local use, the interactive shell is all you need.

PC/OpenData can run as an in-process engine, a dedicated TCP/TLS server, or a drop-in replacement for PostgreSQL client tools.

### One-Command Local Development Server (`--dev`)
For local development and testing, run:
```bash
opendata --dev
```
This immediately starts an asynchronous local server on `127.0.0.1:7350` bound to `dev.pcod`, automatically provisions an administrative user `dev` with password `devpassword12345` if no accounts exist, and prints connection guidance.

### Masked Password Entry
When creating or modifying accounts on the console, you can avoid plain-text password exposure in terminal history by using `IDENTIFIED BY PROMPT`:
```sql
CREATE USER 'analyst' IDENTIFIED BY PROMPT ROLE READ_ONLY;
Enter password for user analyst: **********
User created.
```
If you launch `--server` without any user accounts configured and with an interactive terminal attached, the server automatically prompts you to create an initial admin account before opening the port.

### 1. High-Performance Asynchronous Non-Blocking Reactor
Runs a single-threaded asynchronous reactor using `WSAPoll` / `poll` multiplexing capable of handling hundreds of concurrent connections with zero worker thread contention:
```bash
opendata --database production.pcod --server --port 7350
```

### 2. PostgreSQL v3 Wire Protocol Compatibility
PC/OpenData includes a native, zero-dependency PostgreSQL v3 wire protocol reactor front-end. Any standard PostgreSQL client tool, management GUI, or language driver can connect directly to PC/OpenData without custom adapters:

* **Compatible Drivers & Management GUIs**:
  * `psql` interactive command line
  * DBeaver, TablePlus, DataGrip, VS Code PostgreSQL extensions
  * Python (`psycopg2`, `asyncpg`, `SQLAlchemy`)
  * Node.js (`pg`, `knex`, `Prisma`, `TypeORM`)
  * Go (`pgx`, `lib/pq`), Rust (`tokio-postgres`, `sqlx`)
  * Java / JDBC (`postgresql-*.jar`), .NET (`Npgsql`)

* **Wire Negotiation Flow**:
  * Complete SSL negotiation (`SSLRequest`), Password Cleartext, and MD5 challenge handshakes.
  * Standard PostgreSQL `ParameterStatus` negotiation (`server_version`, `client_encoding`, `DateStyle`).
  * Seamless message processing for Simple Query (`'Q'`), RowDescription (`'T'`), DataRow (`'D'`), CommandComplete (`'C'`), and ReadyForQuery (`'Z'`).
  * Emits PostgreSQL standard format error packets (`'E'`).

```bash
# Connect using standard psql CLI
psql -h 127.0.0.1 -p 7350 -U admin -d production
```

### 3. Enterprise Active Directory & LDAP Authentication
PC/OpenData SQL Server provides native, cross-platform enterprise directory integration:
* **Active Directory (AD)**: Direct domain controller authentication via User Principal Name (UPN) binding (`user@corp.domain.com` or `DOMAIN\user`). On Windows, uses native `wldap32.dll` with zero external dependencies. On Linux, integrates with `libldap`.
* **Generic LDAP / OpenLDAP / FreeIPA**: Configurable LDAP servers, search base DNs, custom user attributes (`uid`, `sAMAccountName`), and LDAPS / SSL encryption.
* **Security Group Role Mapping**: Maps designated corporate security groups (e.g., `Domain Admins`) directly to the server's internal `ADMIN` role. Authenticated directory users outside the admin group automatically receive the `READ_ONLY` role.
* **Transparent Local Fallback**: When directory authentication is configured, incoming client attempts authenticate against AD/LDAP first. If the user does not exist in the directory, the server seamlessly falls back to verify against local database accounts.

#### Active Directory Launch Example
```bash
opendata --server --database prod.pcod \
    --ad-server dc01.corp.contoso.com \
    --ad-domain corp.contoso.com \
    --ad-admin-group "Domain Admins"
```

#### Generic LDAP / OpenLDAP Launch Example
```bash
opendata --server --database prod.pcod \
    --ldap-server ldap.internal.org \
    --ldap-port 636 \
    --ldap-ssl \
    --ldap-base-dn "ou=users,dc=internal,dc=org" \
    --ldap-admin-group "cn=dbadmins,ou=groups,dc=internal,dc=org"
```

### 4. Secure Enterprise TLS Network Server
```bash
opendata.exe --database production.pcod --tcps --port 7338
```
*(Windows builds leverage native Windows Schannel TLS with zero external OpenSSL DLL requirements).*

#### TLS wallets

> **⏩ Skip if you're just getting started.** TLS wallets are for encrypted network deployments. Local shell use doesn't need them.

`--wallet <directory>` selects a strict version-1 `opendata-wallet.conf` instead
of `--cert`, `--key`, and `--cluster-ca`. Relative PEM paths are resolved from
the wallet directory. POSIX private keys must be unencrypted and accessible
only by their owner (for example, `chmod 600 server-key.pem`). The server and
cluster identities are separate:

```ini
version=1
server.cert=server-chain.pem
server.key=server-key.pem
cluster.cert=node-chain.pem
cluster.key=node-key.pem
cluster.ca=cluster-ca.pem
```

Server-only wallets need just `version`, `server.cert`, and `server.key`.
Cluster mode requires `cluster.cert`, `cluster.key`, and `cluster.ca`.
Unknown, duplicate, malformed, absolute, or parent-traversing path entries
are rejected. Encrypted keys and PKCS#12 are not supported.

POSIX wallets enforce filesystem integrity as well as key confidentiality.
The wallet directory, manifest, PEM files, and their path components must be
owned by the effective service user or root. They must not be group/world
writable; private keys additionally allow no group/world permissions.
All manifest and PEM inputs must be regular files. Symlinks are rejected at
every component, including the wallet directory and its ancestors; relative
paths cannot escape the wallet. A root-owned sticky ancestor such as `/tmp`
is allowed, but the wallet directory itself must be protected. Provision
wallets beneath trusted, non-writable directories (for example a root-owned
configuration directory or a service-owned `0700` directory).
These checks assume the service user and root are trusted; they do not
protect against those principals replacing their own credentials.

On Windows, the wallet references certificates already provisioned in the
current user's stores; private keys remain in the OS store and are never
exported:

```ini
version=1
server.store=CurrentUser\My
server.thumbprint=REPLACE_WITH_40_HEX_THUMBPRINT
cluster.store=CurrentUser\My
cluster.thumbprint=REPLACE_WITH_40_HEX_THUMBPRINT
cluster.trust_store=CurrentUser\Root
```

`--inspect-wallet <directory>` validates the configuration and reports its
resolved certificate/key paths or Windows store references without displaying
private key material. Existing direct certificate flags remain supported and
cannot be combined with `--wallet`.

#### Certificate rotation and CA rollover

Wallet credentials are loaded at startup, not hot-reloaded. Provision a new
protected wallet directory instead of replacing files used by a running process.
Keep the previous protected wallet available until rollback is no longer needed.

1. Obtain a matching certificate/key and intermediate chain. Check the endpoint
   SAN and role-specific EKUs; cluster identities need both client/server use,
   the configured peer IP, and `opendata-node-<id>` as the common name.
2. Run `opendata --inspect-wallet <new-directory>` under the service account.
   POSIX cluster inspection verifies its identity against `cluster.ca`; inspection
   does not establish that remote peers trust the new issuer. Windows inspection
   is read-only and does not install trust or export private keys.
3. Before changing issuers, distribute a trust bundle containing both old and
   new CA chains to every peer/client through your normal provisioning system.
   On Windows, a trusted administrator provisions the required system/user trust;
   the application and test harness do not install it for production.
4. Stop and restart with the new wallet. Verify a real client connection with
   chain and hostname/IP verification, then check authenticated queries and
   cluster commit acknowledgments. Never disable peer verification to get through
   a rollout. The cluster prototype has no automatic failover; schedule downtime
   where a rolling restart cannot preserve quorum.
5. If verification fails, stop and restart with the previous still-valid wallet
   and compatible overlap trust. A compromised/expired identity is not a safe
   rollback target; revoke and replace it instead. Retain recovery access offline.
6. Only after all endpoints have migrated and rollback is retired, remove old CA
   trust and retire old private keys using the platform's credential procedures.
   Monitor expiry ahead of time; a new certificate still requires restart.

Never use `OPENDATA_TEST_TLS_FIXTURES` in a release build. Isolated Windows
fixtures intentionally replace identity selection only in their dedicated test
binary and cannot certify production store provisioning.

### Zero-downtime hot-patch restarts

> **⏩ Skip if you're just getting started.** Hot-patch restarts are for production server upgrades. You don't need them for local development.

A running standalone server replaces its own binary without dropping the listening
socket, so clients never see a refused connection:

```sql
-- As an ADMIN network session (or the trusted local console):
RESTART;
```

On POSIX, `kill -USR2 <pid>` or `kill -HUP <pid>` triggers the same restart. Do not
use `systemctl reload` with the shipped `Type=simple` unit: systemd stops the unit
when the predecessor exits after the handoff and would signal the successor. The
server spawns its
successor from the original launch path — place the patched binary there first — with
the original arguments (a one-time `--restore` is not repeated). The listener is handed
to the successor (inherited descriptor on POSIX, `WSADuplicateSocket` on Windows) while
the kernel backlog keeps queuing new connections. In-flight replies are flushed within
a bounded five-second drain, idle sessions are closed, the predecessor releases the
database lock and exits cleanly, and the successor reloads the durable snapshot and
resumes service. Uncommitted transactions are discarded, exactly as at a normal exit.

If the successor cannot be launched, the restart aborts and the predecessor keeps
serving. Cluster nodes refuse `RESTART`; restart them one at a time so the quorum
survives. Use a hot-patch restart to pick up a replaced binary or a newly provisioned
TLS wallet. `SIGTERM`/`SIGINT` stop the server gracefully without spawning a successor.

### Running as a production service

> **⏩ Skip if you're just getting started.** This section is for production deployment.

**Linux (systemd).** A hardened unit file ships at `opendata.service` in the
repository root. It runs the server as a dedicated `opendata` user, restarts on
failure, and documents why `systemctl reload` is unsafe with `Type=simple` (use
`kill -USR2 <pid>` for hot-patch restarts instead). Install it with:

```bash
sudo useradd --system --home /var/lib/opendata opendata
sudo mkdir -p /var/lib/opendata /var/backups/opendata /etc/opendata
sudo cp opendata.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now opendata
```

Edit `ExecStart` in the unit to point at your database, backup directory, and TLS
certificate. Put bootstrap credentials in `/etc/opendata/server.env` with mode
`0600` (`OPENDATA_SERVER_USER` / `OPENDATA_SERVER_PASSWORD`) for first startup only.

**Windows.** Register the server as a Windows service with `sc.exe` or a service
manager such as NSSM:

```powershell
sc.exe create OpenData binPath= "C:\OpenData\bin\opendata.exe --database C:\OpenData\data\production.pcod --backup-dir D:\Backups\opendata --tcps --port 7338 --cert store:<thumbprint>" start= auto
sc.exe failure OpenData reset= 60 actions= restart/5000/restart/5000/restart/10000
sc.exe start OpenData
```

Run the service under a dedicated low-privilege account, grant it write access only
to the database and backup directories, and store the TLS certificate in that
account's `CurrentUser\My` store. Use `RESTART;` from an authenticated ADMIN session
for zero-downtime binary upgrades.

**Docker & Podman (Container).** A production multi-stage Alpine container is provided at [`docker/Dockerfile`](docker/Dockerfile) along with a turnkey [`docker/docker-compose.yml`](docker/docker-compose.yml). It drops privileges to dedicated user `opendata`, provides automated internal TLS healthchecks on port `7338`, and mounts persistent volumes for `/data` and `/backups`:

```bash
docker compose -f docker/docker-compose.yml up -d
```

### Database users and roles
Network authentication uses the durable database account catalog. For a new database or
an older snapshot without a catalog, set `OPENDATA_SERVER_USER` and
`OPENDATA_SERVER_PASSWORD` for the first server startup. This creates one administrator
and persists a salted PBKDF2-HMAC-SHA-256 verifier (210,000 iterations); plaintext
passwords are not stored. After bootstrap, the environment variables are optional.
Use a secret manager for bootstrap and do not put passwords in command-line arguments.
For clusters, provision the leader before replication starts. A generation-zero,
empty follower does not bootstrap independent credentials: it waits for the
leader's authenticated snapshot, rejecting database logins until accounts arrive.
For an existing account-less follower, provision a leader snapshot offline rather
than enabling local writes or independently initializing a different database identity.

Passwords must be 16-255 bytes. Usernames must be 1-63 printable ASCII characters without
whitespace or apostrophes. Account management over a network connection requires
an authenticated `ADMIN`; trusted local console/embedded API access is privileged.
Run `HELP;` in the local shell for the built-in **DATABASE USER ADMINISTRATION**
command reference. Keep passwords out of shell arguments, scripts checked into
source control, and spooled transcripts.

#### Create and list users

Create a read-only account:
```sql
CREATE USER 'analyst' IDENTIFIED BY 'generate-a-long-secret' ROLE READ_ONLY;
COMMIT;
```

Create another administrator and list accounts (passwords/verifiers are never listed):
```sql
CREATE USER 'backupadmin' IDENTIFIED BY 'another-unique-long-secret' ROLE ADMIN;
COMMIT;
SHOW USERS;
```

#### Change passwords, roles, or access

Each operation below is independent; commit the change you want to apply:
```sql
ALTER USER 'analyst' ROLE ADMIN;
COMMIT;
ALTER USER 'analyst' PASSWORD 'another-long-generated-secret';
COMMIT;
ALTER USER 'analyst' DISABLE;
COMMIT;
ALTER USER 'analyst' ENABLE;
COMMIT;
```

Disabling an account retains it for later re-enabling. To permanently remove it:
```sql
DROP USER 'analyst';
COMMIT;
```
Use `ROLLBACK;` instead of `COMMIT;` to discard pending account changes when
autocommit is off.

The built-in roles are database-wide: `ADMIN` can read, mutate, and manage accounts;
`READ_ONLY` starts with no table access over the network. The last enabled
administrator cannot be disabled, demoted, or dropped. Account changes use normal
transaction semantics and become effective after `COMMIT`; existing connections are
rechecked on their next request. The transaction engine remains process-global, so this
does not provide tenant isolation or independent concurrent transactions. Network users,
including admins, cannot use local shell/file commands.
A pending network transaction has one owning connection; other network
connections receive a busy error until commit/rollback, and owner disconnect
rolls it back. This ownership guard does not isolate trusted local console or
embedded API callers, which bypass network authentication. Use separate
processes and OS access controls for independent trust domains.

#### Per-table privileges (GRANT / REVOKE)

`READ_ONLY` network users have no table access until an administrator grants it:

```sql
GRANT SELECT ON employees TO 'analyst';
GRANT SELECT, INSERT ON timesheets TO 'analyst';
GRANT ALL ON reports TO 'analyst';        -- SELECT + INSERT + UPDATE + DELETE
REVOKE ON employees FROM 'analyst';       -- remove all grants on that table
SHOW GRANTS;                              -- list every grant (ADMIN only)
COMMIT;
```

Grants are durable — they are stored in the database snapshot and flow through
backups and replication. `ADMIN` users always have full access; grants apply only
to `READ_ONLY` users. DDL (`CREATE`/`ALTER`/`DROP`/`CREATE INDEX`) still requires
the `ADMIN` role. Grants are per-table, not per-column or per-row.

Both wire protocols share authentication admission limits keyed by the actual
peer IPv4 address, not its ephemeral port or forwarded headers. A source/name
pair gets five attempts, replenishing one per five seconds; a source gets
twenty attempts, replenishing one per two seconds. A process-wide budget of
sixty-four attempts replenishes one per 100 milliseconds. All password
checks, including successful ones, consume tokens. Rejections are generic,
with no account-wide hard lockout across source addresses or sleeps in the
network loop. Existing authenticated sessions are not throttled. Source and
source/name tables hold at most 256 and 512 entries; inactive entries expire
after sixty seconds. If all entries are active, new entries fail closed rather
than evicting an attacker's exhausted budget. Proxies/NAT share an IP budget;
deploy upstream connection controls for distributed or volumetric attacks.
The reactor writes aggregate `AUTH_STATS` counters to stderr at startup, every
sixty seconds, and normal shutdown. Counters separate admitted password checks,
credential failures, per-source/pair/global throttling, and source/pair table
saturation. They reset at server startup and contain no usernames, passwords,
or source addresses. They are operator diagnostics, not a network account lookup.
Forced process termination can lose the last interval's report.

The account catalog is included in snapshots, backups, and replicated commits. New
versions can read and migrate older account-less snapshots. Older binaries cannot reopen
snapshots after the account-catalog extension has been written.

### Offline administrator recovery

There is no network password-reset bypass. OS access to the database and local
console/embedded API is privileged access; protect it accordingly.

1. Stop the server and, for a cluster, stop every node that could write or replicate.
   Confirm the database's exclusive file lock is available. Preserve the current
   database and verified backups before making changes; do not overwrite the only
   copy or remove account data from a snapshot by hand.
2. As the authorized service/OS administrator, open the database locally with
   `opendata --database <database-file> --subsystem`, without `--server`, cluster
   options, or `--readonly`. Use the existing trusted local `SHOW USERS`,
   `ALTER USER '<name>' PASSWORD '<new-secret>'`, `ENABLE`, and `ROLE ADMIN`
   commands, followed by `COMMIT`. If necessary, create a new admin using
   `CREATE USER ... ROLE ADMIN`. The last-enabled-admin rule still applies.
   Supply the SQL through a protected local input mechanism: do not put secrets
   in `--execute`, shell arguments/history, checked-in SQL, spooling, recordings,
   or logs. Local input is not a masked password prompt.
3. Exit, reopen locally, and confirm the intended enabled admin exists. Start a
   standalone server on loopback and verify a fresh login with the new password,
   then stop it before returning to the production configuration.
4. For a cluster, recover the authoritative leader's snapshot and provision
   followers consistently from it while stopped, preserving database identity
   and generation. Do not initialize independent follower account catalogs.
   Start with the normal mTLS configuration and verify account changes replicate.
5. If corruption prevents local recovery, restore a verified compatible backup
   to a **new** database path using the existing restore workflow, then repeat
   local account management. A backup can contain older passwords and account
   states: rotate recovered credentials and review enabled administrators.
   Keep the original unchanged until the restored database is verified.

This procedure uses existing local privileges and transaction semantics; it adds
no new credential-reset command, environment override, or authentication exemption.

### Query Timeout Watchdog (`statement_timeout`)

To prevent runaway nested loops, expensive unindexed table scans, or pathological client queries from degrading server availability:
* **Cooperative Watchdog**: During query execution (such as sequential table scans, joins, and row updates), the engine periodically samples elapsed monotonic time against `g_statement_timeout_ms`.
* **Safe Abort**: If the timeout deadline is exceeded, query processing aborts immediately with `ERROR: statement timeout exceeded (<ms> ms)`. The pending transaction is cleanly protected without risking database corruption or leaking memory.
* **Configuration**:
  ```bash
  # Start server with 10-second default statement timeout
  opendata --server --statement-timeout 10000
  ```
  ```sql
  -- Set timeout for current session (in milliseconds; 0 disables timeout)
  SET STATEMENT_TIMEOUT 5000;
  ```

### Kerberos / SPNEGO Directory Authentication

PC/OpenData supports enterprise Kerberos v5 and SPNEGO single sign-on (SSO) ticket verification:
* **Seamless GSSAPI Ticket Validation**: Clients connecting via PostgreSQL v3 wire protocol or native wire connections can submit ASN.1 SPNEGO / Kerberos initial context tickets (`0x60`, `0xa0`, `0x6e` application tags).
* **Cross-Platform Security**: On Windows, tickets can be validated using the Native SSPI / Kerberos security support provider. On Linux, tickets can be authenticated against system keytabs via GSSAPI / MIT Kerberos libraries.
* **Role Mapping**: Validated Kerberos ticket principals (e.g. `user@REALM.COM` or `service/host@REALM.COM`) are mapped to `ADMIN` or `READ_ONLY` database roles automatically based on service configuration.
* **Configuration**:
  ```bash
  opendata --server \
    --krb-realm "CORP.EXAMPLE.COM" \
    --krb-service "postgres/db.corp.example.com" \
    --krb-keytab "/etc/security/keytabs/opendata.keytab"
  ```

---

## 13. Visual Studio Code Extension Guide

PC/OpenData provides an official Visual Studio Code extension (**PC/OpenData SQL Server & Vector Studio**) packaged as `pc-opendata-vscode-27.0.0.vsix`. It allows developers to browse snapshots, inspect binary `.pcod`/`.mdf`/`.dbf` files visually, run queries with CodeLens, and view real-time engine telemetry directly inside the editor.

### Key Capabilities in v27.0:
* **Multi-Line CodeLens Execution**: `▶ Run Statement` accumulates multi-line SQL statements until the terminating semicolon (`;`), allowing full complex queries to run from a single click.
* **Live Engine Introspection**: The **Tables & Vectors** explorer dynamically inspects the active database snapshot using `SHOW TABLES;` and `DESCRIBE <table>;` to display actual column names, types, and primary keys.
* **Interactive Snapshot Inspector**: Open any `.pcod`, `.mdf`, or `.dbf` file directly in the visual custom editor. Click **Export to SQL** to dump full DDL/DML, or **Check Integrity** to verify CRC32 and relational consistency.

### Getting Started Walkthrough

After installing, open the Command Palette (`Ctrl+Shift+P`) and run
**PC/OpenData: Open Getting Started Walkthrough**. Four guided steps take you
from zero to a working database:

1. **Open or create a database** — one click opens (or creates) `mydata.pcod`
   in your workspace via the `PC/OpenData: Open Getting Started Database` command.
2. **Run your first query** — paste `LOAD SAMPLE;` and `SELECT * FROM SAMPLE_DATA;`
   into a `.sql` file and use the CodeLens **▶ Run Statement** link.
3. **Explore your schema** — the Activity Bar views show snapshots, tables,
   vectors, and cluster health.
4. **Next steps** — guided CSV import, verified backups, and secure sharing.

### Installing the Extension

#### Method 1: Using the Command Line
```powershell
code --install-extension E:\Github\pc-opendata\pc-opendata-vscode-27.0.0.vsix --force
```

#### Method 2: Inside the VS Code UI
1. Open Visual Studio Code.
2. Press `Ctrl+Shift+X` to open the **Extensions** view.
3. Click the **`...`** (Views and More Actions) menu in the upper-right corner of the Extensions panel.
4. Select **Install from VSIX...**
5. Navigate to your repository root and select `pc-opendata-vscode-27.0.0.vsix`.
6. Press `Ctrl+Shift+P` and execute `Developer: Reload Window`.

---

### Extension Visual Mockup (How It Looks in VS Code)

```
+-------------------------------------------------------------------------------------------------------------------------+
| [O] [o] [o]  pc-opendata-workspace — Visual Studio Code                                                                 |
+----+-----------------------+--------------------------------------------------------------------------------------------+
| 📁 | PC/OPENDATA EXPLORER  | [📄 vector_queries.sql]  [🟦 mssql_store.mdf (Inspector)]  [⚙️ Engine Telemetry]           |
| 🔍 | --------------------- |--------------------------------------------------------------------------------------------+
| 🌿 | ▾ Workspace Snapshots |  ▶ Run Statement  |  ⚡ Benchmark (100x)  |  🔍 Explain CBO Plan                               |
| [P]|   🟢 production.pcod  |  SELECT id, doc, vec <=> '[1.0, 0.0, 0.0]' AS cosine_dist,                                |
|    |   🟦 mssql_store.mdf  |         L2_DISTANCE(vec, '[1.0, 0.0, 0.0]') AS l2_dist                                     |
|    |   🟥 oracle_store.dbf |  FROM embeddings                                                                           |
|    |                       |  WHERE vec <=> '[1.0, 0.0, 0.0]' < 0.50 ORDER BY cosine_dist ASC;                          |
|    | ▾ Tables & Vectors    |--------------------------------------------------------------------------------------------+
|    |   ▾ 📋 INV (2 rows)   |  PC/OPENDATA RESULTS               OUTPUT    TERMINAL           [✓ SUCCESS (0.294 ms)]      |
|    |     🔑 ID : NUMBER    |  +----+--------------+-------------------+---------+-----------------------------------+  |
|    |     🔒 NAME : VARCHAR |  | ID | DOC          | COSINE_DIST (<=>) | L2_DIST | SIMILARITY SCORE                  |  |
|    |     📦 QTY : NUMBER   |  +----+--------------+-------------------+---------+-----------------------------------+  |
|    |   ▾ 🧠 EMBEDDINGS     |  | 1  | SQL Kernel   | 0.0000            | 0.0000  | [██████████████████████████] 100% |  |
|    |     🔑 ID : NUMBER    |  | 2  | Vector Index | 0.2000            | 0.4472  | [████████████████████      ]  80% |  |
|    |     📄 DOC : VARCHAR  |  +----+--------------+-------------------+---------+-----------------------------------+  |
|    |     📐 VEC: VECTOR(3) |  2 rows selected via 4-way SIMD float32 dot-product vectorization.                         |
|    |                       |--------------------------------------------------------------------------------------------+
| ⚙️ |  Async Wire: Port 7350|  Language Server: Active (Grammar: PC/OpenData 27.0)                                       |
+----+-----------------------+--------------------------------------------------------------------------------------------+
| 🗄️ PC/OpenData: production.pcod (Gen 3)  |  ✓ Clean (No Pending Tx)  |  ⚡ PGv3 Shim: 127.0.0.1:7350  |  UTF-8 | C11 Consensus |
+-------------------------------------------------------------------------------------------------------------------------+
```

---

### Core Extension Features & How to Use Them

#### 1. Custom Binary File Inspector (`.pcod`, `.mdf`, `.dbf`)
* **How to use**: In the VS Code File Explorer or the **PC/OpenData Explorer** sidebar, single-click any database file (e.g. `production.pcod`, `finance.mdf`, or `erp.dbf`).
* **What you see**:
  - The file's binary signature header (`PCOD0001`, `MSQLMDF1`, `ORCLDBF1`).
  - Checksum integrity status (`CRC32 Valid`).
  - Active durable generation count.
  - Interactive table summary with column data types and row counts.
  - Quick action buttons: **Export to SQL** and **Check Integrity**.

#### 2. SQL & Vector CodeLens
* **How to use**: Open or create any `.sql` file in your workspace.
* **What happens**: Actionable CodeLens links appear automatically directly above every executable statement (`SELECT`, `INSERT`, `UPDATE`, `CREATE TABLE`, `BENCHMARK`, etc.):
  - Click **`▶ Run Statement`**: Executes the query immediately against the engine and displays results in the adjacent webview panel.
  - Click **`⚡ Benchmark (100x)`**: Runs the statement 100 times silently and renders average, minimum, maximum latency, and query-per-second (QPS) throughput.
  - Click **`🔍 Explain CBO Plan`**: Formats and displays the Cost-Based Optimizer execution plan tree and sampled cardinalities.

#### 3. Native SIMD Vector Visualizer
When running semantic vector search queries utilizing `<=>` (Cosine Distance) or `<->` (Euclidean Distance), the results table displays:
* Exact float distance values.
* Real-time visual similarity score bars (from `0%` to `100%`) showing relative semantic proximity at a glance.

#### 4. Active Status Bar Monitor
The blue status bar at the bottom of the VS Code window provides continuous database state:
* **Active Database**: Indicates the connected file and current generational commit number.
* **Transaction State**: Displays `✓ Clean` or `* Pending Uncommitted Changes`.
* **Wire Protocol Port**: Live indicator for the embedded PostgreSQL v3 / Async Wire shim (`127.0.0.1:7350`).
* Clicking the status bar automatically runs `SHOW ENGINE STATUS;` and reveals buffer pool residency metrics.

---


---

## 14. Embedded C API & Language Drivers (Python, Node.js)

### Embedded C Library API
Include `opendata.h` and link against `opendata.exe` (or `opendata.dll` / engine objects):

```c
#include "opendata.h"
#include <stdio.h>

int main(void) {
    // Open or create database
    if (!opendata_open("app.pcod", "backups", 60, NULL)) {
        fprintf(stderr, "Error: %s\n", opendata_errmsg());
        return 1;
    }

    // Execute DDL
    opendata_exec("CREATE TABLE IF NOT EXISTS metrics (id SERIAL PRIMARY KEY, score NUMBER, emb VECTOR(3));");

    // Prepared statement with typed binders (1-based index)
    OpendataStmt* stmt = opendata_prepare("INSERT INTO metrics (score, emb) VALUES (?, ?);");
    opendata_bind_double(stmt, 1, 98.5);
    float vec[3] = {1.0f, 0.0f, 0.5f};
    opendata_bind_vector(stmt, 2, vec, 3);
    opendata_step(stmt);
    opendata_finalize(stmt);

    // Commit changes
    opendata_commit();

    // Query with typed column getters (0-based index)
    stmt = opendata_prepare("SELECT id, score, emb FROM metrics;");
    while (opendata_step(stmt)) {
        int64_t id = opendata_column_int64(stmt, 0);
        double score = opendata_column_double(stmt, 1);
        float out_vec[3];
        opendata_column_vector(stmt, 2, out_vec, 3);
        printf("Row: id=%lld, score=%.1f, vec=[%g, %g, %g]\n",
               (long long)id, score, out_vec[0], out_vec[1], out_vec[2]);
    }
    opendata_finalize(stmt);

    opendata_close();
    return 0;
}
```

### Python DB-API 2.0 Driver (`bindings/python/opendata.py`)
Zero external dependencies (uses standard library `ctypes` and `subprocess`). Fully conforms to PEP 249:

```python
import opendata

# Connect to database file
conn = opendata.connect('app.pcod')
cur = conn.cursor()

# Create table and insert data
cur.execute("CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, name VARCHAR, active BOOLEAN DEFAULT TRUE);")
cur.execute("INSERT INTO users (name) VALUES (?);", ("Alice",))
cur.execute("INSERT INTO users (name, active) VALUES (?, ?);", ("Bob", False))
conn.commit()

# Query results
cur.execute("SELECT id, name, active FROM users;")
print("Columns:", [col[0] for col in cur.description])
for row in cur.fetchall():
    print(row)

cur.close()
conn.close()
```

### Node.js Driver (`bindings/node/opendata.js`)
Zero native build dependencies (uses asynchronous child process execution with JSON framing):

```javascript
const opendata = require('./bindings/node/opendata.js');

async function main() {
    const client = opendata.connect({ database: 'app.pcod' });

    // Table-less query calculation
    const calc = await client.query('SELECT 1 + 1 AS sum, 10 * 10 AS mult;');
    console.log('Calculation:', calc.rows[0]);

    // Query tables with parameters
    const users = await client.query('SELECT * FROM users WHERE active = ?;', [true]);
    console.log('Active users:', users.rows);

    await client.close();
}

main().catch(console.error);
```

## 15. Troubleshooting

| Symptom | Likely cause | Fix |
| :--- | :--- | :--- |
| `ERROR: not connected to any database` | You launched the shell without a database file. | Type `CREATE DATABASE 'mydata.pcod';` or restart with `opendata mydata.pcod`. |
| My data is gone after I exit | You turned autocommit off and didn't `COMMIT;` before exiting. | Autocommit is ON by default. If you turned it off, always `COMMIT;` before `exit;`. See [§7](#7-data-manipulation--transaction-control). |
| `ERROR: database '...' already exists` | You tried `CREATE DATABASE` on an existing file. | Use `CONNECT 'mydata.pcod';` instead. |
| The shell shows `->` and won't run my command | Your statement looks incomplete (open quote/parenthesis, or it ends with a word like `FROM` or a comma). | Finish the statement, type `;` on its own line, or type `\c` to cancel the buffer. In the interactive shell, complete single-line statements run on Enter without a semicolon. |
| `ERROR: invalid or unsupported SELEC statement. Did you mean SELECT?` | A command was misspelled. | Use the suggested command; type `HELP;` for the full reference. |
| `ERROR: invalid or unsupported SELECT statement` | Syntax error in your query. | Check spelling of table/column names with `SHOW TABLES;` and `DESCRIBE tablename;`. |
| `ERROR: writes blocked by storage/backup failure` | A previous backup or disk write failed. | Run `STATUS;` to see details; check disk space and backup directory permissions. |
| `ERROR: cannot acquire database lock` | Another process has the database open. | Close the other shell or server, or use `CONNECT '...' READONLY;`. |
| `ERROR: read-only role cannot execute this statement` | Your account is `READ_ONLY`. | Ask an administrator to change your role, or use a different account. |
| `ERROR: permission denied: SELECT on <table>` | Your `READ_ONLY` account has no grant on that table. | Ask an administrator: `GRANT SELECT ON <table> TO '<you>';` then `COMMIT;`. |
| `ERROR: permission denied for this statement (check table grants...)` | A `READ_ONLY` network user tried a statement class they don't hold. | Check `SHOW GRANTS;` as an ADMIN; grant the needed privilege. |
| `ERROR: command is not permitted over the network` | You tried a local-only command (like `SOURCE` or `IRIS`) over a network connection. | Run the command from the local shell instead. |
| I forgot my password | Passwords are not recoverable. | An administrator can reset it: `ALTER USER 'name' PASSWORD 'new-password';` then `COMMIT;`. |

---

## 16. Command Reference Quick Sheet

| Command | Category | Description |
| :--- | :--- | :--- |
| `CREATE DATABASE '<path>'` | Lifecycle | Initialize a new empty database snapshot. |
| `CONNECT '<path>' [READONLY]` | Lifecycle | Mount an existing `.pcod`, `.mdf`, or `.dbf` database. |
| `DISCONNECT` | Lifecycle | Release locks and close active database instance. |
| `SHOW DATABASES` | Inspection | List all database files in the current folder. |
| `GUIDE` | Onboarding | Show an optional, task-based menu for common next steps. |
| `LOAD SAMPLE` | Onboarding | Create the `SAMPLE_DATA` practice table with five rows. |
| `CREATE WIZARD` | DDL | Build a table interactively with prompts — no SQL needed. |
| `SET SEMICOLON OPTIONAL ON\|OFF` | Shell | Run complete single-line statements on Enter without `;`. |
| `SET STATEMENT_TIMEOUT <ms>` | Watchdog | Set session query execution deadline watchdog in milliseconds (0 disables). |
| `SET AUTO_VACUUM ON\|OFF` | Maintenance | Toggle autonomous background vacuum compaction scheduler. |
| `SHOW TABLES` / `DESCRIBE <t>` | Inspection | Discover tables and inspect their columns and types. |
| `BROWSE <t>` | Inspection | Preview a table 20 rows at a time; interactive sessions page with Enter/q. |
| `STATUS` | Diagnostics | Display engine health, durable generation, and backup status. |
| `CONNECTION STATUS` | Operations | Show current session transport security and service/database health. |
| `SUPPORT REPORT TO '<file>'` | Diagnostics | Create a privacy-preserving support summary without overwriting a file. |
| `BEGIN / COMMIT / ROLLBACK` | Transactions | Explicit ACID transaction demarcation. |
| `SAVEPOINT <name>` | Transactions | Create named recovery savepoint. |
| `ROLLBACK TO <name>` | Transactions | Revert changes back to specific savepoint. |
| `CREATE TABLE ...` | DDL | Define tables with numbers, strings, dates, or vectors. |
| `ALTER TABLE ... ADD CONSTRAINT` | DDL | Dynamically attach PK, UNIQUE, or FK constraints. |
| `CREATE INDEX <name> ON <t>(<c>)` | DDL | Build an ordered secondary B+ Tree index. |
| `CREATE INDEX <name> ON <t> USING HNSW (<col>)` | Vector | Build an HNSW index graph for fast approximate nearest neighbor (ANN) search. |
| `ROW_NUMBER() / RANK() / DENSE_RANK() OVER (...)` | Query | SQL window analytical functions across partitions and sorted windows. |
| `col->'path' / col->>'path' / JSON_VALUE()` | JSON | Extract structured JSON elements or scalar string values. |
| `INSERT ... ON CONFLICT DO UPDATE`| DML | Atomic upsert with excluded expression updates. |
| `<=>` and `<->` | Vector | Cosine and Euclidean L2 distance operators. |
| `EXPORT MSSQL / ORACLE '<file>'` | Migration | Export tables directly into SQL Server or Oracle binary formats. |
| `IMPORT MSSQL / ORACLE '<file>'` | Migration | Ingest `.mdf` or `.dbf` files into active database. |
| `IMPORT WIZARD` | Import | Preview a CSV, review suggested types, and import safely into a new table. |
| `IMPORT '<file>' [INTO <t>]` | Import | Zero-prompt direct CSV/JSON file import with auto-table generation. |
| `CREATE TABLE ... AS SELECT` | DDL | Create table and populate directly from query (CTAS). |
| `INSERT INTO ... SELECT` | DML | Bulk insert rows returned from a query into target table. |
| `ALTER TABLE ... MODIFY [COLUMN]` | DDL | Alter existing column data type and nullability constraints. |
| `DRYRUN / TEST <statement>` | Safety | Simulate destructive statement and preview impact without modifying database file. |
| `SET CONFIRM ON\|OFF` | Safety | Toggle interactive confirmation and dry-run prompts for destructive commands. |
| `SET FORMAT BOX\|MARKDOWN\|VERTICAL` | Shell | Switch query result rendering between Box, Markdown, Vertical, and Table. |
| `\dt` / `.tables` | Shell | List user tables. |
| `\d [table]` / `.schema [table]` | Shell | Describe table schema. |
| `\l` | Shell | List database files in the current folder. |
| `\du` | Shell | List database users. |
| `\i <file>` / `.read <file>` | Shell | Execute SQL script file. |
| `\x` | Shell | Toggle expanded/vertical record card display mode. |
| `\q` / `.exit` | Shell | Exit interactive shell. |
| `opendata --dev` | Server | Start local development server on 127.0.0.1:7350 with default credentials. |
| `opendata --pitr <timestamp>` | Recovery | Perform continuous Point-In-Time Recovery to specific Unix epoch seconds. |
| `opendata --install-service [name]` | Service | Register PC/OpenData SQL Server as a Windows background service. |
| `opendata --uninstall-service [name]`| Service | Remove PC/OpenData Windows background service registration. |
| `opendata --start-service / --stop-service` | Service | Start or stop the registered Windows background service. |
| `opendata --ad-server ... --ad-domain ...` | Security | Start server with Active Directory UPN authentication & role mapping. |
| `opendata --ldap-server ... --ldap-base-dn ...` | Security | Start server with generic LDAP/LDAPS directory authentication. |
| `opendata --krb-realm ... --krb-service ...` | Security | Start server with Kerberos / SPNEGO GSSAPI ticket single sign-on. |
| `opendata --statement-timeout <ms>` | Watchdog | Configure global query execution deadline watchdog in milliseconds. |
| `opendata --auto-vacuum / --no-auto-vacuum` | Maintenance | Enable or disable periodic background vacuum sweep daemon. |
| `CHECK INTEGRITY` | Health | Validate foreign keys, uniqueness, and CRC32 sums. |
| `VACUUM` / `COMPACT` | Maintenance | Compact storage pages and defragment records. |
| `BENCHMARK <n> <query>` | Diagnostics | Run query $N$ times and report average latency and QPS. |
| `WATCH [INTERVAL <s>] <query>` | Diagnostics | Live poll query results on a scheduled interval. |
| `DIFF SCHEMA '<other_db>'` | Diagnostics | Compare active schema against another database snapshot. |
| `SHOW SESSIONS` | Operations | List active connections and executing queries. |
| `CREATE USER ... ROLE ...` | Security | Create an account with the `ADMIN` or `READ_ONLY` role. |
| `ALTER USER ...` / `DROP USER ...` | Security | Change password, role, enabled state, or delete an account. |
| `SHOW USERS` | Security | List usernames, roles, and enabled states (never password verifiers). |
| `GRANT ... ON <t> TO '<user>'` | Security | Grant per-table SELECT/INSERT/UPDATE/DELETE to a READ_ONLY user. |
| `REVOKE ON <t> FROM '<user>'` | Security | Remove all of a user's grants on a table. |
| `SHOW GRANTS` | Security | List per-table privilege grants (ADMIN only). |
| `VERIFY BACKUP '<file>'` | Operations | Offline restorability check of a backup snapshot. |
| `KILL SESSION <id>` | Operations | Forcibly disconnect an active client session. |
| `IRIS ['<file>']` | Editor | Open full-screen GNU Nano-style SQL editor. |
| `DUMP DATABASE TO '<file>'` | Export | Generate canonical SQL DDL and INSERT script. |
| `SHOW CLUSTER` | Replication | View 3-node Raft consensus quorum and term status. |

---

## 17. Core Architecture & Highlights

> **⏩ Skip if you're just getting started.** This section describes the engine's internals. You don't need to understand any of this to use PC/OpenData productively.

* **Single-Executable Zero-Dependency Footprint**: Distributed as a single self-contained binary (`opendata.exe` on Windows, `opendata` on Linux/Solaris) compiling cleanly with standard ISO C11.
* **Dual Persistence Layer**:
  * **Memory Slotted-Page Storage**: High-speed, lock-free query execution backed by a 64-frame Clock-Sweep page buffer pool.
  * **Durable Atomic Publishing**: Writes write-ahead snapshots using CRC32 integrity verification, atomic directory synchronization, and generational durability counters.
* **Zero Data Loss Quorum (3-Node Raft Consensus)**: Synchronous dual-write WAL replication guaranteeing survivability across node partitions.
* **Direct Binary Cross-Engine Compatibility**: Read, write, mount, and convert native PC/OpenData (`.pcod`), Microsoft SQL Server (`.mdf`, `.ndf`), and Oracle Database (`.dbf`, `.ora`) files with native CRC verification.
* **Native Vector(dim) Embeddings**: Native float32 vector columns (up to 64 dimensions) with 4-way SIMD-accelerated Cosine distance (`<=>`) and Euclidean L2 distance (`<->`).
* **Multi-Protocol Network Reactor**: High-throughput non-blocking event reactor supporting native TCP, encrypted TLS (Schannel/OpenSSL), and a PostgreSQL v3 wire protocol compatibility front-end.
* **Database-wide User Roles**: Durable per-user password verifiers, individually authenticated connections, and `ADMIN`/`READ_ONLY` roles.

---

*PC/OpenData SQL Server is developed and maintained by PC/OpenSystems LLC. Released under the BSD-3-Clause License.*
