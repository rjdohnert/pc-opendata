#!/bin/sh
# ==============================================================================
# PC/OpenData SQL Server - Unified POSIX Build Script
# Supports Linux (RHEL, Debian, Ubuntu, Rocky, Alma, Fedora, Azure Linux),
# FreeBSD (13/14/15), and Oracle Solaris / SunOS.
# ==============================================================================

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

# Defaults
COMPILER="${CC:-}"
CONFIGURATION="Release"
AMALGAMATION=0
ENABLE_TLS=1
SKIP_TESTS=0

usage() {
    cat <<EOF
Usage: ./build.sh [OPTIONS]

Options:
  -c, --compiler <cc>     C compiler to use (clang, gcc, cc). Default: auto-detect
  -a, --amalgamation      Build from single-file amalgamation (src/opendata.c)
  -d, --debug             Build with debug symbols and no optimization (-O0 -g)
  -r, --release           Build optimized release binary (-O2 -g) [default]
      --no-tls            Disable OpenSSL TLS encryption
      --skip-tests        Skip automatic built-in self-test verification
  -h, --help              Show this help message and exit

Environment Variables:
  CC                      C compiler path or name
  CFLAGS                  Additional compiler flags
  LDFLAGS                 Additional linker flags
  OPENDATA_TLS            Set to 0 to disable TLS (same as --no-tls)

Supported Platforms:
  - Linux (Enterprise Linux 9/10, Ubuntu, Debian, Fedora, Azure Linux)
  - FreeBSD 13, 14, 15
  - Oracle Solaris / SunOS
EOF
    exit 0
}

# Parse command line options
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--compiler)
            COMPILER="$2"
            shift 2
            ;;
        --compiler=*)
            COMPILER="${1#*=}"
            shift
            ;;
        -a|--amalgamation)
            AMALGAMATION=1
            shift
            ;;
        -d|--debug)
            CONFIGURATION="Debug"
            shift
            ;;
        -r|--release)
            CONFIGURATION="Release"
            shift
            ;;
        --no-tls)
            ENABLE_TLS=0
            shift
            ;;
        --skip-tests)
            SKIP_TESTS=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "ERROR: Unknown option: $1" >&2
            echo "Run './build.sh --help' for usage." >&2
            exit 1
            ;;
    esac
done

if [ "${OPENDATA_TLS:-1}" = "0" ]; then
    ENABLE_TLS=0
fi

# Detect platform
OS_NAME=$(uname -s 2>/dev/null || echo "Unknown")

# Compiler selection
if [ -z "$COMPILER" ]; then
    for cand in clang gcc cc; do
        if command -v "$cand" >/dev/null 2>&1; then
            COMPILER="$cand"
            break
        fi
    done
fi

if [ -z "$COMPILER" ] || ! command -v "$COMPILER" >/dev/null 2>&1; then
    echo "ERROR: No C11 compiler found. Please install Clang or GCC." >&2
    exit 1
fi

# Base compiler flags
BASE_CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror -pthread"
if [ "$CONFIGURATION" = "Release" ]; then
    OPT_CFLAGS="-O2 -g"
else
    OPT_CFLAGS="-O0 -g"
fi

INC_FLAGS="-Iinclude"
LIB_FLAGS="-lm"

# Platform-specific adjustments
case "$OS_NAME" in
    FreeBSD)
        # FreeBSD ports/packages install headers/libs in /usr/local
        INC_FLAGS="$INC_FLAGS -I/usr/local/include"
        LIB_FLAGS="-L/usr/local/lib $LIB_FLAGS"
        ;;
    SunOS)
        # Solaris socket and network libraries
        LIB_FLAGS="$LIB_FLAGS -lsocket -lnsl"
        ;;
esac

# TLS and Crypto configuration
if [ "$ENABLE_TLS" -eq 1 ]; then
    BASE_CFLAGS="$BASE_CFLAGS -DOPENDATA_TLS"
    LIB_FLAGS="-lcrypto -lssl $LIB_FLAGS"
else
    LIB_FLAGS="-lcrypto $LIB_FLAGS"
fi

# LDAP libraries (for directory authentication)
LIB_FLAGS="-lldap -llber $LIB_FLAGS"

# Output directory and binary name
mkdir -p bin
TARGET_BIN="bin/opendata"
if [ "$AMALGAMATION" -eq 1 ]; then
    TARGET_BIN="bin/opendata_amalgamated"
fi

echo "================================================================================"
echo " Building PC/OpenData SQL Server v27.0"
echo " Platform:      $OS_NAME"
echo " Compiler:      $COMPILER ($($COMPILER --version 2>/dev/null | head -n 1 || echo 'C11'))"
echo " Configuration: $CONFIGURATION"
echo " TLS Support:   $([ "$ENABLE_TLS" -eq 1 ] && echo 'Enabled (OpenSSL)' || echo 'Disabled')"
echo " Target Layout: $([ "$AMALGAMATION" -eq 1 ] && echo 'Single-file Amalgamation' || echo 'Modular (38 translation units)')"
echo " Output Binary: $TARGET_BIN"
echo "================================================================================"

# Gather source files
if [ "$AMALGAMATION" -eq 1 ]; then
    if [ ! -f "src/opendata.c" ]; then
        echo "ERROR: Amalgamated source src/opendata.c not found." >&2
        exit 1
    fi
    SOURCES="src/opendata.c"
else
    if [ ! -f "src/modules.txt" ]; then
        echo "ERROR: Module manifest src/modules.txt not found." >&2
        exit 1
    fi
    SOURCES=""
    while IFS= read -r line || [ -n "$line" ]; do
        # Strip carriage returns and whitespace
        clean_line=$(echo "$line" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        [ -z "$clean_line" ] && continue
        [ ! -f "src/$clean_line" ] && { echo "ERROR: Missing module source: src/$clean_line" >&2; exit 1; }
        SOURCES="$SOURCES src/$clean_line"
    done < src/modules.txt
fi

# Compilation
# Note: word-splitting on SOURCES, CFLAGS, LDFLAGS is deliberate for flags & files
# shellcheck disable=SC2086
$COMPILER $BASE_CFLAGS $OPT_CFLAGS $INC_FLAGS ${CFLAGS:-} $SOURCES $LIB_FLAGS ${LDFLAGS:-} -o "$TARGET_BIN"

echo "Build complete: $TARGET_BIN"

# Run built-in self-tests
if [ "$SKIP_TESTS" -eq 0 ]; then
    echo "Running built-in self-test verification..."
    if ! "$TARGET_BIN" self-test; then
        echo "ERROR: Built-in self-test verification failed!" >&2
        exit 1
    fi
fi

echo "================================================================================"
echo " PC/OpenData SQL Server built and verified successfully!"
echo " Binary: $TARGET_BIN"
echo " Run:    $TARGET_BIN --help"
echo "================================================================================"
