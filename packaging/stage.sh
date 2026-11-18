#!/usr/bin/env bash
# Called by AppBuilder's pack-deb.sh with STAGE and INSTALL_PREFIX
# (/usr/share/APPLaunch) exported; runs from the project root.
set -euo pipefail
install -D -m 755 build/qso-zero               "$STAGE$INSTALL_PREFIX/bin/qso-zero"
strip --strip-unneeded "$STAGE$INSTALL_PREFIX/bin/qso-zero"
install -d "$STAGE/usr/bin"
ln -s "$INSTALL_PREFIX/bin/qso-zero"           "$STAGE/usr/bin/qso-zero"       # for the shell
install -D -m 755 packaging/qso-zero-setup     "$STAGE/usr/bin/qso-zero-setup"
for i in qso-zero_100.png qso-zero_80.png; do
    install -D -m 644 "packaging/appstore/$i" "$STAGE$INSTALL_PREFIX/share/images/$i"
done
install -D -m 644 data/js8.jsc                 "$STAGE/usr/share/qso-zero/js8.jsc"
install -D -m 644 dist/qso-zero.conf.example   "$STAGE/usr/share/qso-zero/qso-zero.conf.example"
install -D -m 644 dist/qso-zero@.service       "$STAGE/usr/lib/systemd/system/qso-zero@.service"
install -D -m 644 README.md                    "$STAGE/usr/share/doc/qso-zero/README.md"
install -D -m 644 packaging/copyright          "$STAGE/usr/share/doc/qso-zero/copyright"
