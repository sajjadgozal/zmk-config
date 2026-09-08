# media_controller

A ZMK-based media remote / mini pointing device: 3 buttons (prev /
play-pause / next), a rotary encoder (volume + mute), and an analog
joystick (cursor movement + left-click) — built for a Pro-Micro-footprint
nRF52840 board (target: "V1940 Pro Micro nRF52840", flashed as
`nice_nano//zmk` since it shares the nice!nano v2 pinout). Works over BLE
(pairs as a standard HID keyboard/consumer-control/mouse device — no
drivers needed on macOS or Bluetooth-capable TVs) or over a USB cable,
switchable at any time.

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
- `boards/shields/media_controller/` — the custom "shield": which GPIO/ADC
  pins the buttons/encoder/joystick are wired to
- `drivers/input/analog_joystick.c` + `dts/bindings/input/zmk,analog-joystick.yaml`
  + `zephyr/module.yml` — a custom Zephyr input driver (this repo doubles
  as its own Zephyr module) that polls the joystick's two ADC axes and
  reports relative mouse movement once deflection passes a deadzone.
  **Not hardware-tested** — expect to tune `deadzone` and `sensitivity` in
  the overlay once it's flashed to real hardware.
- `build.yaml` — tells GitHub Actions which board+shield to build

## Pin layout

| Signal | pro_micro index | Silkscreen label (nice!nano-pinout boards) |
|---|---|---|
| prev | 4 | D4 |
| play/pause | 5 | D5 |
| next | 6 | D6 |
| encoder push (mute) | 7 | D7 |
| joystick push (left-click) | 9 | D9 |
| encoder A | 8 | D8 |
| encoder B | 10 | D10 |
| joystick X | 19 (AIN0) | A1 |
| joystick Y | 20 (AIN5) | A2 |

⚠️ **Verify this against your V1940's actual silkscreen before soldering**
— this table assumes it's pin-compatible with nice!nano (true for most
"Pro Micro nRF52840" clones, but not guaranteed). Two things worth
knowing if you need to change pins:

- Buttons/encoder can go on any free digital GPIO.
- The joystick's X/Y wires **must** land on ADC-capable pins. On this
  chip, only 3 header pins qualify — the ones silkscreened `A1`/`A2`/`A3`.
  The pin labeled `A0` is *not* ADC-capable here (unlike on a real
  Arduino), so don't use it for the joystick even though the label
  suggests otherwise.

## Building the firmware

Push this repo to GitHub and the included Action
(`.github/workflows/build.yml`) builds a `.uf2` firmware file on every push
— check the Actions tab, download the `firmware` artifact from a successful
run. This is also how you'll catch any compile errors in the custom
joystick driver, since it hasn't been build-tested locally.

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
- **Clear a broken pairing:** hold `play/pause` + `next` + the encoder
  push button together to forget the currently active profile's pairing,
  then pair again.
- **Switch USB ↔ Bluetooth:** hold `play/pause` + `next` + the joystick
  push button together to toggle output. Plug in a cable any time you
  want wired/zero-latency mode.

## Controls

| Input | Action |
|---|---|
| `prev` button | Previous track |
| `play/pause` button | Play / pause |
| `next` button | Next track |
| Encoder turn | Volume up / down |
| Encoder push | Mute |
| Joystick tilt | Move mouse cursor |
| Joystick push | Left click |

## First-time bring-up / testing

Test incrementally rather than wiring everything and hoping — each stage
below is independently checkable:

1. **Flash with nothing wired yet.** Confirm the board powers up, enters
   bootloader mode on double-tap-reset, and boots the new firmware
   without crashing (an LED blink pattern or just staying enumerated over
   USB is enough evidence — see Flashing above).
2. **Buttons first.** Wire just `prev`/`play/pause`/`next` and pair over
   BLE (or plug in via USB — no pairing needed to test). On macOS, open
   any media app (Music, Spotify, a YouTube tab) and press each button;
   you should see play/pause/track-change respond immediately. If a
   button does nothing, double check it's on the pin the overlay expects
   and that it's wired to *ground* (these use `GPIO_ACTIVE_LOW` +
   internal pull-up, so a press should short the pin to GND).
3. **Encoder next.** Wire the encoder A/B pins and its push button. Turn
   it — volume should move in the OS. If it moves the wrong direction,
   swap the A/B wires (or swap `a-gpios`/`b-gpios` in the overlay). Push
   should mute.
4. **Joystick last**, since it's the least tested part of this build.
   Wire X/Y to the two ADC pins from the table above and the click button
   to its digital pin. On macOS, watch the cursor: it should sit still at
   rest and move when tilted. If it drifts at rest, increase `deadzone`
   in the overlay's `joystick` node; if too slow/fast, adjust
   `sensitivity` (lower = faster). Each tuning change needs a re-flash.
5. **Bluetooth profile switching.** With everything wired, test the
   pairing combos (see Pairing below) — pair to macOS on profile 0, then
   try the TV/profile 1 combo and confirm it visibly disconnects from one
   and becomes discoverable for the other.
6. **USB fallback.** Plug in a USB-C cable and use the output-toggle
   combo; confirm the device still responds when BLE is out of range or
   off, without needing to re-pair anything.

If a stage fails, isolate it: comment out later stages in the overlay
(or just don't wire them yet) so you know exactly which piece to
debug — don't debug all five inputs at once.

## Tuning the joystick

Once flashed, if the cursor drifts at rest, increase `deadzone` in the
`joystick` node in `media_controller.overlay`. If movement feels too
slow/fast, decrease/increase `sensitivity` (it's a divisor — lower means
faster movement). Re-push to trigger a CI rebuild after each change.
