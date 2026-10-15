#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ ! -x build/windchime || ! -x build/windchime-input ]];then ./build.sh;fi
pkexec "$PWD/install-system.sh" "$(id -u)" "$(id -un)"
exec /opt/windchime/windchime --show
