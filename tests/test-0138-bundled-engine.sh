#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ENGINE="$ROOT/third_party/am-10.6/am"
[ -x "$ENGINE" ]
[ -r "$ROOT/third_party/am-10.6/LICENSE" ]
[ -r "$ROOT/third_party/am-10.6/ATTRIBUTION.md" ]
! grep -q 'AM is not installed' "$ROOT/src/appimage-i18n.c"
! grep -q 'AM/main/INSTALL' "$ROOT/src/appimage-page.c"
grep -q 'AI_BUNDLED_BACKEND' "$ROOT/src/appimage-page.c"
grep -q 'EDPKG_APPIMAGE_EMBEDDED' "$ENGINE"
grep -q 'gtk_about_dialog_add_credit_section' "$ROOT/src/edpkg-gui.c"
bash -n "$ENGINE"
for m in "$ROOT"/third_party/am-10.6/modules/*.am; do bash -n "$m"; done
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home" "$T/data" "$T/cache" "$T/config"
# No Internet and no global AM binary required to list installed packages.
TERM=dumb EDPKG_APPIMAGE_EMBEDDED=1 NO_COLOR=1 LANG=C LANGUAGE=en \
HOME="$T/home" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache" \
XDG_CONFIG_HOME="$T/config" timeout 12 "$ENGINE" -fi --byname > "$T/list" 2>&1
# Distros with no /opt app images still output the table header.
grep -q 'APPNAME' "$T/list"
! grep -q 'Shell completion has been enabled' "$T/list"
! test -f "$T/data/applications/am-install.desktop"
! test -f "$T/data/bash-completion/completions/am"
printf '%s\n' 'Bundled AM engine: local listing OK; no external AM installation or automatic protocol/CLI setup.'
