# media_controller

A ZMK-based Bluetooth media remote: 3 buttons (prev / play-pause / next) +
a rotary encoder (volume + mute), built for a Pro-Micro-footprint nRF52840
board (tested target: "V1940 Pro Micro nRF52840", flashed as `nice_nano_v2`
since it shares the nice!nano pinout). Pairs as a standard BLE HID
Consumer Control device, so it works with macOS and most Bluetooth-capable
TVs/streaming boxes with no drivers.

## Repo layout

- `config/west.yml` — points at upstream ZMK firmware
- `config/media_controller.keymap` — key bindings + Bluetooth-profile combos
- `config/media_controller.conf` — BLE/USB/power Kconfig options
- `boards/shields/media_controller/` — the custom "shield": which GPIO
  pins the buttons/encoder are wired to
- `build.yaml` — tells GitHub Actions which board+shield to build

## ⚠️ Before you wire anything up

The pin numbers in `boards/shields/media_controller/media_controller.overlay`
(`&pro_micro 4/5/6/7` for buttons, `&pro_micro 20/21` for the encoder) are a
reasonable starting guess for a nice!nano-pinout board, **not verified
against your specific V1940 board's silkscreen**. Before soldering, check
your board's pinout diagram and edit those pin numbers to match whichever
GPIOs you actually wire the switches/encoder to. Any free digital GPIO pins
work — there's nothing special about 4/5/6/7/20/21 other than that they're
commonly free on nice!nano-compatible boards.

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

## Controls

| Input | Action |
|---|---|
| `prev` button | Previous track |
| `play/pause` button | Play / pause |
| `next` button | Next track |
| Encoder turn | Volume up / down |
| Encoder push | Mute |
