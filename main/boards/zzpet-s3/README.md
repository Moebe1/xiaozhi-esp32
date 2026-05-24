# ZZPET-BOT v1.2.8 (`zzpet-s3`)

xiaozhi-esp32 board variant for the **ZZPET-BOT v1.2.8** — an unmarked
ESP32-S3 quadruped pet robot sold without public schematics or firmware
source. This board variant was built by reverse-engineering the OEM firmware
plus empirical hardware probing, so the device can run modern xiaozhi
(v2.2.6+) against a self-hosted backend instead of the OEM's closed stack.

The vendor SKU is `zzpet-s3`; the board name here matches deliberately so
OTA artefacts and release zips align with the physical hardware.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3 (QFN56), rev v0.2, 240 MHz dual-core |
| Flash | 16 MB (Boya, QIO) |
| PSRAM | 8 MB embedded, octal (AP_3v3, generation 3) |
| Console | USB-Serial-JTAG (`/dev/cu.usbmodem*` on macOS) |
| Display | **SH1106** 128×64 OLED, I2C @ 0x3C |
| Audio | Simplex I2S — single MEMS mic + class-D amp (no codec, no MCLK, no PA enable) |
| LEDs | 3× WS2812 — bottom pair (2 LEDs) + WiFi indicator (1 LED) |
| Legs | 4× SG90-class hobby servos, single-DOF hip pivots (no knee) |
| Buttons | BOOT (hardware), capacitive touch sensor |
| Battery | Li-ion + USB charging, VBAT/2 ADC sense |
| FN button | Hardware EN/RESET (not a GPIO) |

## Pinout

| GPIO | Function |
|------|----------|
| 0 | BOOT button (xiaozhi default) |
| 2 | Battery sense (VBAT/2 divider, ADC1_CH1) |
| 4 | I2S mic WS |
| 5 | I2S mic SCK |
| 6 | I2S mic DIN |
| 7 | I2S speaker DOUT (to amp DIN) |
| 8 | WS2812 bottom pair (2 LEDs) |
| 10 | Capacitive touch |
| 13 | Servo: FR (right leg) |
| 14 | Servo: BR (right foot) |
| 15 | I2S speaker BCLK |
| 16 | I2S speaker WS / LRCK |
| 17 | Servo: FL (left leg) |
| 18 | Servo: BL (left foot) |
| 19 / 20 | USB-Serial-JTAG D-/D+ |
| 41 | OLED I2C SDA |
| 42 | OLED I2C SCL |
| 48 | WS2812 WiFi LED |

## MCP tools exposed

Body-coordinate angle convention (uniform across all 4 legs): **90 = leg
straight down**, **180 = leg fully forward**, **0 = leg fully back**.
Left-side servos are physically mirror-mounted; the board class flips
them internally so the LLM never has to think about which side it's
addressing.

| Tool | Args | Behaviour |
|------|------|-----------|
| `self.pet.move_leg` | leg 1-4, angle 0-180 | Synchronous; atomic primitive |
| `self.pet.set_pose` | pose 1-4 | 1=stand, 2=sit, 3=lie_down, 4=alert — **HOLDS** until next command |
| `self.pet.wave` | leg 1-4 | Animation: raise to 180°, oscillate 60↔180 three times, return to neutral |
| `self.pet.shake_paw` | — | FR leg raised, **HOLDS** the offer |
| `self.pet.bow` | — | Both front legs forward, **HOLDS** |
| `self.pet.scratch` | leg 3-4 | Rapid small oscillation, return to neutral |
| `self.pet.happy_wiggle` | — | Alternating front/back sway, return to neutral |
| `self.pet.walk` | steps 1-5 | Diagonal-pair trot, return to neutral (capped at 5 steps) |
| `self.pet.jump` | — | Legs fold inward, snap back to stand |
| `self.pet.set_lights` | color 0-7 | Bottom-strip colour (0=off / 1=red / 2=green / 3=blue / 4=yellow / 5=magenta / 6=cyan / 7=white) |
| `self.pet.wave_hello` | — | Back-compat: same as `wave(leg=2)` |

All long-running motion tools dispatch to a **dedicated `robo_motion`
FreeRTOS task** (8 KiB stack, priority 4) and return to the LLM
immediately — see "Motion task system" under quirks below.

## Deployment context

This board is part of Mo's self-hosted xiaozhi stack:

- **Server**: `xinnan-tech/xiaozhi-esp32-server` (docker compose) on a
  ZimaBoard. WebSocket on `:8000`, OTA/HTTP on `:8003`.
- **STT**: FunASR with the SenseVoiceSmall multilingual model (local).
- **TTS**: EdgeTTS, voice `en-AU-NatashaNeural`.
- **LLM**: Claude Haiku 4.5 via LiteLLM → AWS Bedrock
  (`bedrock/converse/au.anthropic.claude-haiku-4-5-20251001-v1:0`).
