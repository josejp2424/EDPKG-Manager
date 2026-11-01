#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "/tmp/edpkg gui space.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cp -a "$ROOT/." "$TMP/src tree/"
cd "$TMP/src tree"
printf '#!/bin/sh\nexit 0\n' > edpkg-gui
chmod 0755 edpkg-gui
# Ensure the dummy executable is newer than the source so make only exercises install.
sleep 1
touch edpkg-gui
DEST="$TMP/package root"
make -s DESTDIR="$DEST" install
test -x "$DEST/usr/lib/edpkg-gui/edpkg-gui"
test -x "$DEST/usr/bin/edpkg-gui"
grep -Fq 'exec gksu "$REAL"' "$DEST/usr/bin/edpkg-gui"
test -f "$DEST/usr/lib/edpkg-gui/edpkg-manager.png"
test -f "$DEST/usr/share/applications/edpkg-gui.desktop"
echo "PASS: install supports source/build paths containing spaces"
