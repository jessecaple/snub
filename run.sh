#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
config_file=${SNUB_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/snub/snub.conf}

# Maps each config key to its snub option. Environment variables with the same
# names override the config file.
declare -A option_for=(
    [DEADZONE_PERCENT]=--deadzone-percent
    [STRONG_PERCENT]=--strong-percent
    [CROSSING_PERCENT]=--crossing-percent
    [COMMIT_PERCENT]=--commit-percent
    [GATE_MS]=--gate-ms
    [CROSSING_WINDOW_MS]=--crossing-window-ms
    [NEUTRAL_RESET_MS]=--neutral-reset-ms
    [SNAP_PARTIAL_MS]=--snap-partial-ms
    [SNAP_HARD_MS]=--snap-hard-ms
)
declare -A setting=()

if [[ -r "$config_file" ]]; then
    line_number=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line_number=$((line_number + 1))
        line=${line%%#*}
        [[ "$line" =~ ^[[:space:]]*$ ]] && continue
        if [[ ! "$line" =~ ^[[:space:]]*([A-Z_]+)[[:space:]]*=[[:space:]]*([^[:space:]]*)[[:space:]]*$ ]]; then
            echo "$config_file:$line_number: expected KEY=VALUE" >&2
            exit 2
        fi
        key=${BASH_REMATCH[1]}
        if [[ -z "${option_for[$key]:-}" && "$key" != VERBOSE ]]; then
            echo "$config_file:$line_number: unknown setting $key" >&2
            exit 2
        fi
        value=${BASH_REMATCH[2]}
        value=${value#[\"\']}
        value=${value%[\"\']}
        setting[$key]=$value
    done < "$config_file"
fi

for key in "${!option_for[@]}" VERBOSE; do
    if [[ -n "${!key:-}" ]]; then
        setting[$key]=${!key}
    fi
done

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
        return
    done
}

while :; do
    find_device
    [[ -n "$device" ]] && break
    if [[ "${WAIT_FOR_DEVICE:-0}" != 1 ]]; then
        echo "Xbox Elite Series 2 event device not found over USB or Bluetooth." >&2
        exit 1
    fi
    sleep 2
done

arguments=()
for key in "${!option_for[@]}"; do
    if [[ -n "${setting[$key]:-}" ]]; then
        arguments+=("${option_for[$key]}" "${setting[$key]}")
    fi
done

# Log filter decisions when run from a terminal unless VERBOSE says otherwise.
verbose=${setting[VERBOSE]:-}
if [[ -z "$verbose" ]]; then
    [[ -t 2 ]] && verbose=1 || verbose=0
fi
if [[ "$verbose" != 0 ]]; then
    arguments+=(--verbose)
fi
arguments+=("$device")

exec "$script_dir/snub" "${arguments[@]}"