- **OTA URL** baked into firmware via Mo's commit `0884f7e` — points the
  device at the ZimaBoard rather than `api.tenclass.net`.

The device reaches the server on the LAN. No internet egress required on
the device itself.

## Build

```bash
# release.py is the canonical path — generates a release zip + merged binary.
python scripts/release.py zzpet-s3

# Or manual:
idf.py set-target esp32s3
idf.py menuconfig    # Xiaozhi Assistant → Board Type → ZZPET-BOT v1.2.8
idf.py build
```

Gotcha: `release.py` silently skips a build if `releases/v<ver>_zzpet-s3.zip`
already exists. Delete the zip before rebuilding the same variant.

## Flash

Easiest one-liner using the merged image:

```bash
cd build
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXX -b 460800 \
    write_flash 0x0 merged-binary.bin
```

The `@flash_args` recipe in the repo uses paths relative to `build/` and
fails if invoked from repo root.

## Discovery methodology

The OEM ships no public docs or source. This board variant was built by:

1. **Vendor firmware disassembly.** The OEM firmware (`zzpet-s3` SKU,
   v1.2.8) is a vendor fork of xiaozhi-esp32. Sliced the app partition
   out of the merged binary, ran `xtensa-esp32s3-elf-objdump` on the
   IROM segment, located `InitializeAudio()` and `InitializeDisplayI2c()`,
   read the `movi` immediates into the constructor arg registers to
   recover the I2S and I2C pin numbers.
2. **Empirical probing.** A standalone discovery sketch (`board-probe/`
   in the workspace) implementing interactive I2C scan, NeoPixel sweep,
   servo PWM sweep, ADC mapping, and pull-up / pull-down GPIO classification
   over USB-Serial-JTAG. Confirmed every pin the disassembly suggested,
   and found the two right-side servos that the disassembly didn't
   reveal (vendor's `ZzPetController` does servo init via a path the
   first pass missed).
3. **Cross-reference with `otto-robot`.** ZZPET is structurally derived
   from xiaozhi's `otto-robot` board (`NoAudioCodecSimplex`, same
   `oscillator.cc` / `power_manager.h` filenames in the binary's strings
   table). The audio pinout matches otto's `NON_CAMERA_VERSION_CONFIG`
   exactly; the leg pins differ.
4. **End-to-end validation.** Ran the X-test suite (full peripheral sweep
   in `board-probe`) to verify each pin assignment actually drove the
   intended peripheral. Then flashed this board variant and confirmed
   audio loopback, MCP tool dispatch, and Wi-Fi config flow against the
   ZimaBoard.

## Quirks and workarounds

- **SH1106 driver bug.** The `tny-robotics__sh1106-esp-idf` managed
  component's `panel_mirror()` maps `mirror_x` to `0xA6/0xA7` (display
  *colour invert*), not segment remap. We work around it by sending
  `0xA0` (`SEG_REMAP_NORMAL`) via `esp_lcd_panel_io_tx_param()`
  directly *after* `OledDisplay` is constructed (LVGL calls
  `panel_mirror()` internally during init).
- **Left/right servo mirroring.** Servos on the dog's left side are
  physically mirror-mounted. The board class applies a `180 − angle`
  flip to the left-side LEDC channels so the LLM can speak in body
  coordinates uniformly for all four legs.
- **Servo duty range.** SG90 clones in this dog reach about half their
  visible motion with the spec 1.0–2.0 ms pulse window. Extended the
  range to **0.6–2.4 ms** (duty 245–983 at 13-bit @ 50 Hz) for the
  expected ~180° body-frame sweep.
- **Mic gain.** The audio path is `NoAudioCodecSimplex` (no analog gain
  control). The codec's existing input-gain multiplier was integer-cast,
  which truncated any value < 2.0 to a no-op. The `no_audio_codec.cc`
  patch in this PR switches to float multiplication so fractional gains
  (e.g. `1.1f` = +10 %) take effect.
- **Motion task system.** Synchronous MCP tool lambdas run on the main
  FreeRTOS task; a single multi-second tool blocks audio + state-machine
  events. Long-running motion tools here dispatch to a dedicated
  `robo_motion` task (8 KiB stack, mutex-serialised so two motions can't
  contend). A binary semaphore (NOT a priority-inherit mutex) is used
  because the take happens on the main task and the give on the motion
  task — FreeRTOS mutexes assert on cross-task release.
- **Display orientation.** The OLED is physically mounted upside-down
  relative to its silkscreen. With the SH1106 driver workaround above,
  rendering reads upright in the natural viewing direction.
- **FN button.** Wired to the chip's hardware EN/RESET, not a GPIO —
  pressing it resets the device (and drops USB enumeration). Useful for
  recovery; not exposed to firmware.
