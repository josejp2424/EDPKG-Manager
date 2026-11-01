#!/bin/sh
set -eu
SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/src/edpkg-gui.c"
for fam in linux-image linux-kbuild linux-headers linux-modules linux-source linux-config linux-compiler linux-support linux-doc kernel-image kernel-headers kernel-modules; do
    grep -Fq "\"$fam\"" "$SRC"
done
grep -Fq 'apt_simulation_kernel_check' "$SRC"
grep -Fq 'run_apt_guarded' "$SRC"
grep -Fq 'package resolution would change protected kernel package' "$SRC"
grep -Fq 'edpkg_dependency_guard' "$SRC"
grep -Fq 'dependency simulation failed:' "$SRC"
# Installed kernel packages remain visible in local status; the repository catalog remains blocked.
python3 - "$SRC" <<'PY'
import sys
s=open(sys.argv[1],encoding='utf-8').read()
a=s.index('static void accept_status_stanza')
b=s.index('static void clear_local_package_state',a)
assert 'package_is_kernel_blocked' not in s[a:b]
a=s.index('static void accept_repo_stanza')
b=s.index('static void parse_packages_file_fast',a)
assert 'package_is_kernel_blocked' in s[a:b]
print('PASS: installed kernels remain visible while repository/actions are protected')
PY
