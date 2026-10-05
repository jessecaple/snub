#include "filter.h"

#include <stdlib.h>
#include <string.h>

static int sign_of(int value)
{
    return (value > 0) - (value < 0);
}

static int magnitude_from_center(const struct axis_filter *filter, int value)
{
    return abs(value - filter->center);
}

static int filtered_value(const struct axis_filter *filter, int raw_value)
{
    if (magnitude_from_center(filter, raw_value) <= filter->deadzone) {
        return filter->center;
    }
    return raw_value;
}

static struct filter_result emit_if_changed(struct axis_filter *filter,
                                            int value)
{
    struct filter_result result = {0};
    if (value != filter->output_value) {
        filter->output_value = value;
        result.emit = true;
        result.value = value;
    }
    return result;
}

void filter_config_defaults(struct filter_config *config)
{
    config->deadzone_percent = 6.0;
    config->strong_percent = 70.0;
    config->crossing_percent = 25.0;
    config->commit_percent = 80.0;
    config->gate_ms = 13.0;
    config->crossing_window_ms = 24.0;
    config->neutral_reset_ms = 50.0;
    config->snap_partial_ms = 35.0;
    config->snap_hard_ms = 120.0;
}

static bool in_range(double value, double minimum, double maximum)
{
    return value >= minimum && value <= maximum;
}

const char *filter_config_validate(const struct filter_config *config)
{
    if (!in_range(config->deadzone_percent, 0.0, 99.0)) {
        return "deadzone must be between 0 and 99 percent";
    }
    if (!in_range(config->strong_percent, 1.0, 100.0) ||
        !in_range(config->crossing_percent, 1.0, 100.0) ||
        !in_range(config->commit_percent, 1.0, 100.0)) {
        return "strong, crossing and commit must be between 1 and 100 percent";
    }
    if (config->strong_percent <= config->deadzone_percent ||
        config->commit_percent <= config->deadzone_percent) {
        return "strong and commit must be larger than the deadzone";
    }
    if (!in_range(config->gate_ms, 0.0, 1000.0) ||
        !in_range(config->crossing_window_ms, 0.0, 1000.0) ||
        !in_range(config->neutral_reset_ms, 0.0, 1000.0) ||
        !in_range(config->snap_partial_ms, 0.0, 1000.0) ||
        !in_range(config->snap_hard_ms, 0.0, 1000.0)) {
        return "times must be between 0 and 1000 ms";
    }
    if (config->snap_partial_ms > config->snap_hard_ms) {
        return "snap partial time must not exceed snap hard time";
    }
    return NULL;
}

static uint64_t ms_to_ns(double ms)
{
    return (uint64_t)(ms * 1000000.0);
}

void axis_filter_init(struct axis_filter *filter,
                      int minimum,
                      int maximum,
                      int initial_value,
                      const struct filter_config *config)
{
    memset(filter, 0, sizeof(*filter));
    filter->minimum = minimum;
    filter->maximum = maximum;
    filter->center = (int)(((int64_t)minimum + (int64_t)maximum + 1) / 2);

    int negative_radius = filter->center - minimum;
    int positive_radius = maximum - filter->center;
    int radius = negative_radius > positive_radius ? negative_radius : positive_radius;

    filter->deadzone = (int)(radius * config->deadzone_percent / 100.0);
    filter->strong_threshold = (int)(radius * config->strong_percent / 100.0);
    filter->crossing_delta_threshold =
        (int)(radius * config->crossing_percent / 100.0);
    filter->snap_commit_threshold = (int)(radius * config->commit_percent / 100.0);
    filter->gate_ns = ms_to_ns(config->gate_ms);
    filter->crossing_window_ns = ms_to_ns(config->crossing_window_ms);
    filter->neutral_reset_ns = ms_to_ns(config->neutral_reset_ms);
    filter->snap_partial_ns = ms_to_ns(config->snap_partial_ms);
    filter->snap_hard_ns = ms_to_ns(config->snap_hard_ms);

    filter->raw_value = initial_value;
    filter->last_input_value = initial_value;
    filter->output_value = filtered_value(filter, initial_value);
    int centered = filter->output_value - filter->center;
    filter->stable_sign = sign_of(centered);
    filter->peak_magnitude = abs(centered);
    if (filter->stable_sign != 0) {
        filter->last_nonzero_input_value = initial_value;
    }
}

