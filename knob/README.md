# knob — orca-deck firmware for the Waveshare ESP32-S3 Knob (1.8")

The spike panel (`bridge` + a knob UI) as firmware for the ESP32-S3-Knob-Touch-LCD-1.8 (board label "TAIJI KNOB 32").

| Folder | What |
|---|---|
| `hello/` | first test: a ring that follows the crown, a haptic tick per detent, a tap changes colour |
| `deck/` | the knob UI: worktrees on a ring of dots, crown → workspace (Orca follows after 1 s), tap → enter, hold → next agent, double tap → send, swipe right → next |
| `relay.py` | USB transport for debugging: bridge state down the serial line, commands up (`uv run knob/relay.py`) |
| `libraries/lv_conf.h` | LVGL 8.4 config (Montserrat 20 / 28 / 48 on) |

## Build and flash

1. `brew install arduino-cli`; add the Espressif index and `arduino-cli core install esp32:esp32` (≥ 3.2.0).
2. Put LVGL 8.4 at `libraries/lvgl/` (from the Waveshare demo zip, `Arduino/libraries/lvgl`); `arduino-cli lib install ArduinoJson@7.4.2`.
3. `cp deck/secrets.example.h deck/secrets.h` and fill it in.
4. Plug the USB-C so the Mac sees `USB JTAG/serial debug unit` (`/dev/cu.usbmodem*`) — the other orientation reaches the
   board's second chip (an ESP32 behind a CH340). macOS asks "Allow accessory to connect?" the first time.

```bash
cd knob/deck
FQBN='esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB'
arduino-cli compile --fqbn "$FQBN" --libraries ../libraries --build-path /tmp/knob-build .
arduino-cli upload -p /dev/cu.usbmodem* --fqbn "$FQBN" --input-dir /tmp/knob-build
```

Stop `relay.py` before flashing: it holds the serial port.

## Third-party code

- `lcd_bsp.*`, `cst816.*`, `lcd_bl_pwm_bsp.*`, `lcd_config.h`, `esp_lcd_sh8601.*`, `bidi_switch_knob.*` come from the
  Waveshare ESP32-S3-Knob-Touch-LCD-1.8 demo package; `lcd_bsp.c` calls `ui_init()` instead of the LVGL demo.
- `deck_*.c` are Montserrat (SIL Open Font License 1.1) converted with `lv_font_conv` 1.5.2: Latin + Cyrillic.
