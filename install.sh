#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
unit=snub.service
runtime_dir="$HOME/.local/libexec/snub"
unit_dir="$HOME/.config/systemd/user"
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/snub"
rule_path=/etc/udev/rules.d/72-snub.rules
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
if [[ -n "$controller_uniq" &&
      ! "$controller_uniq" =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]; then
    echo "Invalid Bluetooth address: $controller_uniq" >&2
    exit 1
fi

make -C "$project_dir" test all

generated_rule=$(mktemp)
if [[ -n "$controller_uniq" ]]; then
    sed "s/@CONTROLLER_UNIQ@/$controller_uniq/g" \
        "$project_dir/72-snub.rules.in" > "$generated_rule"
else
    # Without an address, keep everything before the Bluetooth access rule.
    sed '/^# Over Bluetooth/,$d' "$project_dir/72-snub.rules.in" \
        > "$generated_rule"
fi

echo "Installing the udev rule (sudo required)..."
sudo install -m 0644 "$generated_rule" "$rule_path"
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput

install -d -m 0755 "$runtime_dir" "$unit_dir"
install -m 0755 "$project_dir/snub" "$runtime_dir/snub"
install -m 0755 "$project_dir/run.sh" "$runtime_dir/run.sh"
if [[ ! -e "$config_dir/snub.conf" ]]; then
    install -d -m 0755 "$config_dir"
    install -m 0644 "$project_dir/snub.conf" "$config_dir/snub.conf"
fi

install -m 0644 "$project_dir/$unit" "$unit_dir/$unit"
systemctl --user daemon-reload
systemctl --user enable "$unit"
systemctl --user restart "$unit"
make -C "$project_dir" clean

echo
echo "Installed and started $unit. Settings: $config_dir/snub.conf"
if [[ -n "$controller_uniq" ]]; then
    echo "Turn the controller off and on once so the new udev rule applies."
else
    echo "No Bluetooth controller was found, so only USB is set up. To add"
    echo "Bluetooth, connect the controller over Bluetooth and run this again."
fi
