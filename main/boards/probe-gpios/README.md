# probe-gpios

Diagnostic board variant — initialises no peripherals; runs a GPIO survey and
I2C scan over USB-Serial-JTAG.

Goal: discover OLED I2C pins on unknown hardware without prior pinout.

## Build / flash

```bash
source ~/esp/esp-idf/export.sh
cd /Users/mo/Desktop/GitHub/xiaozhi-esp32
rm -rf build sdkconfig
python scripts/release.py probe-gpios
idf.py -p /dev/cu.usbmodem101 -b 460800 flash monitor
```

Hold BOOT during USB plug-in if servos are stealing the USB pins.

## What it does

1. Two-second startup delay so the host can attach.
2. **Floating survey:** every safe GPIO gets read once with internal pull-up
   enabled, once with internal pull-down enabled. Pins that read HIGH under
   pull-down have an external pull-up — strong I2C/button candidates.
3. **I2C scan:** every ordered (SDA, SCL) pair from the candidate list gets
   set up as an I2C master and probed at addresses 0x3C / 0x3D (SSD1306
   default + alternate). Hits are logged with `*** I2C device 0xXX responds
   at SDA=N SCL=M ***`.

## Excluded GPIOs

- 19, 20 — USB-Serial-JTAG D-/D+
- 22-25 — don't exist on ESP32-S3
- 26-32 — flash/PSRAM on N16R8

Bootstrap pins (0, 3, 45, 46) are scanned but only as inputs, never driven HIGH.
