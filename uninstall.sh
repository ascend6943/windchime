#!/usr/bin/env bash
set -euo pipefail
task_dir=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ $EUID -eq 0 ]];then printf 'Run this as your normal desktop user.\n' >&2;exit 1;fi
if [[ -x /opt/windchime/windchime ]];then /opt/windchime/windchime --quit || true;fi
pkexec "$task_dir/uninstall-system.sh"
task_config=${XDG_CONFIG_HOME:-"$HOME/.config"}
rm -f "$task_config/autostart/windchime.desktop" "$task_config/Windchime/Windchime.conf"
rmdir "$task_config/Windchime" 2>/dev/null || true
printf 'Windchime, keyboard access, startup entry, and settings have been removed.\n'
