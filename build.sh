#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
task_sysroot=${WINDCHIME_SYSROOT:-/}
task_cxx=${CXX:-g++}
task_triplet=$("$task_cxx" -dumpmachine)
task_qt=""
for task_candidate in "$task_sysroot/usr/include/$task_triplet/qt6" "$task_sysroot/usr/include/qt6" "$task_sysroot/usr/include/qt";do
    if [[ -f "$task_candidate/QtCore/qobject.h" ]];then task_qt=$task_candidate;break;fi
done
if [[ -z $task_qt ]];then
    printf 'Qt 6 development headers are required. On Ubuntu install: build-essential qt6-base-dev qt6-base-dev-tools libpulse-dev libsystemd-dev\n' >&2
    exit 1
fi
task_moc=${MOC:-}
if [[ -z $task_moc ]];then
    for task_candidate in "$task_sysroot/usr/lib/qt6/libexec/moc" "$task_sysroot/usr/lib64/qt6/libexec/moc";do
        if [[ -x $task_candidate ]];then task_moc=$task_candidate;break;fi
    done
fi
[[ -n $task_moc ]] || { printf 'Qt 6 moc was not found. Set MOC to its path.\n' >&2;exit 1; }
mkdir -p build
task_flags=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -fPIC -pthread -I"$task_qt" -I"$task_qt/QtCore" -I"$task_qt/QtGui" -I"$task_qt/QtWidgets" -I"$task_qt/QtNetwork" -I"$task_qt/QtDBus" -I"$task_sysroot/usr/include")
"$task_moc" src/main.cpp -o build/main.moc
"$task_cxx" "${task_flags[@]}" -Ibuild src/main.cpp -o build/windchime -l:libQt6Widgets.so.6 -l:libQt6Gui.so.6 -l:libQt6Core.so.6 -l:libQt6Network.so.6 -l:libQt6DBus.so.6 -l:libpulse.so.0
"$task_cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic -I"$task_sysroot/usr/include" src/helper.cpp -o build/windchime-input -l:libsystemd.so.0
"$task_cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic src/tests.cpp -o build/windchime-tests
build/windchime-tests
printf 'Native build and core checks passed.\n'
