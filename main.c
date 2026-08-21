#define _GNU_SOURCE

#include "filter.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <linux/uinput.h>
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

static volatile sig_atomic_t running = 1;

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

static int set_event_capabilities(int input_fd, int uinput_fd)
{
    unsigned long event_bits[NBITS(EV_MAX + 1)] = {0};
    unsigned long key_bits[NBITS(KEY_MAX + 1)] = {0};
    unsigned long abs_bits[NBITS(ABS_MAX + 1)] = {0};

    if (ioctl(input_fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0) {
        perror("EVIOCGBIT events");
        return -1;
    }

    if (TEST_BIT(EV_KEY, event_bits)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            perror("configure keys");
            return -1;
        }
        for (int code = BTN_MISC; code <= KEY_MAX; code++) {
            if (TEST_BIT(code, key_bits) && ioctl(uinput_fd, UI_SET_KEYBIT, code) < 0) {
                perror("UI_SET_KEYBIT");
                return -1;
            }
        }
    }

    if (TEST_BIT(EV_ABS, event_bits)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
            perror("configure absolute axes");
            return -1;
        }
        for (int code = 0; code <= ABS_MAX; code++) {
            if (!TEST_BIT(code, abs_bits)) {
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

static int create_virtual_device(int input_fd)
{
    int uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    if (set_event_capabilities(input_fd, uinput_fd) < 0) {
        close(uinput_fd);
        return -1;
    }

    struct input_id source_id = {0};
    if (ioctl(input_fd, EVIOCGID, &source_id) < 0) {
        perror("EVIOCGID");
        close(uinput_fd);
        return -1;
    }

    struct uinput_setup setup = {0};
    setup.id = source_id;
    setup.id.bustype = BUS_VIRTUAL;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "Filtered Xbox Elite Series 2");

    if (ioctl(uinput_fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("create uinput device");
        close(uinput_fd);
        return -1;
    }

    return uinput_fd;
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

static int emit_filter_result(int uinput_fd,
                              uint16_t axis,
                              struct filter_result result,
                              bool verbose)
{
    if (verbose && result.gate_started) {
        fprintf(stderr, "%s: reversal gate started\n",
                axis == ABS_X ? "ABS_X" : "ABS_Y");
    }
    if (verbose && result.gate_discarded) {
        fprintf(stderr, "%s: transient discarded\n",
                axis == ABS_X ? "ABS_X" : "ABS_Y");
    }
    if (verbose && result.gate_committed) {
        fprintf(stderr, "%s: sustained reversal committed\n",
                axis == ABS_X ? "ABS_X" : "ABS_Y");
    }
    if (verbose && result.snap_started) {
        fprintf(stderr, "%s: ballistic snapback suppression started\n",
                axis == ABS_X ? "ABS_X" : "ABS_Y");
    }
    if (verbose && result.snap_discarded) {
        fprintf(stderr, "%s: ballistic snapback discarded\n",
                axis == ABS_X ? "ABS_X" : "ABS_Y");
    }
    if (verbose && result.snap_committed) {
        fprintf(stderr, "%s: sustained ballistic reversal committed\n",
                axis == ABS_X ? "ABS_X" : "ABS_Y");
    }
    if (result.emit) {
        return write_event(uinput_fd, EV_ABS, axis, result.value);
    }
    return 0;
}

static int timeout_ms_for_filters(const struct axis_filter *x,
                                  const struct axis_filter *y,
                                  uint64_t now_ns)
{
    uint64_t x_deadline = axis_filter_deadline(x);
    uint64_t y_deadline = axis_filter_deadline(y);
    uint64_t deadline = 0;
    if (x_deadline && y_deadline) {
        deadline = x_deadline < y_deadline ? x_deadline : y_deadline;
    } else {
        deadline = x_deadline ? x_deadline : y_deadline;
    }
    if (!deadline) {
        return -1;
    }
    if (deadline <= now_ns) {
        return 0;
    }
    uint64_t remaining_ns = deadline - now_ns;
    return (int)((remaining_ns + 999999ULL) / 1000000ULL);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options] /dev/input/eventN\n"
            "\n"
            "Options:\n"
            "  --gate-ms N           Opposite-direction confirmation time (default: 13)\n"
            "  --deadzone-percent N  Center deadzone percentage (default: 4)\n"
            "  --strong-percent N    Prior deflection needed to arm gate (default: 70)\n"
            "  --verbose             Log gate decisions\n"
            "  --help                Show this help\n",
            program);
}

int main(int argc, char **argv)
{
    double gate_ms = 13.0;
    double deadzone_percent = 4.0;
    double strong_percent = 70.0;
    bool verbose = false;

    static const struct option options[] = {
        {"gate-ms", required_argument, NULL, 'g'},
        {"deadzone-percent", required_argument, NULL, 'd'},
        {"strong-percent", required_argument, NULL, 's'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "g:d:s:vh", options, NULL)) != -1) {
        switch (option) {
        case 'g': gate_ms = strtod(optarg, NULL); break;
        case 'd': deadzone_percent = strtod(optarg, NULL); break;
        case 's': strong_percent = strtod(optarg, NULL); break;
        case 'v': verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    if (optind + 1 != argc || gate_ms < 0.0 || deadzone_percent < 0.0 ||
        deadzone_percent >= 100.0 || strong_percent <= 0.0 ||
        strong_percent > 100.0) {
        usage(argv[0]);
        return 2;
    }

    const char *device_path = argv[optind];
    int input_fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) {
        perror(device_path);
        fprintf(stderr, "Try running the manual test with sudo if access is denied.\n");
        return 1;
    }

    char device_name[256] = "unknown";
    ioctl(input_fd, EVIOCGNAME(sizeof(device_name)), device_name);

    struct input_id source_id = {0};
    if (ioctl(input_fd, EVIOCGID, &source_id) < 0) {
        perror("EVIOCGID");
        close(input_fd);
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
        close(input_fd);
        return 1;
    }

    struct input_absinfo x_info;
    struct input_absinfo y_info;
    if (ioctl(input_fd, EVIOCGABS(ABS_X), &x_info) < 0 ||
        ioctl(input_fd, EVIOCGABS(ABS_Y), &y_info) < 0) {
        perror("read left-stick axis information");
        close(input_fd);
        return 1;
    }

    struct axis_filter x_filter;
    struct axis_filter y_filter;
    axis_filter_init(&x_filter, x_info.minimum, x_info.maximum, x_info.value,
                     deadzone_percent, strong_percent, gate_ms);
    axis_filter_init(&y_filter, y_info.minimum, y_info.maximum, y_info.value,
                     deadzone_percent, strong_percent, gate_ms);

    int uinput_fd = create_virtual_device(input_fd);
    if (uinput_fd < 0) {
        close(input_fd);
        return 1;
    }

    if (ioctl(input_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        close(input_fd);
        return 1;
    }

    int clock_id = CLOCK_MONOTONIC;
    ioctl(input_fd, EVIOCSCLOCKID, &clock_id);

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);

    fprintf(stderr,
            "Filtering %s as 'Filtered Xbox Elite Series 2'\n"
            "gate=%.1f ms deadzone=%.1f%% strong=%.1f%%; Ctrl-C stops safely\n",
            device_path, gate_ms, deadzone_percent, strong_percent);

    while (running) {
        uint64_t now_ns = monotonic_ns();
        struct filter_result x_tick = axis_filter_tick(&x_filter, now_ns);
        struct filter_result y_tick = axis_filter_tick(&y_filter, now_ns);
        bool tick_emitted = x_tick.emit || y_tick.emit;
        if (emit_filter_result(uinput_fd, ABS_X, x_tick, verbose) < 0 ||
            emit_filter_result(uinput_fd, ABS_Y, y_tick, verbose) < 0) {
            break;
        }
        if (tick_emitted && write_event(uinput_fd, EV_SYN, SYN_REPORT, 0) < 0) {
            break;
        }

        int timeout_ms = timeout_ms_for_filters(&x_filter, &y_filter, now_ns);
        struct pollfd descriptor = {.fd = input_fd, .events = POLLIN};
        int ready = poll(&descriptor, 1, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Controller disconnected.\n");
            break;
        }

        struct input_event events[64];
        ssize_t bytes = read(input_fd, events, sizeof(events));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            perror("read controller");
            break;
        }

        size_t count = (size_t)bytes / sizeof(events[0]);
        for (size_t i = 0; i < count; i++) {
            struct input_event event = events[i];
            if (event.type == EV_ABS && event.code == ABS_X) {
                struct filter_result result = axis_filter_input(
                    &x_filter, event.value, monotonic_ns());
                if (emit_filter_result(uinput_fd, ABS_X, result, verbose) < 0) {
                    running = 0;
                    break;
                }
            } else if (event.type == EV_ABS && event.code == ABS_Y) {
                struct filter_result result = axis_filter_input(
                    &y_filter, event.value, monotonic_ns());
                if (emit_filter_result(uinput_fd, ABS_Y, result, verbose) < 0) {
                    running = 0;
                    break;
                }
            } else if (event.type == EV_KEY &&
                       !is_controller_button(event.code)) {
                /* Ignore the malformed Bluetooth keyboard-key reports. */
                continue;
            } else if (event.type != EV_FF && event.type != EV_UINPUT) {
                if (write_event(uinput_fd, event.type, event.code, event.value) < 0) {
                    running = 0;
                    break;
                }
            }
        }
    }

    fprintf(stderr,
            "Stopped. X gates: %llu/%llu/%llu, snap: %llu/%llu/%llu; "
            "Y gates: %llu/%llu/%llu, snap: %llu/%llu/%llu "
            "(started/discarded/committed)\n",
            (unsigned long long)x_filter.gates_started,
            (unsigned long long)x_filter.gates_discarded,
            (unsigned long long)x_filter.gates_committed,
            (unsigned long long)x_filter.snap_started,
            (unsigned long long)x_filter.snap_discarded,
            (unsigned long long)x_filter.snap_committed,
            (unsigned long long)y_filter.gates_started,
            (unsigned long long)y_filter.gates_discarded,
            (unsigned long long)y_filter.gates_committed,
            (unsigned long long)y_filter.snap_started,
            (unsigned long long)y_filter.snap_discarded,
            (unsigned long long)y_filter.snap_committed);

    ioctl(input_fd, EVIOCGRAB, 0);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(input_fd);
    return 0;
}