struct filter_result axis_filter_input(struct axis_filter *filter,
                                       int raw_value,
                                       uint64_t now_ns)
{
    int previous_value = filtered_value(filter, filter->last_input_value);
    int previous_centered = previous_value - filter->center;
    int previous_sign = sign_of(previous_centered);
    int crossing_reference_value = previous_value;
    uint64_t crossing_reference_ns = filter->last_input_ns;
    if (previous_sign == 0 && filter->last_nonzero_input_ns != 0) {
        crossing_reference_value = filtered_value(
            filter, filter->last_nonzero_input_value);
        crossing_reference_ns = filter->last_nonzero_input_ns;
    }
    int crossing_reference_centered = crossing_reference_value - filter->center;
    int crossing_reference_sign = sign_of(crossing_reference_centered);
    uint64_t elapsed_ns = crossing_reference_ns == 0
        ? UINT64_MAX
        : now_ns - crossing_reference_ns;

    filter->raw_value = raw_value;
    filter->last_input_value = raw_value;
    filter->last_input_ns = now_ns;
    int value = filtered_value(filter, raw_value);
    int centered = value - filter->center;
    int sign = sign_of(centered);
    int magnitude = abs(centered);
    if (sign != 0) {
        filter->last_nonzero_input_value = raw_value;
        filter->last_nonzero_input_ns = now_ns;
    }

    if (filter->snap_active) {
        if (sign == 0) {
            filter->snap_active = false;
            filter->snap_discarded++;
            filter->neutral_since_ns = now_ns;
            struct filter_result result = emit_if_changed(filter, filter->center);
            result.snap_discarded = true;
            return result;
        }

        /*
         * A rebound never reaches the commit threshold, so a large deflection
         * in either direction is deliberate and passes immediately.
         */
        if (magnitude >= filter->snap_commit_threshold) {
            filter->snap_active = false;
            filter->snap_committed++;
            filter->stable_sign = sign;
            filter->peak_magnitude = magnitude;
            filter->neutral_since_ns = 0;
            struct filter_result result = emit_if_changed(filter, value);
            result.snap_committed = true;
            return result;
        }

        if (sign == filter->snap_origin_sign) {
            filter->snap_oscillated = true;
        }
        return emit_if_changed(filter, filter->center);
    }

    if (filter->gate_active) {
        if (sign == 0) {
            filter->gate_active = false;
            filter->gates_discarded++;
            struct filter_result result = emit_if_changed(filter, filter->center);
            result.gate_discarded = true;
            filter->neutral_since_ns = now_ns;
            return result;
        }

        struct filter_result result = emit_if_changed(filter, filter->center);
        return result;
    }

    if (sign == 0) {
        if (filter->neutral_since_ns == 0) {
            filter->neutral_since_ns = now_ns;
        }
        return emit_if_changed(filter, filter->center);
    }

    bool neutral_was_brief = filter->neutral_since_ns == 0 ||
        now_ns - filter->neutral_since_ns < filter->neutral_reset_ns;
    bool opposite = filter->stable_sign != 0 && sign != filter->stable_sign;
    bool was_strong = filter->peak_magnitude >= filter->strong_threshold;
    bool crossed_fast = crossing_reference_sign != 0 &&
        sign != crossing_reference_sign &&
        elapsed_ns <= filter->crossing_window_ns &&
        abs(centered - crossing_reference_centered) >=
            filter->crossing_delta_threshold;

    if (opposite && was_strong && crossed_fast &&
        magnitude < filter->snap_commit_threshold) {
        filter->snap_active = true;
        filter->snap_oscillated = false;
        filter->snap_origin_sign = filter->stable_sign;
        filter->snap_candidate_sign = sign;
        filter->snap_partial_deadline_ns = now_ns + filter->snap_partial_ns;
        filter->snap_hard_deadline_ns = now_ns + filter->snap_hard_ns;
        filter->snap_started++;

        struct filter_result result = emit_if_changed(filter, filter->center);
        result.snap_started = true;
        return result;
    }

    if (opposite && was_strong && crossed_fast &&
        magnitude >= filter->snap_commit_threshold) {
        filter->stable_sign = sign;
        filter->peak_magnitude = magnitude;
        filter->neutral_since_ns = 0;
        return emit_if_changed(filter, value);
    }

    if (opposite && was_strong && neutral_was_brief) {
        filter->gate_active = true;
        filter->candidate_sign = sign;
        filter->gate_deadline_ns = now_ns + filter->gate_ns;
        filter->gates_started++;

        struct filter_result result = emit_if_changed(filter, filter->center);
        result.gate_started = true;
        return result;
    }

    if (filter->stable_sign == sign) {
        if (magnitude > filter->peak_magnitude) {
            filter->peak_magnitude = magnitude;
        }
    } else {
        filter->stable_sign = sign;
        filter->peak_magnitude = magnitude;
    }
    filter->neutral_since_ns = 0;
    return emit_if_changed(filter, value);
}

struct filter_result axis_filter_tick(struct axis_filter *filter,
                                      uint64_t now_ns)
{
    struct filter_result result = {0};

    if (filter->snap_active) {
        int value = filtered_value(filter, filter->raw_value);
        int centered = value - filter->center;
        int sign = sign_of(centered);

        bool partial_intentional = !filter->snap_oscillated &&
            now_ns >= filter->snap_partial_deadline_ns &&
            sign == filter->snap_candidate_sign;
        bool hard_timeout = now_ns >= filter->snap_hard_deadline_ns;

        if (partial_intentional || hard_timeout) {
            filter->snap_active = false;
            filter->snap_committed++;
            filter->stable_sign = sign;
            filter->peak_magnitude = abs(centered);
            filter->neutral_since_ns = sign == 0 ? now_ns : 0;
            result = emit_if_changed(filter, value);
            result.snap_committed = true;
            return result;
        }

        return result;
    }

    if (!filter->gate_active || now_ns < filter->gate_deadline_ns) {
        return result;
    }

    filter->gate_active = false;
    int value = filtered_value(filter, filter->raw_value);
    int centered = value - filter->center;
    int sign = sign_of(centered);

    if (sign == 0) {
        filter->gates_discarded++;
        filter->neutral_since_ns = now_ns;
        result = emit_if_changed(filter, filter->center);
        result.gate_discarded = true;
        return result;
    }

    filter->stable_sign = sign;
    filter->peak_magnitude = abs(centered);
    filter->neutral_since_ns = 0;
    filter->gates_committed++;
    result = emit_if_changed(filter, value);
    result.gate_committed = true;
    return result;
}

uint64_t axis_filter_deadline(const struct axis_filter *filter)
{
    if (filter->snap_active) {
        return filter->snap_oscillated
            ? filter->snap_hard_deadline_ns
            : filter->snap_partial_deadline_ns;
    }
    return filter->gate_active ? filter->gate_deadline_ns : 0;
}
