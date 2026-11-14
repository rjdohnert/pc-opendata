# PC/OpenData Container Image

Lightweight (~12 MB uncompressed, ~4.7 MB compressed), production-hardened container for the PC/OpenData SQL Engine built on Alpine Linux 3.19.

## Contents
- **`pc-opendata-27.0.tar.gz`**: Pre-built container image archive for version 27.0.
- **`pc-opendata-latest.tar.gz`**: Pre-built container image archive tagged as `latest`.
- **`Dockerfile`**: Multi-stage C11 Alpine build definition with TLS and OpenLDAP client libraries.
- **`docker-compose.yml`**: Turnkey Docker Compose orchestration specification.

---

## 🚀 Quick Start (No Build Required)

You do not need to compile or build the container image. Load the pre-packaged archive directly into Docker or Podman:

### 1. Load the Container Image

```bash
# Using Docker
docker load -i docker/pc-opendata-27.0.tar.gz

# Using Podman (RHEL / CentOS Stream / Fedora / WSL)
podman load -i docker/pc-opendata-27.0.tar.gz
```

Verify that the image is loaded:
```bash
docker images pc-opendata
# REPOSITORY    TAG       IMAGE ID       SIZE
# pc-opendata   27.0      788d24623c32   12.3MB
# pc-opendata   latest    788d24623c32   12.3MB
```

---

## 💻 Common Usage Scenarios

### 1. Run the Interactive SQL Console
Launch an interactive database shell session against a local database file:
```bash
docker run -it --rm \
  -v opendata_data:/data \
  pc-opendata:latest /data/production.pcod
```

### 2. Execute SQL Statements Directly
Run one-off queries or schema migrations from scripts without starting a long-running daemon:
```bash
# Run a one-off query
docker run --rm pc-opendata:latest --execute "SELECT 1 + 1 AS test;"

# Run multiple semicolon-delimited statements
docker run --rm pc-opendata:latest --execute "CREATE TABLE test(id INT, name TEXT); INSERT INTO test VALUES (1, 'Alice'); SELECT * FROM test;"
```

### 3. Run the Built-in Self-Test Suite
Verify system integrity, vector indexing, transaction isolation, and encryption inside the container:
```bash
docker run --rm pc-opendata:latest --execute "SELF-TEST;"
```

### 4. Run the Authenticated TLS Server Daemon
Run as a background database service listening on port 7338 with persistent data and backup volumes:
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

### 5. Orchestrate with Docker Compose
```bash
# Start the background server
docker compose -f docker/docker-compose.yml up -d

# View real-time logs
docker compose -f docker/docker-compose.yml logs -f

# Stop the container
docker compose -f docker/docker-compose.yml down
```

---

## 🛠 Building From Source

If you prefer building the image locally from source:

```bash
# From the repository root:
docker build -t pc-opendata:27.0 -f docker/Dockerfile .

# Or using Podman:
podman build -t pc-opendata:27.0 -f docker/Dockerfile .
```

---

## ⚙️ Container Configuration & Volume Reference

| Parameter / Path | Default | Description |
|---|---|---|
| `7338` | Port | Authenticated TLS server port (Native TLS & PostgreSQL wire protocol) |
| `/data` | Volume | Persistent database root directory storing `.pcod` database files and WAL journals |
| `/backups` | Volume | Automatic atomic snapshot backup destination directory |
| `/run/tls` | Mount | Directory containing `server.pem`, `server-key.pem`, and `ca.pem` |
| `OPENDATA_SERVER_USER` | Environment | Bootstrap administrative username created upon initial start |
| `OPENDATA_SERVER_PASSWORD` | Environment | Bootstrap admin password (minimum 16 characters) |
| `OPENDATA_TLS_SERVER_NAME` | `localhost` | Hostname used by internal TLS healthcheck probe |
| `OPENDATA_TLS_CA` | `/run/tls/ca.pem` | Certificate Authority file for TLS healthcheck verification |
