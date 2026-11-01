#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ENGINE="$ROOT/third_party/am-10.6"
HELPER="$ROOT/data/appimage-desktop-migrate"
bash -n "$ENGINE/modules/install.am"
bash -n "$ENGINE/modules/management.am"
sh -n "$HELPER"
# Verify the original empty-SUDOCMD quoted command is gone.
! grep -Fq '"$SUDOCMD" mkdir' "$ENGINE/modules/install.am"
grep -Fq 'EDPKG_APPIMAGE_EMBEDDED:-' "$ENGINE/modules/install.am"
grep -Fq "s#/usr/local/share/applications#/usr/share/applications#g" "$ENGINE/modules/install.am"
# Verify that the exact sed rewrite used by the installer updates both the
# .desktop install command and the generated removal instructions.
T_SCRIPT=$(mktemp)
trap 'rm -f "$T_SCRIPT"' EXIT HUP INT TERM
cat > "$T_SCRIPT" <<'RECIPE'
mkdir -p /opt/3d-puzzles/tmp
printf '%s\n' 'rm -f /usr/local/share/applications/$APP-AM.desktop' > ./remove
mv ./3d-puzzles.desktop /usr/local/share/applications/3d-puzzles-AM.desktop
RECIPE
sed -i 's#/usr/local/share/applications#/usr/share/applications#g' "$T_SCRIPT"
! grep -q '/usr/local/share/applications' "$T_SCRIPT"
grep -q '/usr/share/applications/3d-puzzles-AM.desktop' "$T_SCRIPT"
rm -f "$T_SCRIPT"
# A real AM recipe writes its launcher and remove script to /usr/local.
# Test migration on a fake filesystem, without modifying /opt or /usr.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT HUP INT TERM
mkdir -p "$T/usr/local/share/applications" "$T/opt/3d-puzzles"
cat > "$T/usr/local/share/applications/3d-puzzles-AM.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=3D Puzzles
Exec=3d-puzzles
Icon=/opt/3d-puzzles/icons/3d-puzzles
Categories=Game;
DESKTOP
cat > "$T/opt/3d-puzzles/remove" <<'REMOVE'
#!/bin/sh
rm -f /usr/local/bin/3d-puzzles
rm -f /usr/local/share/applications/3d-puzzles-AM.desktop
rm -rf /opt/3d-puzzles
REMOVE
EDPKG_DESKTOP_TEST=1 "$HELPER" --test-root "$T" 3d-puzzles
[ -s "$T/usr/share/applications/3d-puzzles-AM.desktop" ]
[ ! -e "$T/usr/local/share/applications/3d-puzzles-AM.desktop" ]
grep -q '/usr/share/applications/3d-puzzles-AM.desktop' "$T/opt/3d-puzzles/remove"
# Running it a second time must not overwrite an existing launcher.
if EDPKG_DESKTOP_TEST=1 "$HELPER" --test-root "$T" 3d-puzzles >/dev/null 2>&1; then exit 1; fi
# A name with path separators must not modify anything.
if EDPKG_DESKTOP_TEST=1 "$HELPER" --test-root "$T" '../etc/passwd' >/dev/null 2>&1; then exit 1; fi
# Missing /opt/<app>/remove -> do not move an unrelated launcher.
cat > "$T/usr/local/share/applications/other-AM.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Other
DESKTOP
EDPKG_DESKTOP_TEST=1 "$HELPER" --test-root "$T" --all
[ -f "$T/usr/local/share/applications/other-AM.desktop" ]
[ ! -f "$T/usr/share/applications/other-AM.desktop" ]
printf '%s\n' 'Essora launcher migration: OK (legacy entry, remove script, no overwrite, no arbitrary path, no unrelated apps).'
