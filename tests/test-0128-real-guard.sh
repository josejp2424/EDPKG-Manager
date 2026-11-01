#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BIN="$ROOT/edpkg-gui"
SRC="$ROOT/src/edpkg-gui.c"
[ -x "$BIN" ] || { echo "edpkg-gui binary missing; run make first" >&2; exit 1; }
grep -q 'g_usleep(50000)' "$SRC"
! grep -q 'capture_argv0' "$SRC"
grep -q 'static void cancel_phase_package_command' "$SRC"
grep -q 'line_is_safe_download' "$SRC"
grep -q 'messagedialog, messagedialog box' "$SRC"
grep -q '^strict:' "$ROOT/Makefile"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
FAKE="$TMP/edpkg"
LOG="$TMP/invocations.log"
cat > "$FAKE" <<'FAKE'
#!/bin/sh
printf 'args=' >> "${EDPKG_FAKE_LOG:?}"
printf ' %s' "$@" >> "$EDPKG_FAKE_LOG"
printf '\n' >> "$EDPKG_FAKE_LOG"
case "${1-}" in
  --help)
    # Deliberately non-zero, like apt/dpkg-style tools may do for usage/help.
    echo 'edpkg fake - Options: --simulate --dry-run -y --print-architecture' >&2
    exit 1
    ;;
  --simulate)
    echo 'edpkg: transaction plan: 1 package(s), 1 change(s)'
    echo 'Inst essora-welcome:amd64 (1.0)'
    exit 0
    ;;
  *)
    echo 'unexpected fake edpkg invocation' >&2
    exit 2
    ;;
esac
FAKE
chmod 0755 "$FAKE"
EDPKG_GUI_EDPKG_BIN="$FAKE" EDPKG_FAKE_LOG="$LOG" "$BIN" --selftest-edpkg-guard >"$TMP/out" 2>"$TMP/err"
grep -q 'EDPKG guard selftest: SAFE' "$TMP/out"
grep -q -- '--help' "$LOG"
grep -q -- '--simulate' "$LOG"
# The override is test-only; normal/root execution is pinned to /usr/bin/edpkg.
grep -q 'edpkg_test_override_allowed=TRUE' "$SRC"
echo '0.1.28 real EDPKG guard: OK'
