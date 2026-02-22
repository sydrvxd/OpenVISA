#!/bin/bash
# OpenVISA Release Build Script
# Cross-compiles for Windows (32+64 bit), builds Linux, packages everything
set -e

VERSION="0.2.0"
ROOT="$(cd "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"

echo "=== OpenVISA v${VERSION} Release Build ==="
mkdir -p "$DIST"

# --- Windows 64-bit ---
echo ""
echo "[1/5] Building Windows 64-bit (visa64.dll)..."
mkdir -p "$ROOT/build-win64"
cd "$ROOT/build-win64"
cmake "$ROOT" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/win64-toolchain.cmake" -DOPENVISA_WITH_USB=OFF -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -2
make -j$(nproc) 2>&1 | tail -3
echo "  → visa64.dll ($(stat -c%s visa64.dll) bytes)"

# --- Windows 32-bit ---
echo ""
echo "[2/5] Building Windows 32-bit (visa32.dll)..."
mkdir -p "$ROOT/build-win32"
cd "$ROOT/build-win32"
cmake "$ROOT" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/win32-toolchain.cmake" -DOPENVISA_WITH_USB=OFF -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -2
make -j$(nproc) 2>&1 | tail -3
echo "  → visa32.dll ($(stat -c%s visa32.dll) bytes)"

# --- Linux ---
echo ""
echo "[3/5] Building Linux (libvisa.so)..."
mkdir -p "$ROOT/build"
cd "$ROOT/build"
cmake "$ROOT" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -2
make -j$(nproc) 2>&1 | tail -3
echo "  → libvisa.so ($(stat -c%s libvisa.so.${VERSION}) bytes)"

# --- Run tests ---
echo ""
echo "[4/5] Running tests..."
cd "$ROOT/build"
./test_parser

# --- Portable Windows ZIP ---
echo ""
echo "[5/5] Packaging..."

# Windows portable ZIP
ZIP_NAME="OpenVISA-${VERSION}-win64.zip"
STAGING="$DIST/staging-win"
rm -rf "$STAGING"
mkdir -p "$STAGING/OpenVISA/bin" "$STAGING/OpenVISA/include" "$STAGING/OpenVISA/lib" "$STAGING/OpenVISA/examples"
cp "$ROOT/build-win64/visa64.dll" "$STAGING/OpenVISA/bin/"
cp "$ROOT/build-win32/visa32.dll" "$STAGING/OpenVISA/bin/"
cp "$ROOT/build-win64/libvisa64.dll.a" "$STAGING/OpenVISA/lib/"
cp "$ROOT/build-win32/libvisa32.dll.a" "$STAGING/OpenVISA/lib/"
cp "$ROOT/include/visa.h" "$ROOT/include/visatype.h" "$STAGING/OpenVISA/include/"
cp "$ROOT/examples/idn_query.c" "$ROOT/build-win64/example_idn.exe" "$STAGING/OpenVISA/examples/"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$STAGING/OpenVISA/"
cd "$STAGING"
zip -r "$DIST/$ZIP_NAME" OpenVISA/
echo "  → $ZIP_NAME ($(stat -c%s "$DIST/$ZIP_NAME") bytes)"

# Linux tarball
TAR_NAME="OpenVISA-${VERSION}-linux-x64.tar.gz"
STAGING_LIN="$DIST/staging-linux"
rm -rf "$STAGING_LIN"
mkdir -p "$STAGING_LIN/openvisa/lib" "$STAGING_LIN/openvisa/include" "$STAGING_LIN/openvisa/bin"
cp "$ROOT/build/libvisa.so.${VERSION}" "$STAGING_LIN/openvisa/lib/"
cd "$STAGING_LIN/openvisa/lib" && ln -sf "libvisa.so.${VERSION}" libvisa.so.0 && ln -sf libvisa.so.0 libvisa.so && cd -
cp "$ROOT/build/libvisa_static.a" "$STAGING_LIN/openvisa/lib/"
cp "$ROOT/include/visa.h" "$ROOT/include/visatype.h" "$STAGING_LIN/openvisa/include/"
cp "$ROOT/build/example_idn" "$STAGING_LIN/openvisa/bin/"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$STAGING_LIN/openvisa/"
cd "$STAGING_LIN"
tar czf "$DIST/$TAR_NAME" openvisa/
echo "  → $TAR_NAME ($(stat -c%s "$DIST/$TAR_NAME") bytes)"

# NSIS installer (if makensis available)
if command -v makensis &>/dev/null; then
    echo ""
    echo "  Building NSIS installer..."
    cd "$ROOT/installer"
    makensis openvisa.nsi 2>&1 | tail -5
    if [ -f "$DIST/OpenVISA-${VERSION}-setup-win64.exe" ]; then
        echo "  → OpenVISA-${VERSION}-setup-win64.exe ($(stat -c%s "$DIST/OpenVISA-${VERSION}-setup-win64.exe") bytes)"
    fi
else
    echo "  ⚠ makensis not found — skipping installer"
fi

# Cleanup staging
rm -rf "$DIST/staging-win" "$DIST/staging-linux"

echo ""
echo "=== Done! Release artifacts in: $DIST ==="
ls -lh "$DIST/"
