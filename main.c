#define _GNU_SOURCE

#include "filter.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, array) ((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)
#define SET_BIT(bit, array) (array[(bit) / BITS_PER_LONG] |= 1UL << ((bit) % BITS_PER_LONG))
#define CLEAR_BIT(bit, array) (array[(bit) / BITS_PER_LONG] &= ~(1UL << ((bit) % BITS_PER_LONG)))

/*
 * Steam and SDL identify controllers partly by name, so keep this stable to
 * preserve existing per-controller settings.
 */
#define VIRTUAL_DEVICE_NAME "Filtered Xbox Elite Series 2"

static volatile sig_atomic_t running = 1;

struct bridge {
    int input_fd;
    int uinput_fd;
    bool verbose;
    bool event_clock;
    uint64_t last_ns;
    unsigned long key_bits[NBITS(KEY_MAX + 1)];
    unsigned long abs_bits[NBITS(ABS_MAX + 1)];
    unsigned long keys_down[NBITS(KEY_MAX + 1)];
    struct axis_filter x;
    struct axis_filter y;
};

/*
 * The Bluetooth HID descriptor on this controller advertises a malformed
 * bitmap containing every keyboard key.  Copying those capabilities makes
 * udev classify the virtual device as a keyboard instead of a joystick.
 * Linux reserves codes from BTN_MISC upward for buttons; the Elite 2's real
 * controls (including its paddles) are all in that range.
 */
static bool is_controller_button(uint16_t code)
{
    return code >= BTN_MISC;
}

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* The filter measures elapsed time, so never let its clock run backwards. */
static uint64_t advance_clock(struct bridge *bridge, uint64_t now_ns)
{
    if (now_ns < bridge->last_ns) {
        now_ns = bridge->last_ns;
    }
    bridge->last_ns = now_ns;
    return now_ns;
}

static uint64_t event_time_ns(struct bridge *bridge, const struct input_event *event)
{
    if (!bridge->event_clock) {
        return advance_clock(bridge, monotonic_ns());
    }
    return advance_clock(bridge,
                         (uint64_t)event->input_event_sec * 1000000000ULL +
                         (uint64_t)event->input_event_usec * 1000ULL);
}

static int set_event_capabilities(struct bridge *bridge)
{
    unsigned long event_bits[NBITS(EV_MAX + 1)] = {0};
    int input_fd = bridge->input_fd;
    int uinput_fd = bridge->uinput_fd;

    if (ioctl(input_fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0) {
        perror("EVIOCGBIT events");
        return -1;
    }

    if (TEST_BIT(EV_KEY, event_bits)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_KEY, sizeof(bridge->key_bits)),
                  bridge->key_bits) < 0) {
            perror("configure keys");
            return -1;
        }
        for (int code = 0; code <= KEY_MAX; code++) {
            if (!TEST_BIT(code, bridge->key_bits)) {
                continue;
            }
            if (!is_controller_button((uint16_t)code)) {
                CLEAR_BIT(code, bridge->key_bits);
                continue;
            }
            if (ioctl(uinput_fd, UI_SET_KEYBIT, code) < 0) {
                perror("UI_SET_KEYBIT");
                return -1;
            }
        }
    }

    if (TEST_BIT(EV_ABS, event_bits)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_ABS, sizeof(bridge->abs_bits)),
                  bridge->abs_bits) < 0) {
            perror("configure absolute axes");
            return -1;
        }
        for (int code = 0; code <= ABS_MAX; code++) {
            if (!TEST_BIT(code, bridge->abs_bits)) {
                continue;
            }
            struct input_absinfo info;
            if (ioctl(input_fd, EVIOCGABS(code), &info) < 0) {
                perror("EVIOCGABS");
                return -1;
            }
            if (ioctl(uinput_fd, UI_SET_ABSBIT, code) < 0) {
                perror("UI_SET_ABSBIT");
                return -1;
            }
            struct uinput_abs_setup abs_setup = {
                .code = (uint16_t)code,
                .absinfo = info,
            };
            if (ioctl(uinput_fd, UI_ABS_SETUP, &abs_setup) < 0) {
                perror("UI_ABS_SETUP");
                return -1;
            }
        }
    }

    if (TEST_BIT(EV_MSC, event_bits)) {
        unsigned long msc_bits[NBITS(MSC_MAX + 1)] = {0};
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_MSC) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_MSC, sizeof(msc_bits)), msc_bits) < 0) {
            perror("configure miscellaneous events");
            return -1;
        }
        for (int code = 0; code <= MSC_MAX; code++) {
            if (TEST_BIT(code, msc_bits) && ioctl(uinput_fd, UI_SET_MSCBIT, code) < 0) {
                perror("UI_SET_MSCBIT");
                return -1;
            }
        }
    }

    return 0;
}

