#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -ne 0 ]];then printf 'Run through pkexec or sudo.\n' >&2;exit 1;fi
systemctl disable --now windchime-input.socket || true
systemctl stop windchime-input.service || true
rm -f /etc/systemd/system/windchime-input.socket /etc/systemd/system/windchime-input.service
systemctl daemon-reload
rm -f /usr/share/applications/windchime.desktop /usr/share/icons/hicolor/scalable/apps/windchime.svg
rm -f /opt/windchime/windchime /opt/windchime/windchime-input /opt/windchime/install-system.sh /opt/windchime/uninstall-system.sh /opt/windchime/uninstall.sh /opt/windchime/windchime.svg
rmdir /opt/windchime 2>/dev/null || true
printf 'System files and keyboard access removed.\n'
