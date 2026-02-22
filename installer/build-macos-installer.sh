#!/bin/bash
# Build self-extracting macOS installer (compiles from source on target)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="0.2.0"
DIST="$ROOT/dist"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

mkdir -p "$DIST"

echo "Building macOS installer payload..."

# Create source payload (same as source tarball)
PAYLOAD_DIR="$TMPDIR/OpenVISA"
mkdir -p "$PAYLOAD_DIR"
rsync -a --exclude='build*' --exclude='dist' --exclude='.git' --exclude='*.o' --exclude='*.dll' --exclude='*.exe' --exclude='*.so*' --exclude='*.a' "$ROOT/" "$PAYLOAD_DIR/"

cd "$TMPDIR"
tar czf payload.tar.gz "OpenVISA/"
PAYLOAD_B64=$(base64 -w0 payload.tar.gz)

cat > "$DIST/OpenVISA-${VERSION}-macos-installer.sh" << 'HEADER'
#!/bin/bash
# OpenVISA v0.2.0 — macOS Installer
# Compiles from source and installs to /usr/local
set -e

echo "╔══════════════════════════════════════════╗"
echo "║       OpenVISA v0.2.0 — Installer        ║"
echo "║   Vendor-free VISA implementation         ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# Check dependencies
if ! command -v cmake &>/dev/null; then
    echo "❌ cmake not found. Install with: brew install cmake"
    exit 1
fi

if ! command -v cc &>/dev/null; then
    echo "❌ C compiler not found. Install Xcode Command Line Tools:"
    echo "   xcode-select --install"
    exit 1
fi

PREFIX="${PREFIX:-/usr/local}"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "📦 Extracting source..."
SOURCE_START=$(awk '/^__SOURCE__$/{print NR + 1; exit 0;}' "$0")
tail -n +$SOURCE_START "$0" | base64 -d | tar xzf - -C "$TMPDIR"

cd "$TMPDIR/OpenVISA"

echo "🔨 Building..."
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DOPENVISA_WITH_USB=ON 2>&1 | grep -E "^--|libusb"

cmake --build build --config Release -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4) 2>&1 | tail -5

echo ""
echo "🧪 Running tests..."
cd build && ./test_parser && cd ..

echo ""
echo "📂 Installing to $PREFIX (may need sudo)..."

if [ -w "$PREFIX/lib" ] 2>/dev/null; then
    cmake --install build 2>&1 | tail -5
else
    sudo cmake --install build 2>&1 | tail -5
fi

# Create openvisa-idn command
if [ -f "build/example_idn" ]; then
    INSTALL_CMD=""
    [ ! -w "$PREFIX/bin" ] && INSTALL_CMD="sudo"
    $INSTALL_CMD cp build/example_idn "$PREFIX/bin/openvisa-idn"
    $INSTALL_CMD chmod +x "$PREFIX/bin/openvisa-idn"
fi

echo ""
echo "✅ OpenVISA v0.2.0 installed!"
echo ""
echo "   Libraries: $PREFIX/lib/libvisa.dylib"
echo "   Headers:   $PREFIX/include/visa.h"
echo ""
echo "   Usage:"
echo "   #include <visa.h>"
echo "   cc -o myapp myapp.c -lvisa"
echo ""
echo "   Example:"
echo "   openvisa-idn TCPIP::192.168.1.50::5025::SOCKET"
echo ""
exit 0

__SOURCE__
HEADER

echo "$PAYLOAD_B64" >> "$DIST/OpenVISA-${VERSION}-macos-installer.sh"
chmod +x "$DIST/OpenVISA-${VERSION}-macos-installer.sh"

echo "Done: $(ls -lh "$DIST/OpenVISA-${VERSION}-macos-installer.sh")"
