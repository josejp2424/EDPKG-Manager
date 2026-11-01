#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LAUNCH="$ROOT/data/edpkg-gui-launcher"
MAKE="$ROOT/Makefile"
DESK="$ROOT/data/edpkg-gui.desktop"
BUILD="$ROOT/build-deb.sh"

test -x "$LAUNCH"
grep -Fq 'REAL=/usr/lib/edpkg-gui/edpkg-gui' "$LAUNCH"
grep -Fq 'if [ "$(id -u)" -eq 0 ]' "$LAUNCH"
grep -Fq 'exec gksu "$REAL"' "$LAUNCH"
! grep -Eq 'sudo|pkexec' "$LAUNCH"
grep -Fq 'install -Dm0755 data/edpkg-gui-launcher "$(DESTDIR)$(BINDIR)/edpkg-gui"' "$MAKE"
! grep -Fq 'ln -sfn ../lib/edpkg-gui/edpkg-gui' "$MAKE"
grep -Fq 'Exec=/usr/bin/edpkg-gui' "$DESK"
grep -Fq 'gksu' "$BUILD"
echo "PASS: /usr/bin launcher uses gksu and real binary stays under /usr/lib"
