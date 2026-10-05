#!/usr/bin/env bash
set -euo pipefail

unit=snub.service
runtime_dir="$HOME/.local/libexec/snub"
config_file="${XDG_CONFIG_HOME:-$HOME/.config}/snub/snub.conf"

systemctl --user disable --now "$unit" 2>/dev/null || true
rm -f -- "$HOME/.config/systemd/user/$unit"
systemctl --user daemon-reload

rm -f -- "$runtime_dir/run.sh" "$runtime_dir/snub"
rmdir -- "$runtime_dir" 2>/dev/null || true

echo "Removing the udev rule (sudo required)..."
sudo rm -f /etc/udev/rules.d/72-snub.rules
sudo udevadm control --reload-rules

echo "Uninstalled. Turn the controller off and on to restore normal access."
if [[ -e "$config_file" ]]; then
    echo "Your settings were kept in $config_file."
fi
