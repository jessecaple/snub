# Xbox Elite Series 2 snapback filter

This is an experimental Linux `evdev` to `uinput` filter for the measured
left-stick snapback on this controller. It creates a virtual controller and
exclusively captures the physical controller while running.

The same symmetric filter is applied independently to left-stick X and Y, so
it covers left-to-right, right-to-left, up-to-down, and down-to-up snapback.

The first test configuration uses:

- a 13 ms opposite-direction confirmation gate;
- a 6% center deadzone, chosen to cover the measured resting offset;
- gate arming only after at least 70% stick deflection.

Fast sign crossings are classified using their sample-to-sample velocity,
preserving the last direction across a brief center-zone crossing. A rebound
below 80% is held at neutral until it settles. A deliberate reversal that
continues beyond 80% is passed immediately rather than waiting for a fixed
multi-frame window.

It does not modify controller firmware. It can be run manually or installed as
a per-user service after testing.

## Build

```bash
make test
make
```

## Manual test

Connect the Elite Series 2 over USB or Bluetooth, close any game, and run:

```bash
./run.sh
```

If opening the physical event device is denied, use:

```bash
sudo ./run.sh
```

Leave the filter running, start the game, and test the left stick. Press
`Ctrl-C` in the terminal to restore the unfiltered physical controller.

The test values can be overridden without editing code, for example:

```bash
GATE_MS=13 DEADZONE_PERCENT=6 ./run.sh
```

Rumble is intentionally not exposed by the virtual controller in this first
version.

## Permanent installation

Run:

```bash
./install.sh
```

Connect the controller over Bluetooth before running the installer. It detects
the controller's Bluetooth address and uses it only in the generated rule under
`/etc/udev/rules.d`; the address is never written into this source directory.
The installer then builds and tests the filter, installs its runtime under
`~/.local/libexec`, enables a systemd user service, and cleans the build tree.
The service starts with the desktop session, waits quietly while the controller
is off, supports either USB or Bluetooth, and restarts after a disconnect. Turn
the controller off and on once after installation so the generated rule applies
to newly created device nodes.

Useful commands:

```bash
systemctl --user status xbox-snapback-filter.service
journalctl --user -u xbox-snapback-filter.service
./uninstall.sh
```

## Bluetooth access

Fedora currently classifies this controller's Bluetooth event node partly as a
keyboard because its HID descriptor advertises a malformed keyboard-key
bitmap. The filter exposes only the real controller-button range on its virtual
device, allowing udev and Steam to recognize it as a joystick. Fedora does not
grant the desktop user access to the physical event node by default. The
included rule template matches the Bluetooth name and the unique address
detected during installation. It is generated under `/etc/udev/rules.d/` to
grant the active desktop user access without granting access to keyboards or
other input devices. It also removes Steam's direct `hidraw` access to Bluetooth
Xbox product `045e:0b22`; otherwise Steam could bypass the evdev filter. The
virtual controller has no `hidraw` node and remains available normally.
