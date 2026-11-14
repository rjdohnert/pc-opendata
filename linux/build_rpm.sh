#!/usr/bin/env bash
# ==============================================================================
# Build Linux Binary and RPM Package for PC/OpenData
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

echo "=== [1/4] Building release binary for Linux (x86_64) ==="
OPENDATA_TLS=1 bash build-scripts/wsl-build.sh

echo "=== [2/4] Verifying binary self-test ==="
"$REPO_ROOT/bin/opendata" self-test

# Copy binary to linux folder
cp -f "$REPO_ROOT/bin/opendata" "$REPO_ROOT/linux/opendata"
chmod 0755 "$REPO_ROOT/linux/opendata"

echo "=== [3/4] Packaging RPM via rpmbuild ==="
RPM_WORK="/tmp/pc-opendata-rpm-build"
rm -rf "$RPM_WORK"
mkdir -p "$RPM_WORK"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Prepare sources
cp -f "$REPO_ROOT/linux/opendata" "$RPM_WORK/SOURCES/opendata"
cp -f "$REPO_ROOT/opendata.service" "$RPM_WORK/SOURCES/opendata.service"
cp -f "$REPO_ROOT/README.md" "$RPM_WORK/SOURCES/README.md"
cp -f "$REPO_ROOT/LICENSE" "$RPM_WORK/SOURCES/LICENSE"
cp -f "$REPO_ROOT/linux/opendata.spec" "$RPM_WORK/SPECS/opendata.spec"

# Build binary RPM
rpmbuild --define "_topdir $RPM_WORK" -bb "$RPM_WORK/SPECS/opendata.spec"

# Copy resulting RPM(s) into linux/ folder
find "$RPM_WORK/RPMS" -type f -name "*.rpm" -exec cp -vf {} "$REPO_ROOT/linux/" \;

echo "=== [4/4] Validating generated artifacts in linux/ ==="
ls -lh "$REPO_ROOT/linux"

for rpm_pkg in "$REPO_ROOT"/linux/*.rpm; do
    echo "--- Package info: $rpm_pkg ---"
    rpm -qip "$rpm_pkg"
    echo "--- Package contents: $rpm_pkg ---"
    rpm -qlp "$rpm_pkg"
done

echo "=== RPM build complete! ==="
