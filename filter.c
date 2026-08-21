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

void axis_filter_init(struct axis_filter *filter,
                      int minimum,
                      int maximum,
                      int initial_value,
                      double deadzone_percent,
                      double strong_percent,
                      double gate_ms)
{
    memset(filter, 0, sizeof(*filter));
    filter->minimum = minimum;
    filter->maximum = maximum;
    filter->center = (int)(((int64_t)minimum + (int64_t)maximum + 1) / 2);

    int negative_radius = filter->center - minimum;
    int positive_radius = maximum - filter->center;
    int radius = negative_radius > positive_radius ? negative_radius : positive_radius;

    filter->deadzone = (int)(radius * deadzone_percent / 100.0);
    filter->strong_threshold = (int)(radius * strong_percent / 100.0);
    filter->crossing_delta_threshold = (int)(radius * 25.0 / 100.0);
    filter->snap_commit_threshold = (int)(radius * 80.0 / 100.0);
    filter->gate_ns = (uint64_t)(gate_ms * 1000000.0);
    filter->crossing_window_ns = 24000000ULL;
    filter->neutral_reset_ns = 50000000ULL;

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

        if (sign == filter->snap_candidate_sign &&
            magnitude >= filter->snap_commit_threshold) {
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
        filter->snap_partial_deadline_ns = now_ns + 35000000ULL;
        filter->snap_hard_deadline_ns = now_ns + 120000000ULL;
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
