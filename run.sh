#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
device=

find_device() {
    device=
    for name_file in /sys/class/input/event*/device/name; do
        [[ -r "$name_file" ]] || continue
        device_dir=${name_file%/name}
        [[ -r "$device_dir/id/bustype" ]] || continue
        [[ -r "$device_dir/id/vendor" ]] || continue
        [[ -r "$device_dir/id/product" ]] || continue
        read -r bus < "$device_dir/id/bustype"
        read -r vendor < "$device_dir/id/vendor"
        read -r product < "$device_dir/id/product"
        if [[ "$vendor:$product:$bus" != "045e:0b00:0003" &&
              "$vendor:$product:$bus" != "045e:0b22:0005" ]]; then
            continue
        fi
        event_name=${name_file#/sys/class/input/}
        event_name=${event_name%%/*}
        device="/dev/input/$event_name"
        break
    done
}

while :; do
    find_device
    [[ -n "$device" ]] && break
    if [[ "${WAIT_FOR_DEVICE:-0}" != 1 ]]; then
        echo "Xbox Elite Series 2 event device not found over USB or Bluetooth." >&2
        exit 1
    fi
    sleep 1
done

arguments=(
    --gate-ms "${GATE_MS:-13}"
    --deadzone-percent "${DEADZONE_PERCENT:-6}"
    --strong-percent "${STRONG_PERCENT:-70}"
)
if [[ "${VERBOSE:-1}" != 0 ]]; then
    arguments+=(--verbose)
fi
arguments+=("$device")

exec "$project_dir/xbox-snapback-filter" "${arguments[@]}"
