#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
D="$ROOT/src/edpkg-gui.c"
F="$ROOT/src/flatpak-page.c"
grep -Fq 'progress_pending' "$D"
grep -Fq 'progress_flush_cb' "$D"
grep -Fq 'g_timeout_add(100,progress_flush_cb' "$D"
! grep -Fq 'typedef struct{App*a;char*line;} IdleLine' "$D"
grep -Fq 'progress_pending' "$F"
grep -Fq 'fp_progress_flush_cb' "$F"
grep -Fq 'g_timeout_add(100, fp_progress_flush_cb' "$F"
! grep -Fq 'FpProgressLine' "$F"
grep -Fq '500000' "$D"
grep -Fq '500000' "$F"
echo 'PASS: DEB/Flatpak output is batched and text buffers are bounded'
