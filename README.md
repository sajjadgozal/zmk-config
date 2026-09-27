# media_controller

A ZMK-based media remote / mini pointing device: 3 buttons (play-pause /
next / prev), an encoder push button bound to mute, a rotary encoder
(volume on turn), a spare unbound button, and a joystick (cursor
movement + left-click) — built for a Pro-Micro-footprint nRF52840 board
(target: "V1940 Pro Micro nRF52840", flashed as `nice_nano//zmk` since it
shares the nice!nano v2 pinout). Works over BLE (pairs as a standard HID
keyboard/consumer-control/mouse device — no drivers needed on macOS or
Bluetooth-capable TVs) or over a USB cable, switchable at any time.

## Repo layout

- `config/west.yml` — points at upstream ZMK `main`. (Pinning to the last
  tagged release, `v0.3.0`, was tried first but doesn't actually work
  right now: the CI's Docker build image floats to whatever toolchain
  `main` currently needs, so an old pinned Zephyr source hits unrelated
  compiler-internal failures. `main` is the only branch guaranteed to
  match the image, so we stay on it and suppress the one Kconfig warning
  it currently trips - see `CONFIG_WARN_DEPRECATED=n` in
  `media_controller.conf`, needed because ZMK's `main` branch briefly
  treats using its own (still-required) KSCAN subsystem as deprecated
  post-Zephyr-4.1-upgrade.)
- `config/media_controller.keymap` — key bindings + Bluetooth-profile /
  output-toggle combos
- `config/media_controller.conf` — BLE/USB/pointing/power Kconfig options
- `boards/shields/media_controller/` — the custom "shield": which GPIO
  pins the buttons/encoder/joystick are wired to
- `drivers/input/analog_joystick.c` + `dts/bindings/input/zmk,analog-joystick.yaml`
  + `zephyr/module.yml` — a custom Zephyr input driver (this repo doubles
  as its own Zephyr module) polling the joystick's two ADC axes and
  reporting relative mouse movement once deflection passes a deadzone.
  **Not hardware-tested** — expect to tune `deadzone`/`sensitivity` in
  the overlay's `joystick` node once flashed to real hardware.
- `build.yaml` — tells GitHub Actions which board+shield to build; also
  has a second debug-logging build entry, see "Debugging over USB serial"

## Pin layout (as soldered)

| Signal | pro_micro index | Silkscreen label |
|---|---|---|
| play/pause | 16 (P0.10) | D16 |
| next | 10 (P0.09) | D10 |
| prev | 21 (P0.31) | A3 |
| spare (unbound) | 3 | D3 |
| encoder push (mute) | 2 | D2 |
| joystick push (left-click) | 18 (P1.15) | A0 |
| encoder A | 1 | D1 |
| encoder B | 0 | D0 |
| joystick X (ADC) | 19 (AIN0/P0.02) | A1 |
| joystick Y (ADC) | 20 (AIN5/P0.29) | A2 |

