#!/usr/bin/env bash
# ==============================================================================
# PC/OpenData - WSL Service & Host Port Forwarding Bridge
# Manages headless database daemon inside WSL and binds to Windows localhost.
# ==============================================================================

set -euo pipefail

# Default to per-user paths instead of predictable world-writable /tmp names.
DB_PATH="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/opendata/wsl_opendata.pcod}"
BACKUP_DIR="${2:-${XDG_DATA_HOME:-$HOME/.local/share}/opendata/wsl_backups}"
PORT="${3:-7337}"

if [[ ! "$PORT" =~ ^[0-9]+$ ]] || [ "$PORT" -lt 1 ] || [ "$PORT" -gt 65535 ]; then
    echo "Invalid port: $PORT" >&2
    exit 1
fi

mkdir -p "$(dirname "$DB_PATH")" "$BACKUP_DIR"
chmod 700 "$(dirname "$DB_PATH")" "$BACKUP_DIR" 2>/dev/null || true

BINARY="./bin/opendata"
if [ ! -f "$BINARY" ]; then
    echo "Binary not found. Running wsl-build.sh..."
    ./wsl-build.sh
fi

echo "=== Starting PC/OpenData Headless Server in WSL ==="
echo "Database:   $DB_PATH"
echo "Backups:    $BACKUP_DIR"
echo "Port:       $PORT"

WSL_IP=$(hostname -I | awk '{print $1}')
echo "WSL Internal IP: $WSL_IP"

# If powershell.exe is available (interop enabled), display Windows bridge command
if command -v powershell.exe >/dev/null 2>&1; then
    echo ""
    echo "To access this WSL server from Windows Host applications, run as Admin in PowerShell:"
    echo "  netsh interface portproxy add v4tov4 listenport=$PORT listenaddress=0.0.0.0 connectport=$PORT connectaddress=$WSL_IP"
    echo ""
fi

exec "$BINARY" --database "$DB_PATH" --backup-dir "$BACKUP_DIR" --backup-interval 60 --server --port "$PORT"
