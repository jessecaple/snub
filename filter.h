#ifndef SNUB_FILTER_H
#define SNUB_FILTER_H

#include <stdbool.h>
#include <stdint.h>

struct filter_config {
    double deadzone_percent;
    double strong_percent;
    double crossing_percent;
    double commit_percent;
    double gate_ms;
    double crossing_window_ms;
    double neutral_reset_ms;
    double snap_partial_ms;
    double snap_hard_ms;
};

struct axis_filter {
    int minimum;
    int maximum;
    int center;
    int deadzone;
    int strong_threshold;
    int crossing_delta_threshold;
    int snap_commit_threshold;
    uint64_t gate_ns;
    uint64_t crossing_window_ns;
    uint64_t neutral_reset_ns;
    uint64_t snap_partial_ns;
    uint64_t snap_hard_ns;
    uint64_t snap_partial_deadline_ns;
    uint64_t snap_hard_deadline_ns;

    int raw_value;
    int last_input_value;
    uint64_t last_input_ns;
    int last_nonzero_input_value;
    uint64_t last_nonzero_input_ns;
    int output_value;
    int stable_sign;
    int peak_magnitude;
    uint64_t neutral_since_ns;

    bool gate_active;
    int candidate_sign;
    uint64_t gate_deadline_ns;
    uint64_t gates_started;
    uint64_t gates_discarded;
    uint64_t gates_committed;

    bool snap_active;
    bool snap_oscillated;
    int snap_origin_sign;
    int snap_candidate_sign;
    uint64_t snap_started;
    uint64_t snap_discarded;
    uint64_t snap_committed;
};

struct filter_result {
    bool emit;
    int value;
    bool gate_started;
    bool gate_discarded;
    bool gate_committed;
    bool snap_started;
    bool snap_discarded;
    bool snap_committed;
};

void filter_config_defaults(struct filter_config *config);

/* Returns NULL when valid, otherwise a description of the problem. */
const char *filter_config_validate(const struct filter_config *config);

void axis_filter_init(struct axis_filter *filter,
                      int minimum,
                      int maximum,
                      int initial_value,
                      const struct filter_config *config);

struct filter_result axis_filter_input(struct axis_filter *filter,
                                       int raw_value,
                                       uint64_t now_ns);

struct filter_result axis_filter_tick(struct axis_filter *filter,
                                      uint64_t now_ns);

uint64_t axis_filter_deadline(const struct axis_filter *filter);

#endif
