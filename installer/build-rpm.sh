#!/bin/bash
# Build .rpm package (using fpm or alien as fallback)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="0.2.0"
DIST="$ROOT/dist"
BUILD="$ROOT/build"

mkdir -p "$DIST"

echo "Building RPM package..."

# If we have the .deb, convert via alien
DEB="$DIST/openvisa_${VERSION}_amd64.deb"
if [ ! -f "$DEB" ]; then
    echo "Building DEB first..."
    bash "$(dirname "$0")/build-deb.sh"
fi

if command -v alien &>/dev/null; then
    cd "$DIST"
    sudo alien --to-rpm --scripts "$DEB" 2>&1 | tail -3
    # alien produces openvisa-VERSION*.rpm, rename to standard
    mv -f openvisa-*.rpm "openvisa-${VERSION}-1.x86_64.rpm" 2>/dev/null || true
    echo "Done: $(ls -lh "$DIST/openvisa-${VERSION}-1.x86_64.rpm")"
elif command -v rpmbuild &>/dev/null; then
    echo "TODO: native rpmbuild spec"
    echo "For now: install alien (apt install alien) and re-run"
    exit 1
else
    echo "Neither alien nor rpmbuild found. Install alien:"
    echo "  sudo apt install alien"
    exit 1
fi