Joystick X/Y are true analog readings here — A1/A2 (P0.02/P0.29 =
AIN0/AIN5) are 2 of the board's only 3 SAADC-capable pins (confirmed
against Nordic's official nRF52840 datasheet). play/pause and next moved
onto D10/D16 (P0.09/P0.10, the chip's NFC1/NFC2 antenna pins) to free up
A1/A2 for the joystick — that's electrically fine for plain digital
buttons (`CONFIG_NFCT_PINS_AS_GPIOS=y` makes them usable as GPIO at all).
This is the fix for an earlier revision that had the joystick on the NFC
pins instead — analog reading is physically impossible there, so it had
to fall back to a crude digital-threshold approximation. Real analog
joystick movement needs the fix in this direction (analog-capable pins
for the joystick), not the other way around.

## Building the firmware

Push this repo to GitHub and the included Action
(`.github/workflows/build.yml`) builds a `.uf2` firmware file on every push
— check the Actions tab, download the `firmware` artifact from a successful
run.

To build locally instead, follow ZMK's
["Build and Flash" toolchain setup](https://zmk.dev/docs/development/setup)
then, from the repo root:

```
west init -l config
west update
west build -p -b nice_nano//zmk -- -DSHIELD=media_controller
```

The output `.uf2` will be in `build/zephyr/zmk.uf2`.

## Flashing

1. Double-tap the board's reset button to drop it into UF2 bootloader mode
   (it will show up as a USB mass-storage drive, usually named `NICENANO`).
2. Drag `zmk.uf2` onto that drive. The board reboots automatically running
   the new firmware.

## Debugging over USB serial

`build.yaml` includes a second build entry, `media_controller-debug-log`,
that adds a USB serial console (via ZMK's official `zmk-usb-logging`
snippet) alongside the normal HID interface, with debug-level logging
enabled for sensors (covers the EC11 encoder driver).

1. From a completed GitHub Actions run, download the
   `media_controller-debug-log` artifact instead of the normal one, and
   flash that `.uf2` the same way (see Flashing above).
2. Plug the board into your Mac via USB-C (it works over USB even though
   this is otherwise a BLE-first build).
3. Find the serial device and open a terminal to it:
   ```
   ls /dev/tty.usbmodem*
   screen /dev/tty.usbmodem<whatever showed up> 115200
   ```
   (`Ctrl-A` then `K` to exit `screen` when done.)
4. Watch for `EC11: Delta: ...` lines as you turn the encoder, and
   `kscan_direct_read: Sending event ...` lines as you press buttons.
   Note that whichever BLE profile is selected, ZMK still prefers USB
   as the active output transport whenever a cable is plugged in — so
   don't expect BLE-side effects (like a pairing combo) to be visible
   while debugging over this same USB cable. Test those unplugged.
5. This is a temporary debug artifact - once you've diagnosed the issue,
   remove the second entry from `build.yaml` (or just keep using the
   normal artifact for everyday flashing; the debug one is only for
   troubleshooting sessions like this).

## Pairing

ZMK keeps up to 5 separate Bluetooth pairings ("profiles") and you switch
which one is active — that's how one device talks to both your Mac and
your TV without re-pairing every time.

- **Pair to macOS:** hold `prev` + `next` together (selects profile 0),
  then on the Mac go to System Settings → Bluetooth and pair with
  "Media Remote".
- **Pair to a TV:** hold `prev` + `play/pause` together (selects profile
  1), then pair from the TV's Bluetooth settings menu.
- **Switch between them later:** just repeat the relevant combo — no
  re-pairing needed, ZMK remembers both.
- **Clear a broken pairing:** hold `play/pause` + `next` + the spare
  (unbound) button together to forget the currently active profile's
  pairing, then pair again.
- **Switch USB ↔ Bluetooth:** hold `play/pause` + `next` + the joystick
  push button together to toggle output. Plug in a cable any time you
  want wired/zero-latency mode.

Reminder: these are BLE-side combos, so test them unplugged (or on a
different host) — while a USB cable is connected, ZMK keeps using USB as
the output regardless of which BLE profile is selected.

## Controls

| Input | Action |
|---|---|
| `play/pause` button | Play / pause |
| `next` button | Next track |
| `prev` button | Previous track |
| Encoder turn | Volume up / down |
| Encoder push | Mute |
| Spare button | Unbound (free for a future function) |
| Joystick tilt | Move mouse cursor (analog) |
| Joystick push | Left click |

## First-time bring-up / testing

Test incrementally rather than wiring everything and hoping — each stage
below is independently checkable:

1. **Flash with nothing wired yet.** Confirm the board powers up, enters
   bootloader mode on double-tap-reset, and boots the new firmware
   without crashing (an LED blink pattern or just staying enumerated over
   USB is enough evidence — see Flashing above).
2. **Buttons first.** Wire just `play/pause`/`next`/`prev`. On macOS,
   open any media app (Music, Spotify, a YouTube tab) and press each
   button; you should see play/pause/track-change respond immediately.
   If a button does nothing, double check it's on the pin the overlay
   expects and that it's wired to *ground* (these use `GPIO_ACTIVE_LOW` +
   internal pull-up, so a press should short the pin to GND).
3. **Encoder next.** Wire the encoder A/B pins and its push button. Turn
   it — volume should move in the OS; push should mute. If it moves the
   wrong direction, swap the A/B wires (or swap the two args in
   `sensor-bindings` in the keymap).
4. **Joystick last**, since it's the least tested part of this build.
   Wire X/Y to A1/A2 and the click button to A0. On macOS, watch the
   cursor: it should sit still at rest and move smoothly when tilted. If
   it drifts at rest, increase `deadzone` in the overlay's `joystick`
   node; if too slow/fast, adjust `sensitivity` (lower = faster). Each
   tuning change needs a re-flash.
5. **Bluetooth profile switching.** With everything wired, test the
   pairing combos (see Pairing above) *unplugged* — pair to macOS on
   profile 0, then try the TV/profile 1 combo and confirm it visibly
   disconnects from one and becomes discoverable for the other.
6. **USB fallback.** Plug in a USB-C cable and use the output-toggle
   combo; confirm the device still responds when BLE is out of range or
   off, without needing to re-pair anything.

If a stage fails, isolate it: use the debug-log build (see above) to
check whether firmware sees the input at all before assuming it's a
wiring problem.

## Tuning the joystick

Once flashed, if the cursor drifts at rest, increase `deadzone` in the
`joystick` node in `media_controller.overlay`. If movement feels too
slow/fast, decrease/increase `sensitivity` (it's a divisor — lower means
faster movement). Re-push to trigger a CI rebuild after each change.