static int create_virtual_device(struct bridge *bridge)
{
    bridge->uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (bridge->uinput_fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    struct input_id source_id = {0};
    if (set_event_capabilities(bridge) < 0) {
        goto fail;
    }
    if (ioctl(bridge->input_fd, EVIOCGID, &source_id) < 0) {
        perror("EVIOCGID");
        goto fail;
    }

    struct uinput_setup setup = {0};
    setup.id = source_id;
    setup.id.bustype = BUS_VIRTUAL;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, VIRTUAL_DEVICE_NAME);

    if (ioctl(bridge->uinput_fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(bridge->uinput_fd, UI_DEV_CREATE) < 0) {
        perror("create uinput device");
        goto fail;
    }
    return 0;

fail:
    close(bridge->uinput_fd);
    bridge->uinput_fd = -1;
    return -1;
}

static int write_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event = {
        .type = type,
        .code = code,
        .value = value,
    };
    ssize_t written = write(fd, &event, sizeof(event));
    if (written != (ssize_t)sizeof(event)) {
        if (written < 0) {
            perror("write uinput event");
        } else {
            fprintf(stderr, "short write to uinput\n");
        }
        return -1;
    }
    return 0;
}

static void log_result(uint16_t axis, struct filter_result result)
{
    const char *name = axis == ABS_X ? "ABS_X" : "ABS_Y";
    if (result.gate_started) {
        fprintf(stderr, "%s: reversal gate started\n", name);
    }
    if (result.gate_discarded) {
        fprintf(stderr, "%s: transient discarded\n", name);
    }
    if (result.gate_committed) {
        fprintf(stderr, "%s: sustained reversal committed\n", name);
    }
    if (result.snap_started) {
        fprintf(stderr, "%s: ballistic snapback suppression started\n", name);
    }
    if (result.snap_discarded) {
        fprintf(stderr, "%s: ballistic snapback discarded\n", name);
    }
    if (result.snap_committed) {
        fprintf(stderr, "%s: sustained ballistic reversal committed\n", name);
    }
}

static int emit_filter_result(struct bridge *bridge,
                              uint16_t axis,
                              struct filter_result result)
{
    if (bridge->verbose) {
        log_result(axis, result);
    }
    if (result.emit) {
        return write_event(bridge->uinput_fd, EV_ABS, axis, result.value);
    }
    return 0;
}

static int filter_axis(struct bridge *bridge, uint16_t axis, int value, uint64_t now_ns)
{
    struct axis_filter *filter = axis == ABS_X ? &bridge->x : &bridge->y;
    return emit_filter_result(bridge, axis, axis_filter_input(filter, value, now_ns));
}

static int forward_event(struct bridge *bridge, const struct input_event *event)
{
    if (event->type == EV_ABS && (event->code == ABS_X || event->code == ABS_Y)) {
        return filter_axis(bridge, event->code, event->value,
                           event_time_ns(bridge, event));
    }
    if (event->type == EV_KEY) {
        if (!is_controller_button(event->code) || event->code > KEY_MAX) {
            /* Ignore the malformed Bluetooth keyboard-key reports. */
            return 0;
        }
        if (event->value) {
            SET_BIT(event->code, bridge->keys_down);
        } else {
            CLEAR_BIT(event->code, bridge->keys_down);
        }
    }
    if (event->type == EV_FF || event->type == EV_UINPUT) {
        return 0;
    }
    return write_event(bridge->uinput_fd, event->type, event->code, event->value);
}

/*
 * After SYN_DROPPED the kernel has discarded events, so read the device's
 * current state and send whatever differs.  Otherwise a button release could
 * be lost and leave the button held on the virtual controller.
 */
