#!/usr/bin/env bash
set -euo pipefail

unit=xbox-snapback-filter.service
runtime_dir="$HOME/.local/libexec/xbox-snapback-filter"
unit_path="$HOME/.config/systemd/user/$unit"

systemctl --user disable --now "$unit" 2>/dev/null || true
rm -f -- "$unit_path"
systemctl --user daemon-reload

rm -f -- "$runtime_dir/run.sh" "$runtime_dir/xbox-snapback-filter"
rmdir -- "$runtime_dir" 2>/dev/null || true

echo "Removing the controller-specific udev rule (sudo required)..."
sudo rm -f "/etc/udev/rules.d/70-xbox-snapback-filter.rules"
sudo udevadm control --reload-rules

echo "Uninstalled. Turn the controller off and on to restore normal device access."
