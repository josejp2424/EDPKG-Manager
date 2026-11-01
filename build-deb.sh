#!/bin/sh
set -eu
VERSION=0.1.40
ARCH="${ARCH:-$(dpkg --print-architecture 2>/dev/null || uname -m)}"
case "$ARCH" in x86_64) ARCH=amd64;; aarch64) ARCH=arm64;; esac
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PKG="$ROOT/build/edpkg-gui_${VERSION}_${ARCH}"
OUT="$ROOT/dist"
rm -rf "$ROOT/build" "$OUT"
mkdir -p "$PKG/DEBIAN" "$OUT"
make -C "$ROOT" clean all
make -C "$ROOT" DESTDIR="$PKG" install
cat > "$PKG/DEBIAN/control" <<CONTROL
Package: edpkg-gui
Version: $VERSION
Architecture: $ARCH
Section: admin
Priority: optional
Depends: libgtk-3-0, flatpak, gksu, dpkg | edpkg, apt | edpkg, curl | wget, bash, coreutils, grep, sed, findutils, tar, gzip
Maintainer: Essora Project
Homepage: https://www.essora.org
Description: Essora GTK3 package manager with DEB, Flatpak and AppImage
 Synaptic-style DEB package list with automatic EDPKG or APT/dpkg backend.
 Adds a lightweight Flatpak tab using a persistent catalog and icon index,
 system Flathub initialization on first use, multi-selection and transactions.
 AppImage support includes an AM 10.6-derived internal engine, lazy loading and cache.
CONTROL
cat > "$PKG/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
# 0.1.6 briefly copied Flatpak icons into /usr/lib/eapt/icons.
# 0.1.7 uses the real Flatpak export tree directly and keeps only an index.
rm -rf /usr/lib/eapt/icons
rm -f /usr/lib/eapt/flatpak-cache-v1.tsv /usr/lib/eapt/flatpak-icons-v1.tsv /usr/lib/eapt/.flatpak-initialized
# Only migration from 0.1.38 needs the one-time AppImage launcher repair.
# Limit it to the version with the old embedded AM engine. Never change a
# pre-existing system launcher or move an installed AppImage binary.
case "${2:-}" in
  0.1.38*)
    if [ -x /usr/lib/edpkg-gui/appimage-desktop-migrate ]; then
      /usr/lib/edpkg-gui/appimage-desktop-migrate --all || :
      # PyMenu may cache menus; rebuild once on upgrade if supported.
      if [ -x /usr/bin/fixmenus ]; then /usr/bin/fixmenus >/dev/null 2>&1 || :
      elif [ -x /usr/local/bin/fixmenus ]; then /usr/local/bin/fixmenus >/dev/null 2>&1 || :
      fi
    fi
    ;;
esac
exit 0
POSTINST
chmod 0755 "$PKG/DEBIAN/postinst"
find "$PKG" -type d -exec chmod 0755 {} +
if command -v edpkg >/dev/null 2>&1; then
    edpkg build "$PKG" "$OUT/edpkg-gui_${VERSION}_${ARCH}.deb"
elif command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb --root-owner-group --build "$PKG" "$OUT/edpkg-gui_${VERSION}_${ARCH}.deb"
else
    echo "Need edpkg or dpkg-deb to build package" >&2
    exit 1
fi
echo "$OUT/edpkg-gui_${VERSION}_${ARCH}.deb"
