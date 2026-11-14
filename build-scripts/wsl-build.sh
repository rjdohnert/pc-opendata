#!/usr/bin/env bash
# ==============================================================================
# PC/OpenData - Native WSL and Linux Build Script
# Compiles modular and amalgamated builds; OPENDATA_TLS=1 enables OpenSSL.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

COMPILER="${CC:-clang}"
if ! command -v "$COMPILER" >/dev/null 2>&1; then
    if command -v gcc >/dev/null 2>&1; then
        COMPILER="gcc"
    else
        echo "ERROR: Neither clang nor gcc found in WSL environment." >&2
        exit 1
    fi
fi

mkdir -p bin

CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror -O2 -g -pthread"
LIBS="-lcrypto -lldap -llber -lm"
if [[ "${OPENDATA_TLS:-0}" == "1" ]]; then
    CFLAGS="$CFLAGS -DOPENDATA_TLS"
    LIBS="-lssl $LIBS"
fi

echo "=== Building PC/OpenData in WSL ($COMPILER) ==="

# Read modules from src/modules.txt
MODULES=()
while IFS= read -r line || [[ -n "$line" ]]; do
    line="$(echo "$line" | tr -d '\r')"
    [[ -z "$line" ]] && continue
    MODULES+=("src/$line")
done < src/modules.txt

# Modular build
echo "-> Compiling ${#MODULES[@]} separate translation units..."
"$COMPILER" $CFLAGS "${MODULES[@]}" $LIBS -o bin/opendata

echo "-> Modular build successful: bin/opendata"

# Check if amalgamation matches
if [ -f src/opendata.c ]; then
    echo "-> Compiling single-file amalgamation..."
    "$COMPILER" $CFLAGS src/opendata.c $LIBS -o bin/opendata_amalgamated
    echo "-> Amalgamation build successful: bin/opendata_amalgamated"
fi

echo "==================================================================="
echo "   WSL Build Complete!                                           "
echo "   Run: ./bin/opendata --help                                      "
echo "==================================================================="