static int resync(struct bridge *bridge)
{
    unsigned long key_state[NBITS(KEY_MAX + 1)] = {0};
    if (ioctl(bridge->input_fd, EVIOCGKEY(sizeof(key_state)), key_state) < 0) {
        perror("EVIOCGKEY");
        return -1;
    }
    for (int code = BTN_MISC; code <= KEY_MAX; code++) {
        if (!TEST_BIT(code, bridge->key_bits)) {
            continue;
        }
        bool down = TEST_BIT(code, key_state);
        if (down == (bool)TEST_BIT(code, bridge->keys_down)) {
            continue;
        }
        if (down) {
            SET_BIT(code, bridge->keys_down);
        } else {
            CLEAR_BIT(code, bridge->keys_down);
        }
        if (write_event(bridge->uinput_fd, EV_KEY, (uint16_t)code, down) < 0) {
            return -1;
        }
    }

    uint64_t now_ns = advance_clock(bridge, monotonic_ns());
    for (int code = 0; code <= ABS_MAX; code++) {
        if (!TEST_BIT(code, bridge->abs_bits)) {
            continue;
        }
        struct input_absinfo info;
        if (ioctl(bridge->input_fd, EVIOCGABS(code), &info) < 0) {
            perror("EVIOCGABS");
            return -1;
        }
        int status = code == ABS_X || code == ABS_Y
            ? filter_axis(bridge, (uint16_t)code, info.value, now_ns)
            : write_event(bridge->uinput_fd, EV_ABS, (uint16_t)code, info.value);
        if (status < 0) {
            return -1;
        }
    }

    if (bridge->verbose) {
        fprintf(stderr, "Events were dropped; resynchronized controller state\n");
    }
    return write_event(bridge->uinput_fd, EV_SYN, SYN_REPORT, 0);
}

static uint64_t next_deadline(const struct bridge *bridge)
{
    uint64_t x_deadline = axis_filter_deadline(&bridge->x);
    uint64_t y_deadline = axis_filter_deadline(&bridge->y);
    if (x_deadline && y_deadline) {
        return x_deadline < y_deadline ? x_deadline : y_deadline;
    }
    return x_deadline ? x_deadline : y_deadline;
}

static int run_ticks(struct bridge *bridge, uint64_t now_ns)
{
    struct filter_result x_tick = axis_filter_tick(&bridge->x, now_ns);
    struct filter_result y_tick = axis_filter_tick(&bridge->y, now_ns);
    if (emit_filter_result(bridge, ABS_X, x_tick) < 0 ||
        emit_filter_result(bridge, ABS_Y, y_tick) < 0) {
        return -1;
    }
    if (x_tick.emit || y_tick.emit) {
        return write_event(bridge->uinput_fd, EV_SYN, SYN_REPORT, 0);
    }
    return 0;
}

/* Returns 0 when stopped by a signal or disconnect, 1 on error. */
static int run(struct bridge *bridge, const sigset_t *wait_mask)
{
    bool dropping = false;

    while (running) {
        uint64_t now_ns = advance_clock(bridge, monotonic_ns());
        if (run_ticks(bridge, now_ns) < 0) {
            return 1;
        }

        struct timespec timeout;
        struct timespec *timeout_ptr = NULL;
        uint64_t deadline = next_deadline(bridge);
        if (deadline) {
            uint64_t remaining_ns = deadline > now_ns ? deadline - now_ns : 0;
            timeout.tv_sec = (time_t)(remaining_ns / 1000000000ULL);
            timeout.tv_nsec = (long)(remaining_ns % 1000000000ULL);
            timeout_ptr = &timeout;
        }

        struct pollfd descriptor = {.fd = bridge->input_fd, .events = POLLIN};
        int ready = ppoll(&descriptor, 1, timeout_ptr, wait_mask);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return 1;
        }
        if (ready == 0) {
            continue;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Controller disconnected.\n");
            return 0;
        }

        struct input_event events[64];
        ssize_t bytes = read(bridge->input_fd, events, sizeof(events));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            if (errno == ENODEV) {
                fprintf(stderr, "Controller disconnected.\n");
                return 0;
            }
            perror("read controller");
            return 1;
        }

        size_t count = (size_t)bytes / sizeof(events[0]);
        for (size_t i = 0; i < count; i++) {
            const struct input_event *event = &events[i];
            bool is_report = event->type == EV_SYN && event->code == SYN_REPORT;
            if (dropping) {
                if (is_report) {
                    dropping = false;
                    if (resync(bridge) < 0) {
                        return 1;
                    }
                }
                continue;
            }
            if (event->type == EV_SYN && event->code == SYN_DROPPED) {
                dropping = true;
                continue;
            }
            if (forward_event(bridge, event) < 0) {
                return 1;
            }
        }
    }
    return 0;
}

