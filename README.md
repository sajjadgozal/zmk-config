# media_controller

A ZMK-based media remote / mini pointing device: 3 buttons (prev /
play-pause / next), a rotary encoder (volume + mute), and an analog
joystick (cursor movement + left-click) — built for a Pro-Micro-footprint
nRF52840 board (target: "V1940 Pro Micro nRF52840", flashed as
`nice_nano_v2` since it shares the nice!nano v2 pinout). Works over BLE
(pairs as a standard HID keyboard/consumer-control/mouse device — no
drivers needed on macOS or Bluetooth-capable TVs) or over a USB cable,
switchable at any time.

## Repo layout

- `config/west.yml` — points at upstream ZMK firmware, pinned to the
  `v0.3.0` release rather than `main` (ZMK's `main` branch is currently
  mid-migration off its deprecated KSCAN subsystem post-Zephyr-4.1, which
  makes builds using `zmk,kscan-gpio-direct` — including this one — fail
  outright; ZMK's own CI recommends pinning when this happens). Bump this
  once ZMK finishes that migration and cuts a new release.
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

## ⚠️ Before you wire anything up

The digital pin numbers in
`boards/shields/media_controller/media_controller.overlay`
(`&pro_micro 4/5/6/7/9` for buttons, `&pro_micro 20/21` for the encoder)
are a reasonable starting guess for a nice!nano-pinout board, **not
verified against your specific V1940 board's silkscreen**. The joystick's
X/Y wires are different: they must land on ADC-capable pins, referenced
directly as `NRF_SAADC_AIN0`/`AIN1` in the overlay (these usually
correspond to the pins labeled `A0`/`A1` on nice!nano-pinout boards).
Before soldering, check your board's pinout diagram and adjust:

- button/encoder pins → any free digital GPIO works
- joystick X/Y → must be ADC-capable pins; update `NRF_SAADC_AINx` to
  match whichever analog pins you actually use

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
west build -p -b nice_nano_v2 -- -DSHIELD=media_controller
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
  "Media Controller".
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

## Tuning the joystick

Once flashed, if the cursor drifts at rest, increase `deadzone` in the
`joystick` node in `media_controller.overlay`. If movement feels too
slow/fast, decrease/increase `sensitivity` (it's a divisor — lower means
faster movement). Re-push to trigger a CI rebuild after each change.
