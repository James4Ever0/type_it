# Type It Hardware — RP2040 Zero Macro Keyboard

A physical macro keyboard: a Waveshare **RP2040 Zero** running Arduino
firmware, a 1.3" **SH1106 128×64 I2C OLED**, and an **EC11 rotary encoder**
with push switch. Pick a profile, turn the knob, and the selected snippet is
typed into the focused application as real USB HID keystrokes.

It is the hardware counterpart of the desktop demos in
[`software/`](../software/) — all three share the same
profiles/candidates/recency model.

## Components

| Part | Role |
|------|------|
| Waveshare RP2040 Zero | MCU, USB HID keyboard |
| 1.3" SH1106 128×64 I2C OLED | Menu display, CJK-capable |
| EC11 rotary encoder | Scroll / confirm / long-press back |
| CON / BAK buttons | Optional confirm / back buttons |

## Repository layout

- [`type_it/`](type_it/) — Arduino sketch (`type_it/type_it.ino`)
- [`DEPENDENCIES.md`](DEPENDENCIES.md) — Arduino core + library install, board setup, upload
- [`WIRING.md`](WIRING.md) — GPIO pin map and step-by-step wiring guide
- [`casing/`](casing/) — 3D-printable enclosure (`.3mf`)
- [`assets/`](assets/) — wiring reference photo

## Quick start

1. Wire the module — see [`WIRING.md`](WIRING.md) (GPIO table, pull-ups,
   encoder direction, I2C address).
2. Install the toolchain — see [`DEPENDENCIES.md`](DEPENDENCIES.md).
3. Open `type_it/type_it.ino` and upload (Arduino IDE or `arduino-cli`).
4. Open the serial monitor at **115200 baud** and send `HELP` for the full
   command list.

## Firmware highlights

- **Profiles on LittleFS** — every `.txt` in `/profiles` is a profile; each
  non-empty line is a type-able candidate.
- **Recency ordering** — profiles and candidates re-sort by last use and the
  order survives reboots (stored in `/usage.dat`).
- **USB HID typing** — ASCII types directly; Unicode/CJK types per host
  method (Linux IBus, macOS Unicode Hex Input, Windows Alt+Numpad). Switch
  host behavior with `SETOS:lin|mac|win` or the Settings menu.
- **CJK display** — the SH1106 is rendered with the U8g2 `wqy12` GB2312 font.
- **Serial management** — create/rename/delete profiles and edit candidates
  over serial (`SETTEXT:profile <<EOF … EOF`, `APPEND`, `LIST`, …).
- **Diagnostics** — `DIAG` runs a 7-step self-test (display, encoder,
  buttons, OS typing, filesystem, EEPROM, font).
- **Resume state** — the last view and selection are restored from EEPROM on
  power-up.
