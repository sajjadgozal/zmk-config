# media_controller

A ZMK-based media remote / mini pointing device: 3 buttons (play-pause /
next / prev — next/prev become Home/Back while connected to the TV, see
"Output modes"), an encoder push button bound to mute, a rotary encoder
(volume on turn), a mode-toggle button, and a joystick — built for a
Pro-Micro-footprint nRF52840 board (target: "V1940 Pro Micro nRF52840",
flashed as `nice_nano//zmk` since it shares the nice!nano v2 pinout).
Works over BLE (pairs as a standard HID keyboard/consumer-control/mouse
device — no drivers needed on macOS or Bluetooth-capable TVs) or over a
USB cable, switchable at any time.

Two keymap layers, swapped with the mode-toggle button, change what the
joystick does — everything else (play/pause/next/prev/mute/volume) is
identical in both:

- **Mouse mode** (default): joystick tilt moves the cursor, joystick
  push = left-click.
- **Arrow mode**: joystick tilt sends arrow-key presses (held while
  tilted, released when centered), joystick push = Enter.

See "Modes" below for details.

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
  as its own Zephyr module) polling the joystick's two ADC axes. Reports
  relative mouse movement by default once deflection passes a deadzone;
  while the devicetree's `arrow-layer` (default `1`) is the active ZMK
  keymap layer, it instead sends held arrow-key presses — see "Modes"
  below. Tune `deadzone`/`sensitivity` in the overlay's `joystick` node
  to taste.
- `build.yaml` — tells GitHub Actions which board+shield to build; also
  has a second debug-logging build entry, see "Debugging over USB serial"

## Pin layout (as soldered)

| Signal | pro_micro index | Silkscreen label |
|---|---|---|
| play/pause | 16 (P0.10) | D16 |
| next (Home on TV profile) | 10 (P0.09) | D10 |
| prev (Back on TV profile) | 21 (P0.31) | A3 |
| mode-toggle | 3 | D3 |
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
   don't expect BLE-side effects to be visible while debugging over
   this same USB cable unless you select a profile with its combo
   (which switches output to Bluetooth).
5. This is a temporary debug artifact - once you've diagnosed the issue,
   remove the second entry from `build.yaml` (or just keep using the
   normal artifact for everyday flashing; the debug one is only for
   troubleshooting sessions like this).

## Output modes

Three ways to connect, each picked with a combo:

| Combo | Output | next / prev buttons |
|---|---|---|
| hold `prev` + `play/pause` | Bluetooth profile 0 (Mac) | media next / previous track |
| hold `prev` + `next` | Bluetooth profile 1 (Android TV) | Home / Back (mouse button 4) |
| hold `play/pause` + `next` + joystick push | USB cable | media next / previous track |

- The two Bluetooth combos also switch output to Bluetooth, so they work
  even with a cable plugged in (otherwise ZMK keeps sending over USB
  whenever a cable is connected).
- The next/prev → Home/Back switch is automatic: `src/endpoint_layer.c`
  turns the keymap's `tv_layer` on only while Bluetooth profile 1 is the
  active, connected output (`CONFIG_ZMK_ENDPOINT_LAYER` in
  `media_controller.conf`).
- **First pairing:** select the profile with its combo, then pair from the
  Mac's System Settings → Bluetooth or the TV's Bluetooth menu ("Media
  Remote"). ZMK remembers both; just repeat a combo to switch later.
- **Clear a broken pairing:** hold `play/pause` + `next` + the
  mode-toggle button to forget the currently active profile's pairing.

## Modes

The mode-toggle button (`&tog 1` in the keymap) persistently switches
between two layers — press once to switch, press again to switch back;
it stays until you toggle it again, no holding required.

- **Mouse mode** (layer 0, default at boot): joystick tilt moves the
  mouse cursor at constant speed while held past the deadzone; joystick
  push = left-click **and** Enter together (`click_or_enter` macro) — see
  "Known Android limitations" below for why.
- **Arrow mode** (layer 1): joystick tilt sends a held arrow-key press
  per axis (release when centered — so holding right continuously
  repeats "right arrow" the way holding a real arrow key does); joystick
  push = Enter.

## Known Android limitations

Testing on an Android TV turned up two host-side quirks that aren't
fixable purely from this firmware:

- **Mouse-button clicks over BLE HID don't register at all**, while
  relative movement works fine — a known, widely-reported Android
  limitation, not specific to this device. Worked around by having
  joystick push send an Enter keypress alongside the click (see
  `click_or_enter` in the keymap) — Enter is a keyboard-page key, which
  *does* work.
- **Consumer "Media Select Home" (0x9A) doesn't trigger Home** on this
  TV, despite being purpose-built for exactly this. The usage Android's
  own remotes actually use for Home is "AC Home" (0x223) — but that
  requires switching `CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES` from
  `_BASIC` to `_FULL`, which is what broke play/pause/mute/volume/back
  in the first place (see the Basic-usage fix earlier in this file's
  history). Whether Full mode with *only* AC Home added back-breaks
  everything else again hasn't been tested — if you want a working Home
  button, that's the next experiment: temporarily switch to `_FULL`,
  rebind `home` to `C_AC_HOME`, and see whether the other consumer keys
  survive. If they don't, there's currently no known working Home
  button over generic BLE HID for this TV.

Everything else — play/pause, next/prev, mute, volume, and the
Bluetooth/output combos — behaves identically in both modes; only the
joystick's behavior changes. The switch happens inside the joystick
driver itself (it checks whether keymap layer 1 is active), not through
a kscan binding, since the joystick isn't a keymap position.

To change which layer index the joystick treats as "arrow mode," or to
add more layers of your own, edit `arrow-layer` in the overlay's
`joystick` node and keep it in sync with the layer's position in
`media_controller.keymap`.

## Controls

| Input | Action (mouse mode) | Action (arrow mode) |
|---|---|---|
| `play/pause` button | Play / pause | Play / pause |
| `next` button | Next track (TV profile: Home, unreliable - see above) | same |
| `prev` button | Previous track (TV profile: Back, sent as mouse button 4) | same |
| Encoder turn | Volume up / down | Volume up / down |
| Encoder push | Mute | Mute |
| Mode-toggle button | Switch to arrow mode | Switch to mouse mode |
| Joystick tilt | Move mouse cursor | Arrow-key presses |
| Joystick push | Left click + Enter | Enter |

## First-time bring-up / testing

Test incrementally rather than wiring everything and hoping — each stage
below is independently checkable:

1. **Flash with nothing wired yet.** Confirm the board powers up, enters
   bootloader mode on double-tap-reset, and boots the new firmware
   without crashing (an LED blink pattern or just staying enumerated over
   USB is enough evidence — see Flashing above).
2. **Buttons first.** Wire just `play/pause`/`next`/`prev`. On macOS,
   open any media app (Music, Spotify, a YouTube tab) and press each
   button; play/pause and track changes should respond immediately. If a button
   does nothing at all anywhere, double check it's on the pin the
   overlay expects and that it's wired to *ground* (these use
   `GPIO_ACTIVE_LOW` + internal pull-up, so a press should short the pin
   to GND).
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
   pairing combos (see Output modes above) *unplugged* — pair to macOS on
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
