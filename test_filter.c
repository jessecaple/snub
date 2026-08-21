#include "filter.h"

#include <assert.h>
#include <stdio.h>

#define MS(value) ((uint64_t)(value) * 1000000ULL)

static void test_normal_input(void)
{
    struct axis_filter filter;
    axis_filter_init(&filter, -32768, 32767, 0, 4.0, 70.0, 13.0);

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
    axis_filter_init(&filter, -32768, 32767, 0, 4.0, 70.0, 13.0);

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
    axis_filter_init(&filter, -32768, 32767, 0, 4.0, 70.0, 13.0);

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
    axis_filter_init(&filter, -32768, 32767, 913, 4.0, 70.0, 13.0);
    assert(filter.output_value == 0);

    struct filter_result result = axis_filter_input(&filter, 1200, MS(1));
    assert(!result.emit);
    result = axis_filter_input(&filter, 2000, MS(2));
    assert(result.emit && result.value == 2000);
}

static void test_measured_ballistic_snapback_is_discarded(void)
{
    struct axis_filter filter;
    axis_filter_init(&filter, -32768, 32767, 0, 6.0, 70.0, 13.0);

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
    axis_filter_init(&filter, -32768, 32767, 0, 6.0, 70.0, 13.0);

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
    axis_filter_init(&filter, -32768, 32767, 0, 6.0, 70.0, 13.0);

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
    axis_filter_init(&filter, -32768, 32767, 0, 6.0, 70.0, 13.0);

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
    puts("filter tests passed");
    return 0;
}
