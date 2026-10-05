# SNUB

Snapback Neutralizing Uinput Bridge: a Linux filter for left-stick snapback on
the Xbox Elite Series 2 controller.

On a worn Elite 2, letting go of the left stick after a hard push can make it
bounce past center and register a short flick the other way. SNUB reads the
controller through evdev, holds those rebounds at center, and passes
everything else to a virtual controller. It doesn't touch the controller's
firmware, and stopping SNUB gives you the unfiltered controller back.

Both axes are filtered in both directions. A reversal that goes past 80% of
the stick's travel passes immediately. A smaller one that follows a hard push
is held at center until it settles or proves steady, for at most 120 ms.

It works over USB (`045e:0b00`) and Bluetooth (`045e:0b22`). I use it on Fedora
with Steam. It's experimental.

## Try it

Stop the service if it's installed, close any game, connect the controller,
and run:

```bash
make test && make
./run.sh
```

From a terminal it logs each filter decision. Ctrl-C stops it. If opening the
controller is denied, run `sudo ./run.sh` or install the udev rule below.

## Install

```bash
./install.sh
```

The installer builds and tests SNUB, copies it to `~/.local/libexec/snub`,
installs a udev rule (this step uses sudo), and starts a systemd user service.
The service waits for the controller and restarts after a disconnect.

If you use Bluetooth, connect over Bluetooth before installing. Fedora doesn't
give the desktop user access to the controller's Bluetooth event node, so the
rule grants it for your controller's address. Turn the controller off and on
once afterward.

```bash
systemctl --user status snub.service
journalctl --user -u snub.service
./uninstall.sh
```

## Settings

The installer creates `~/.config/snub/snub.conf`, which lists every tuning
value with a short explanation. The defaults came from one worn controller, so
yours may need different numbers. Watch `./run.sh` in a terminal while you
test, edit the file, then run `systemctl --user restart snub.service`.
Environment variables with the same names override the file, for example
`GATE_MS=15 ./run.sh`.

## Caveats

- Steam loses direct access to every Bluetooth Elite Series 2 on the machine.
  The udev rule removes Steam's hidraw access to `045e:0b22` so Steam can't
  read around the filter. This stays in place until you uninstall, even when
  SNUB isn't running. Steam still sees the filtered controller.
- The udev rule also lets the logged-in desktop user write to `/dev/uinput`,
  which any of their programs can use to create virtual input devices. Steam's
  own udev rules already grant this.
- While SNUB runs it grabs the physical controller, so games see only
  "Filtered Xbox Elite Series 2".
- The virtual controller has no rumble.
- Only the left stick is filtered, and only the first Elite Series 2 found.
- Bluetooth access is tied to one controller's address. Run `./install.sh`
  again after pairing a different one.

## License

MIT
