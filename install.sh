#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
unit=xbox-snapback-filter.service
runtime_dir="$HOME/.local/libexec/xbox-snapback-filter"
unit_dir="$HOME/.config/systemd/user"
controller_uniq=${CONTROLLER_UNIQ:-}
generated_rule=

cleanup() {
    [[ -z "$generated_rule" ]] || rm -f -- "$generated_rule"
}
trap cleanup EXIT

if [[ -z "$controller_uniq" ]]; then
    for name_file in /sys/class/input/event*/device/name; do
        [[ -r "$name_file" ]] || continue
        device_dir=${name_file%/name}
        [[ -r "$device_dir/id/bustype" ]] || continue
        [[ -r "$device_dir/id/vendor" ]] || continue
        [[ -r "$device_dir/id/product" ]] || continue
        [[ -r "$device_dir/uniq" ]] || continue
        read -r bus < "$device_dir/id/bustype"
        read -r vendor < "$device_dir/id/vendor"
        read -r product < "$device_dir/id/product"
        if [[ "$vendor:$product:$bus" == "045e:0b22:0005" ]]; then
            read -r controller_uniq < "$device_dir/uniq"
            break
        fi
    done
fi

controller_uniq=${controller_uniq,,}
if [[ ! "$controller_uniq" =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]; then
    echo "Could not detect the controller's Bluetooth address." >&2
    echo "Connect the Elite Series 2 over Bluetooth, then run this installer again." >&2
    exit 1
fi

make -C "$project_dir" test all

generated_rule=$(mktemp /tmp/xbox-snapback-filter.XXXXXX.rules)
sed "s/@CONTROLLER_UNIQ@/$controller_uniq/g" \
    "$project_dir/70-xbox-snapback-filter.rules.in" > "$generated_rule"

echo "Installing the controller-specific udev rule (sudo required)..."
sudo install -m 0644 "$generated_rule" \
    "/etc/udev/rules.d/70-xbox-snapback-filter.rules"
sudo udevadm control --reload-rules

install -d -m 0755 "$runtime_dir" "$unit_dir"
install -m 0755 "$project_dir/xbox-snapback-filter" \
    "$runtime_dir/xbox-snapback-filter"
install -m 0755 "$project_dir/run.sh" "$runtime_dir/run.sh"

systemctl --user disable --now "$unit" >/dev/null 2>&1 || true
rm -f -- "$unit_dir/$unit"
install -m 0644 "$project_dir/$unit" "$unit_dir/$unit"
systemctl --user daemon-reload
systemctl --user enable --now "$unit"
make -C "$project_dir" clean

echo
echo "Installed and enabled $unit."
echo "Turn the controller off and on once so the new device rule takes effect."
