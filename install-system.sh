#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
if [[ $EUID -ne 0 ]]; then printf 'Run this installer through pkexec or sudo.\n' >&2;exit 1;fi
cd -- "$(dirname -- "$0")"
task_uid=${1:?Supply your numeric user ID}
task_user=${2:?Supply your username}
[[ $task_uid =~ ^[1-9][0-9]*$ ]] || exit 2
[[ $(id -u "$task_user") == "$task_uid" ]] || exit 2
task_gid=$(id -g "$task_user")
if [[ -x build/windchime ]];then task_build=build;else task_build=.;fi
[[ -x "$task_build/windchime" && -x "$task_build/windchime-input" ]] || { printf 'Build the application first.\n' >&2;exit 1; }
# Stage all release files before stopping the running helper. This also makes
# repair from /opt/windchime safe: no file is copied onto itself.
task_stage=$(mktemp -d /tmp/windchime-install.XXXXXXXX)
trap 'rm -rf -- "$task_stage"' EXIT
for task_binary in windchime windchime-input;do
    install -o root -g root -m 0755 "$task_build/$task_binary" "$task_stage/$task_binary"
done
for task_script in install-system.sh uninstall-system.sh uninstall.sh;do
    install -o root -g root -m 0755 "$task_script" "$task_stage/$task_script"
done
for task_asset in windchime.svg windchime.desktop;do
    install -o root -g root -m 0644 "$task_asset" "$task_stage/$task_asset"
done
if [[ -f /etc/systemd/system/windchime-input.socket ]];then systemctl stop windchime-input.socket windchime-input.service;fi
install -d -o root -g root -m 0755 /opt/windchime
for task_binary in windchime windchime-input;do
    install -o root -g root -m 0755 "$task_stage/$task_binary" "/opt/windchime/$task_binary"
done
for task_script in install-system.sh uninstall-system.sh uninstall.sh;do
    install -o root -g root -m 0755 "$task_stage/$task_script" "/opt/windchime/$task_script"
done
for task_asset in windchime.svg windchime.desktop;do
    install -o root -g root -m 0644 "$task_stage/$task_asset" "/opt/windchime/$task_asset"
done
install -o root -g root -m 0644 "$task_stage/windchime.desktop" /usr/share/applications/windchime.desktop
install -d -o root -g root -m 0755 /usr/share/icons/hicolor/scalable/apps
install -o root -g root -m 0644 "$task_stage/windchime.svg" /usr/share/icons/hicolor/scalable/apps/windchime.svg
cat > /etc/systemd/system/windchime-input.socket <<EOF
[Unit]
Description=Windchime keyboard note socket

[Socket]
ListenStream=/run/windchime/input.sock
SocketUser=$task_uid
SocketGroup=$task_gid
SocketMode=0600
DirectoryMode=0755
RemoveOnStop=yes

[Install]
WantedBy=sockets.target
EOF
cat > /etc/systemd/system/windchime-input.service <<EOF
[Unit]
Description=Windchime physical keyboard note reader
Requires=windchime-input.socket
After=systemd-logind.service

[Service]
ExecStart=/opt/windchime/windchime-input $task_uid
Restart=on-failure
RestartSec=2
User=root
Group=root
NoNewPrivileges=yes
CapabilityBoundingSet=CAP_DAC_READ_SEARCH
AmbientCapabilities=CAP_DAC_READ_SEARCH
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateNetwork=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_UNIX
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
UMask=0077
StandardOutput=null
StandardError=journal
EOF
systemctl daemon-reload
systemctl enable --now windchime-input.socket
systemctl try-restart windchime-input.service
if command -v update-desktop-database >/dev/null;then update-desktop-database /usr/share/applications;fi
printf 'Windchime is installed. Keyboard access is restricted to %s.\n' "$task_user"
