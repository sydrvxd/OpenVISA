#!/bin/bash
# Build self-extracting Linux installer
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="0.2.0"
DIST="$ROOT/dist"
BUILD="$ROOT/build"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

mkdir -p "$DIST"

echo "Building Linux installer payload..."

# Create payload tarball
PAYLOAD_DIR="$TMPDIR/OpenVISA-${VERSION}-linux-x64"
mkdir -p "$PAYLOAD_DIR/lib" "$PAYLOAD_DIR/include" "$PAYLOAD_DIR/bin"

cp "$BUILD/libvisa.so.${VERSION}" "$PAYLOAD_DIR/lib/"
cd "$PAYLOAD_DIR/lib" && ln -sf "libvisa.so.${VERSION}" libvisa.so.0 && ln -sf libvisa.so.0 libvisa.so && cd -
cp "$BUILD/libvisa_static.a" "$PAYLOAD_DIR/lib/"
cp "$ROOT/include/visa.h" "$ROOT/include/visatype.h" "$PAYLOAD_DIR/include/"
cp "$BUILD/example_idn" "$PAYLOAD_DIR/bin/openvisa-idn"
chmod +x "$PAYLOAD_DIR/bin/openvisa-idn"

cd "$TMPDIR"
tar czf payload.tar.gz "OpenVISA-${VERSION}-linux-x64/"
PAYLOAD_B64=$(base64 -w0 payload.tar.gz)

# Build installer script
cat > "$DIST/OpenVISA-${VERSION}-linux-installer.sh" << 'HEADER'
#!/bin/bash
# OpenVISA v0.2.0 — Linux Installer
set -e

echo "╔══════════════════════════════════════════╗"
echo "║       OpenVISA v0.2.0 — Installer        ║"
echo "║   Vendor-free VISA implementation         ║"
echo "╚══════════════════════════════════════════╝"
echo ""

PREFIX="${PREFIX:-/usr/local}"

echo "📂 Installing to $PREFIX..."

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_START=$(awk '/^__SOURCE__$/{print NR + 1; exit 0;}' "$0")
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

tail -n +$SOURCE_START "$0" | base64 -d | tar xzf - -C "$TMPDIR"

INSTALL_CMD=""
if [ "$(id -u)" != "0" ]; then
    if command -v sudo &>/dev/null; then
        INSTALL_CMD="sudo"
    else
        echo "⚠️  Not running as root. Install may fail."
    fi
fi

$INSTALL_CMD mkdir -p "$PREFIX/lib" "$PREFIX/include" "$PREFIX/bin"

$INSTALL_CMD cp "$TMPDIR/OpenVISA-0.2.0-linux-x64/lib/libvisa.so.0.2.0" "$PREFIX/lib/"
$INSTALL_CMD ln -sf libvisa.so.0.2.0 "$PREFIX/lib/libvisa.so.0"
$INSTALL_CMD ln -sf libvisa.so.0 "$PREFIX/lib/libvisa.so"
$INSTALL_CMD cp "$TMPDIR/OpenVISA-0.2.0-linux-x64/lib/libvisa_static.a" "$PREFIX/lib/"
$INSTALL_CMD cp "$TMPDIR/OpenVISA-0.2.0-linux-x64/include/visa.h" "$PREFIX/include/"
$INSTALL_CMD cp "$TMPDIR/OpenVISA-0.2.0-linux-x64/include/visatype.h" "$PREFIX/include/"
$INSTALL_CMD cp "$TMPDIR/OpenVISA-0.2.0-linux-x64/bin/openvisa-idn" "$PREFIX/bin/openvisa-idn"
$INSTALL_CMD chmod +x "$PREFIX/bin/openvisa-idn"

# Update ldconfig
if [ -d /etc/ld.so.conf.d ]; then
    echo "$PREFIX/lib" | $INSTALL_CMD tee /etc/ld.so.conf.d/openvisa.conf > /dev/null
    $INSTALL_CMD ldconfig 2>/dev/null || true
fi

echo ""
echo "✅ OpenVISA v0.2.0 installed!"
echo ""
echo "   Libraries: $PREFIX/lib/libvisa.so"
echo "   Headers:   $PREFIX/include/visa.h"
echo ""
echo "   cc -o myapp myapp.c -lvisa"
echo "   openvisa-idn TCPIP::192.168.1.50::5025::SOCKET"
echo ""
exit 0

__SOURCE__
HEADER

echo "$PAYLOAD_B64" >> "$DIST/OpenVISA-${VERSION}-linux-installer.sh"
chmod +x "$DIST/OpenVISA-${VERSION}-linux-installer.sh"

echo "Done: $(ls -lh "$DIST/OpenVISA-${VERSION}-linux-installer.sh")"
