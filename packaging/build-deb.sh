#!/bin/sh
# Build build/qso-zero_<version>_<arch>.deb from an already-built tree.
# Run on (or in an emulated container of) the target architecture.
set -eu
VERSION=${VERSION:-0.1.9}
ARCH=$(dpkg --print-architecture)
STAGE=build/deb-root
PKG=build/qso-zero_${VERSION}_${ARCH}.deb
rm -rf "$STAGE"
install -D -m 755 build/qso-zero               "$STAGE/usr/bin/qso-zero"
install -D -m 755 packaging/qso-zero-setup     "$STAGE/usr/bin/qso-zero-setup"
install -D -m 644 data/js8.jsc                 "$STAGE/usr/share/qso-zero/js8.jsc"
install -D -m 644 dist/qso-zero.conf.example   "$STAGE/usr/share/qso-zero/qso-zero.conf.example"
install -D -m 644 dist/qso-zero@.service       "$STAGE/usr/lib/systemd/system/qso-zero@.service"
# CardputerZero launcher (APPLaunch): menu entry + icons
install -D -m 644 packaging/qso-zero.desktop   "$STAGE/usr/share/APPLaunch/applications/qso-zero.desktop"
for i in qso-zero.png qso-zero_100.png qso-zero_80.png; do
    install -D -m 644 "packaging/appstore/$i" "$STAGE/usr/share/APPLaunch/share/images/$i"
done
install -D -m 644 README.md                    "$STAGE/usr/share/doc/qso-zero/README.md"
install -D -m 644 packaging/copyright          "$STAGE/usr/share/doc/qso-zero/copyright"
strip --strip-unneeded "$STAGE/usr/bin/qso-zero"

# libc requirement from the newest GLIBC symbol version the binary uses
GLIBC=$(objdump -T "$STAGE/usr/bin/qso-zero" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n | tail -1)
SIZE=$(du -sk "$STAGE" | cut -f1)
mkdir -p "$STAGE/DEBIAN"
sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" -e "s/@GLIBC@/$GLIBC/" -e "s/@SIZE@/$SIZE/" \
    packaging/control.in > "$STAGE/DEBIAN/control"
for s in postinst prerm postrm; do install -m 755 "packaging/$s" "$STAGE/DEBIAN/$s"; done
dpkg-deb --root-owner-group --build "$STAGE" "$PKG" >/dev/null
echo "built $PKG"
dpkg-deb --info "$PKG" | sed -n '/Package:/,$p'