static bool parse_number(const char *text, double *value)
{
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

static void usage(const char *program)
{
    struct filter_config defaults;
    filter_config_defaults(&defaults);
    fprintf(stderr,
            "Usage: %s [options] /dev/input/eventN\n"
            "\n"
            "Options (defaults in parentheses):\n"
            "  --deadzone-percent N    Center deadzone (%g)\n"
            "  --strong-percent N      Deflection that arms snapback detection (%g)\n"
            "  --crossing-percent N    Jump in one sample that counts as a fast crossing (%g)\n"
            "  --commit-percent N      Rebound size that passes immediately (%g)\n"
            "  --gate-ms N             Confirmation time for slower reversals (%g)\n"
            "  --crossing-window-ms N  Max time between samples for a fast crossing (%g)\n"
            "  --neutral-reset-ms N    Time at center that disarms detection (%g)\n"
            "  --snap-partial-ms N     Hold time before a steady partial reversal passes (%g)\n"
            "  --snap-hard-ms N        Max hold time for a suspected snapback (%g)\n"
            "  --verbose               Log filter decisions\n"
            "  --help                  Show this help\n",
            program,
            defaults.deadzone_percent, defaults.strong_percent,
            defaults.crossing_percent, defaults.commit_percent,
            defaults.gate_ms, defaults.crossing_window_ms,
            defaults.neutral_reset_ms, defaults.snap_partial_ms,
            defaults.snap_hard_ms);
}

enum {
    OPT_DEADZONE = 256,
    OPT_STRONG,
    OPT_CROSSING,
    OPT_COMMIT,
    OPT_GATE,
    OPT_CROSSING_WINDOW,
    OPT_NEUTRAL_RESET,
    OPT_SNAP_PARTIAL,
    OPT_SNAP_HARD,
};

int main(int argc, char **argv)
{
    struct filter_config config;
    filter_config_defaults(&config);
    bool verbose = false;

    static const struct option options[] = {
        {"deadzone-percent", required_argument, NULL, OPT_DEADZONE},
        {"strong-percent", required_argument, NULL, OPT_STRONG},
        {"crossing-percent", required_argument, NULL, OPT_CROSSING},
        {"commit-percent", required_argument, NULL, OPT_COMMIT},
        {"gate-ms", required_argument, NULL, OPT_GATE},
        {"crossing-window-ms", required_argument, NULL, OPT_CROSSING_WINDOW},
        {"neutral-reset-ms", required_argument, NULL, OPT_NEUTRAL_RESET},
        {"snap-partial-ms", required_argument, NULL, OPT_SNAP_PARTIAL},
        {"snap-hard-ms", required_argument, NULL, OPT_SNAP_HARD},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "vh", options, NULL)) != -1) {
        double *target = NULL;
        switch (option) {
        case OPT_DEADZONE: target = &config.deadzone_percent; break;
        case OPT_STRONG: target = &config.strong_percent; break;
        case OPT_CROSSING: target = &config.crossing_percent; break;
        case OPT_COMMIT: target = &config.commit_percent; break;
        case OPT_GATE: target = &config.gate_ms; break;
        case OPT_CROSSING_WINDOW: target = &config.crossing_window_ms; break;
        case OPT_NEUTRAL_RESET: target = &config.neutral_reset_ms; break;
        case OPT_SNAP_PARTIAL: target = &config.snap_partial_ms; break;
        case OPT_SNAP_HARD: target = &config.snap_hard_ms; break;
        case 'v': verbose = true; continue;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
        if (!parse_number(optarg, target)) {
            fprintf(stderr, "%s: invalid number '%s'\n", argv[0], optarg);
            return 2;
        }
    }

    if (optind + 1 != argc) {
        usage(argv[0]);
        return 2;
    }
    const char *problem = filter_config_validate(&config);
    if (problem) {
        fprintf(stderr, "%s: invalid settings: %s\n", argv[0], problem);
        return 2;
    }

    struct bridge bridge = {.input_fd = -1, .uinput_fd = -1, .verbose = verbose};
    const char *device_path = argv[optind];
    bridge.input_fd = open(device_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (bridge.input_fd < 0) {
        perror(device_path);
        fprintf(stderr, "Try running the manual test with sudo if access is denied.\n");
        return 1;
    }

    char device_name[256] = "unknown";
    ioctl(bridge.input_fd, EVIOCGNAME(sizeof(device_name)), device_name);

    struct input_id source_id = {0};
    if (ioctl(bridge.input_fd, EVIOCGID, &source_id) < 0) {
        perror("EVIOCGID");
        close(bridge.input_fd);
        return 1;
    }
    bool supported_usb = source_id.bustype == BUS_USB &&
        source_id.vendor == 0x045e && source_id.product == 0x0b00;
    bool supported_bluetooth = source_id.bustype == BUS_BLUETOOTH &&
        source_id.vendor == 0x045e && source_id.product == 0x0b22;
    if (!supported_usb && !supported_bluetooth) {
        fprintf(stderr,
                "Refusing unexpected device: %s (bus=%04x vendor=%04x product=%04x)\n",
                device_name, source_id.bustype, source_id.vendor, source_id.product);
        close(bridge.input_fd);
        return 1;
    }

    struct input_absinfo x_info;
    struct input_absinfo y_info;
    if (ioctl(bridge.input_fd, EVIOCGABS(ABS_X), &x_info) < 0 ||
        ioctl(bridge.input_fd, EVIOCGABS(ABS_Y), &y_info) < 0) {
        perror("read left-stick axis information");
        close(bridge.input_fd);
        return 1;
    }
    axis_filter_init(&bridge.x, x_info.minimum, x_info.maximum, x_info.value, &config);
    axis_filter_init(&bridge.y, y_info.minimum, y_info.maximum, y_info.value, &config);

    if (create_virtual_device(&bridge) < 0) {
        close(bridge.input_fd);
        return 1;
    }

    if (ioctl(bridge.input_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        ioctl(bridge.uinput_fd, UI_DEV_DESTROY);
        close(bridge.uinput_fd);
        close(bridge.input_fd);
        return 1;
    }

    int clock_id = CLOCK_MONOTONIC;
    bridge.event_clock = ioctl(bridge.input_fd, EVIOCSCLOCKID, &clock_id) == 0;

    /*
     * Keep SIGINT and SIGTERM blocked except while waiting in ppoll, so a
     * signal can't slip in between checking `running` and going to sleep.
     */
    sigset_t blocked;
    sigset_t wait_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &wait_mask);
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGTERM);

    struct sigaction action = {.sa_handler = stop_running};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    fprintf(stderr,
            "Filtering %s as '" VIRTUAL_DEVICE_NAME "'\n"
            "deadzone=%g%% strong=%g%% crossing=%g%% commit=%g%% gate=%gms "
            "window=%gms reset=%gms snap=%g/%gms; Ctrl-C stops safely\n",
            device_path, config.deadzone_percent, config.strong_percent,
            config.crossing_percent, config.commit_percent, config.gate_ms,
            config.crossing_window_ms, config.neutral_reset_ms,
            config.snap_partial_ms, config.snap_hard_ms);

    int status = run(&bridge, &wait_mask);

    fprintf(stderr,
            "Stopped. X gates: %llu/%llu/%llu, snap: %llu/%llu/%llu; "
            "Y gates: %llu/%llu/%llu, snap: %llu/%llu/%llu "
            "(started/discarded/committed)\n",
            (unsigned long long)bridge.x.gates_started,
            (unsigned long long)bridge.x.gates_discarded,
            (unsigned long long)bridge.x.gates_committed,
            (unsigned long long)bridge.x.snap_started,
            (unsigned long long)bridge.x.snap_discarded,
            (unsigned long long)bridge.x.snap_committed,
            (unsigned long long)bridge.y.gates_started,
            (unsigned long long)bridge.y.gates_discarded,
            (unsigned long long)bridge.y.gates_committed,
            (unsigned long long)bridge.y.snap_started,
            (unsigned long long)bridge.y.snap_discarded,
            (unsigned long long)bridge.y.snap_committed);

    ioctl(bridge.input_fd, EVIOCGRAB, 0);
    ioctl(bridge.uinput_fd, UI_DEV_DESTROY);
    close(bridge.uinput_fd);
    close(bridge.input_fd);
    return status;
}
