#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
./build.sh
task_test=$(mktemp -d /tmp/windchime-checks.XXXXXXXX)
trap 'rm -rf -- "$task_test"' EXIT
mkdir -p "$task_test/config" "$task_test/runtime"
chmod 700 "$task_test/runtime"
QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$task_test/config" XDG_RUNTIME_DIR="$task_test/runtime" build/windchime --ui-check
python3 tests/check_helper.py
"${CXX:-g++}" -std=c++20 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined src/tests.cpp -o "$task_test/core-sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$task_test/core-sanitized"
printf 'All available checks passed.\n'
