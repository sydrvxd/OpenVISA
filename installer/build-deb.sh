#!/bin/bash
# Build .deb package
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="0.2.0"
DIST="$ROOT/dist"
BUILD="$ROOT/build"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

mkdir -p "$DIST"

echo "Building DEB package..."

DEB_ROOT="$TMPDIR/openvisa_${VERSION}_amd64"
mkdir -p "$DEB_ROOT/DEBIAN"
mkdir -p "$DEB_ROOT/usr/lib/x86_64-linux-gnu"
mkdir -p "$DEB_ROOT/usr/include"
mkdir -p "$DEB_ROOT/usr/bin"
mkdir -p "$DEB_ROOT/etc/ld.so.conf.d"

# Libraries
cp "$BUILD/libvisa.so.${VERSION}" "$DEB_ROOT/usr/lib/x86_64-linux-gnu/"
cd "$DEB_ROOT/usr/lib/x86_64-linux-gnu"
ln -sf "libvisa.so.${VERSION}" libvisa.so.0
ln -sf libvisa.so.0 libvisa.so
cd -
cp "$BUILD/libvisa_static.a" "$DEB_ROOT/usr/lib/x86_64-linux-gnu/"

# Headers
cp "$ROOT/include/visa.h" "$ROOT/include/visatype.h" "$DEB_ROOT/usr/include/"

# Binary
cp "$BUILD/example_idn" "$DEB_ROOT/usr/bin/openvisa-idn"
chmod +x "$DEB_ROOT/usr/bin/openvisa-idn"

# ldconfig
echo "/usr/lib/x86_64-linux-gnu" > "$DEB_ROOT/etc/ld.so.conf.d/openvisa.conf"

# Control file
INSTALLED_SIZE=$(du -sk "$DEB_ROOT" | awk '{print $1}')
cat > "$DEB_ROOT/DEBIAN/control" << EOF
Package: openvisa
Version: ${VERSION}
Section: libs
Priority: optional
Architecture: amd64
Installed-Size: ${INSTALLED_SIZE}
Maintainer: sydrvxd <sydrvxd@users.noreply.github.com>
Homepage: https://github.com/sydrvxd/OpenVISA
Description: Open-source VISA (Virtual Instrument Software Architecture) implementation
 OpenVISA is a vendor-free, drop-in replacement for NI-VISA and
 Keysight IO Libraries. It provides the standard IVI Foundation VISA
 C API (visa.h) with support for TCPIP (raw socket, VXI-11, HiSLIP),
 USB-TMC, Serial, and GPIB transports.
EOF

# Post-install: ldconfig
cat > "$DEB_ROOT/DEBIAN/postinst" << 'EOF'
#!/bin/sh
ldconfig
EOF
chmod 755 "$DEB_ROOT/DEBIAN/postinst"

# Post-remove: ldconfig
cat > "$DEB_ROOT/DEBIAN/postrm" << 'EOF'
#!/bin/sh
ldconfig
EOF
chmod 755 "$DEB_ROOT/DEBIAN/postrm"

dpkg-deb --build "$DEB_ROOT" "$DIST/openvisa_${VERSION}_amd64.deb"

echo "Done: $(ls -lh "$DIST/openvisa_${VERSION}_amd64.deb")"
