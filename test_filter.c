#include "filter.h"

#include <assert.h>
#include <stdio.h>

#define MS(value) ((uint64_t)(value) * 1000000ULL)

static void init_filter(struct axis_filter *filter,
                        int minimum,
                        int maximum,
                        int initial_value,
                        double deadzone_percent)
{
    struct filter_config config;
    filter_config_defaults(&config);
    config.deadzone_percent = deadzone_percent;
    axis_filter_init(filter, minimum, maximum, initial_value, &config);
}

static void test_normal_input(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 4.0);

    struct filter_result result = axis_filter_input(&filter, -32767, MS(1));
    assert(result.emit && result.value == -32767 && !result.gate_started);

    result = axis_filter_input(&filter, 0, MS(20));
    assert(result.emit && result.value == 0);

    result = axis_filter_input(&filter, 32767, MS(100));
    assert(result.emit && result.value == 32767 && !result.gate_started);
}

static void test_short_snapback_is_discarded(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 4.0);

    axis_filter_input(&filter, -32767, MS(1));
    struct filter_result result = axis_filter_input(&filter, 21000, MS(10));
    assert(result.emit && result.value == 0 && result.snap_started);

    result = axis_filter_input(&filter, 0, MS(18));
    assert(!result.emit && result.snap_discarded);
    assert(filter.snap_started == 1 && filter.snap_discarded == 1);
}

static void test_slow_intentional_reversal_commits_after_one_frame(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 4.0);

    axis_filter_input(&filter, -32767, MS(1));
    struct filter_result result = axis_filter_input(&filter, 32767, MS(40));
    assert(result.emit && result.value == 0 && result.gate_started);

    result = axis_filter_tick(&filter, MS(52));
    assert(!result.emit && !result.gate_committed);
    result = axis_filter_tick(&filter, MS(53));
    assert(result.emit && result.value == 32767 && result.gate_committed);
}

static void test_center_deadzone(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 913, 4.0);
    assert(filter.output_value == 0);

    struct filter_result result = axis_filter_input(&filter, 1200, MS(1));
    assert(!result.emit);
    result = axis_filter_input(&filter, 2000, MS(2));
    assert(result.emit && result.value == 2000);
}

static void test_measured_ballistic_snapback_is_discarded(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 6.0);

    axis_filter_input(&filter, -32767, MS(1));
    axis_filter_input(&filter, -27098, MS(9));
    axis_filter_input(&filter, -5924, MS(17));

    struct filter_result result = axis_filter_input(&filter, 16416, MS(25));
    assert(result.emit && result.value == 0 && result.snap_started);

    result = axis_filter_input(&filter, 20808, MS(33));
    assert(!result.emit);
    result = axis_filter_input(&filter, 9463, MS(41));
    assert(!result.emit);
    result = axis_filter_input(&filter, -8325, MS(49));
    assert(!result.emit);
    result = axis_filter_input(&filter, 500, MS(81));
    assert(!result.emit && result.snap_discarded);
    assert(filter.snap_started == 1 && filter.snap_discarded == 1);
}

static void test_measured_ballistic_snapback_is_discarded_when_mirrored(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 6.0);

    axis_filter_input(&filter, 32767, MS(1));
    axis_filter_input(&filter, 27098, MS(9));
    axis_filter_input(&filter, 5924, MS(17));

    struct filter_result result = axis_filter_input(&filter, -16416, MS(25));
    assert(result.emit && result.value == 0 && result.snap_started);

    result = axis_filter_input(&filter, -20808, MS(33));
    assert(!result.emit);
    result = axis_filter_input(&filter, -9463, MS(41));
    assert(!result.emit);
    result = axis_filter_input(&filter, 8325, MS(49));
    assert(!result.emit);
    result = axis_filter_input(&filter, -500, MS(81));
    assert(!result.emit && result.snap_discarded);
    assert(filter.snap_started == 1 && filter.snap_discarded == 1);
}

static void test_strong_intentional_reversal_passes_without_software_wait(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 6.0);

    axis_filter_input(&filter, -32767, MS(1));
    axis_filter_input(&filter, -5000, MS(9));
    struct filter_result result = axis_filter_input(&filter, 16000, MS(17));
    assert(result.emit && result.value == 0 && result.snap_started);

    result = axis_filter_input(&filter, 30000, MS(25));
    assert(result.emit && result.value == 30000 && result.snap_committed);
}

static void test_ballistic_crossing_through_deadzone_keeps_history(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 6.0);

    axis_filter_input(&filter, -32767, MS(1));
    axis_filter_input(&filter, -5000, MS(9));
    axis_filter_input(&filter, -1000, MS(17));

    struct filter_result result = axis_filter_input(&filter, 9000, MS(25));
    assert(result.snap_started);
    assert(!result.emit);

    result = axis_filter_input(&filter, 0, MS(41));
    assert(result.snap_discarded);
    assert(!result.emit);
}

static void test_strong_return_to_origin_passes_without_software_wait(void)
{
    struct axis_filter filter;
    init_filter(&filter, -32768, 32767, 0, 6.0);

    axis_filter_input(&filter, -32767, MS(1));
    axis_filter_input(&filter, -5000, MS(9));
    struct filter_result result = axis_filter_input(&filter, 16000, MS(17));
    assert(result.snap_started);

    result = axis_filter_input(&filter, -30000, MS(25));
    assert(result.emit && result.value == -30000 && result.snap_committed);
}

static void test_bluetooth_axis_range(void)
{
    struct axis_filter filter;
    init_filter(&filter, 0, 65535, 32768, 6.0);
    assert(filter.center == 32768 && filter.output_value == 32768);

    axis_filter_input(&filter, 0, MS(1));
    axis_filter_input(&filter, 27000, MS(9));
    struct filter_result result = axis_filter_input(&filter, 49000, MS(17));
    assert(result.emit && result.value == 32768 && result.snap_started);

    result = axis_filter_input(&filter, 32800, MS(25));
    assert(!result.emit && result.snap_discarded);
}

static void test_config_validation(void)
{
    struct filter_config config;
    filter_config_defaults(&config);
    assert(filter_config_validate(&config) == NULL);

    config.deadzone_percent = 75.0;
    assert(filter_config_validate(&config) != NULL);

    filter_config_defaults(&config);
    config.snap_partial_ms = 200.0;
    assert(filter_config_validate(&config) != NULL);

    filter_config_defaults(&config);
    config.gate_ms = -1.0;
    assert(filter_config_validate(&config) != NULL);
}

int main(void)
{
    test_normal_input();
    test_short_snapback_is_discarded();
    test_slow_intentional_reversal_commits_after_one_frame();
    test_center_deadzone();
    test_measured_ballistic_snapback_is_discarded();
    test_measured_ballistic_snapback_is_discarded_when_mirrored();
    test_strong_intentional_reversal_passes_without_software_wait();
    test_ballistic_crossing_through_deadzone_keeps_history();
    test_strong_return_to_origin_passes_without_software_wait();
    test_bluetooth_axis_range();
    test_config_validation();
    puts("filter tests passed");
    return 0;
}
