#!/usr/bin/env bash
# ==============================================================================
# PC/OpenData - SSH Remote Execution and Secure Tunnel Helper
# Executes remote SQL commands or creates an encrypted SSH tunnel to TCP/TCPS.
# ==============================================================================

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage:"
    echo "  Execute remote query:  $0 <user@host> --query \"<SQL>\" [--format table|csv|json]"
    echo "  Establish tunnel:      $0 <user@host> --tunnel [local_port] [remote_port]"
    exit 1
fi

quote_posix() {
    local value="$1"
    local quoted="'"
    local i char
    for ((i = 0; i < ${#value}; i++)); do
        char="${value:i:1}"
        if [ "$char" = "'" ]; then
            quoted+="'\\''"
        else
            quoted+="$char"
        fi
    done
    printf "%s'" "$quoted"
}

HOST="$1"
shift

# Reject host names that could be interpreted as ssh options or contain invalid characters.
case "$HOST" in
    -*) echo "Invalid host: must not start with '-'." >&2; exit 1 ;;
esac
if [[ ! "$HOST" =~ ^[a-zA-Z0-9_.-]+(@[a-zA-Z0-9_.-]+)?(:[0-9]+)?$ ]]; then
    echo "Invalid host name format: $HOST" >&2
    exit 1
fi

MODE="$1"
shift

if [ "$MODE" = "--tunnel" ]; then
    LOCAL_PORT="${1:-7337}"
    REMOTE_PORT="${2:-7337}"
    if [[ ! "$LOCAL_PORT" =~ ^[0-9]+$ ]] || [ "$LOCAL_PORT" -lt 1 ] || [ "$LOCAL_PORT" -gt 65535 ]; then
        echo "Invalid local port: $LOCAL_PORT" >&2
        exit 1
    fi
    if [[ ! "$REMOTE_PORT" =~ ^[0-9]+$ ]] || [ "$REMOTE_PORT" -lt 1 ] || [ "$REMOTE_PORT" -gt 65535 ]; then
        echo "Invalid remote port: $REMOTE_PORT" >&2
        exit 1
    fi
    echo "Establishing SSH tunnel to PC/OpenData on $HOST ($LOCAL_PORT -> localhost:$REMOTE_PORT)..."
    ssh -N -L "$LOCAL_PORT:127.0.0.1:$REMOTE_PORT" -- "$HOST"
elif [ "$MODE" = "--query" ]; then
    if [ "$#" -lt 1 ]; then
        echo "Missing SQL query." >&2
        exit 1
    fi
    SQL="$1"
    FORMAT="${2:-json}"
    case "$FORMAT" in
        table|csv|json) ;;
        *)
            echo "Invalid format: $FORMAT (expected table, csv, or json)." >&2
            exit 1
            ;;
    esac
    SQL_ARGUMENT=$(quote_posix "$SQL")
    ssh -- "$HOST" "opendata --database /data/production.pcod --format $FORMAT --execute $SQL_ARGUMENT"
else
    echo "Unknown option: $MODE"
    exit 1
fi
