# Dependencies — type_it Macro Keyboard

## Required software

1. **Arduino IDE** 1.8.x or 2.x, or **Arduino CLI**
2. **Earle Philhower Arduino-Pico core** (supports RP2040/RP2350 boards with USB and LittleFS)
3. **U8g2 library** by Oliver Kraus (SH1106 display driver)

## Arduino-Pico core installation

### Arduino IDE 2.x

1. Open **File > Preferences**.
2. In **Additional boards manager URLs**, add:
   ```text
   https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
   ```
3. Open **Tools > Board > Boards Manager**.
4. Search for **"Raspberry Pi Pico/RP2040"** by Earle F. Philhower, III.
5. Install the latest version.

### Arduino CLI

```bash
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index
arduino-cli core install rp2040:rp2040
```

## U8g2 library installation

### Arduino IDE

1. Open **Tools > Manage Libraries...**.
2. Search for **"U8g2"** by Oliver Kraus.
3. Install the latest version.

### Arduino CLI

```bash
arduino-cli lib install "U8g2"
```

## Board selection

Select the board that matches your hardware. For the Waveshare RP2040 Zero:

- **Board**: *Raspberry Pi Pico / RP2040 > Waveshare RP2040 Zero*
- **USB Stack**: *Adafruit TinyUSB* (recommended; required for `Keyboard.h` and USB-CDC Serial)
- **Flash Size**: choose a split that leaves room for a filesystem, e.g. *2 MB (Sketch: 1 MB, FS: 1 MB)* or larger

If your exact board is not listed, use **"Raspberry Pi Pico"** and set the correct USB stack and flash size.

## Build and upload

### Arduino IDE

1. Open `hardware_project/type_it/type_it.ino`.
2. Select the board and port as described above.
3. Click **Upload**.

### Arduino CLI

From the `hardware_project` directory:

```bash
arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2040_zero type_it
arduino-cli upload --fqbn rp2040:rp2040:waveshare_rp2040_zero -p /dev/ttyACM0 type_it
```

Replace `/dev/ttyACM0` with the actual serial port of your RP2040 Zero.

## First use

1. Open the **Serial Monitor** at **115200 baud**.
2. The firmware prints a startup banner and the number of stored profiles.
3. Type `HELP` and press Enter to see the available serial commands.

Example: create a profile and upload multiline text

```text
ADDPROFILE:work
OK created work

SETTEXT:work <<EOF
Hello, world!
Thanks,
Me
EOF
OK replaced work (29 bytes)

LIST
Profiles:
  work (3 items)
```

## Notes and limitations

- **HID typing**: `Keyboard.print()` sends raw keycodes. ASCII/Latin-1 text works reliably. Typing CJK or other multi-byte Unicode characters is not supported by the standard Arduino `Keyboard` library; such text can be stored and viewed on the display but will not type correctly through USB HID.
- **Display font**: The default font is `u8g2_font_7x14_tf` for the header and `u8g2_font_6x13_tf` for list items. If you need CJK glyphs on the display, replace these with a U8g2 CJK font (e.g., `u8g2_font_unifont_t_chinese2`) and note that the larger font may reduce the number of visible lines.
- **Filesystem wear**: Every type action writes the usage-order file to LittleFS. For heavy daily use, consider periodically backing up profiles via `VIEW:profile` and rebuilding the device rather than relying on the flash for indefinite high-frequency writes.
