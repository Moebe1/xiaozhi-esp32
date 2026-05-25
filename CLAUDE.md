# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

XiaoZhi AI voice-assistant firmware for the ESP32 family. ESP-IDF C++ project targeting ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C5 / ESP32-C6 / ESP32-P4. One codebase produces 100+ board variants from `main/boards/*`; the active board is chosen at configure time via Kconfig.

- Current version: `PROJECT_VER` in root `CMakeLists.txt` (currently `2.2.6`).
- `origin` is the fork `github.com/Moebe1/xiaozhi-esp32.git` — upstream is `github.com/78/xiaozhi-esp32`. Treat upstream as the source of truth for issues, PR reviews, and design docs.
- **v1 vs v2 partitions are incompatible** — no OTA upgrade path from v1 to v2. v1 stable is `1.9.2` (`git checkout v1`), maintained until Feb 2026. New work goes on `main` (v2).

## Toolchain

- **ESP-IDF >= 5.5.2** required (see `main/idf_component.yml`). Most board variants only build on the targets listed in that file's `rules:` blocks — don't assume a board supports a chip not declared there.
- **`clang-format`** for C++ style (Google C++ base, 4-space indent, 100-col limit, attach braces). `.clang-format` at repo root.
- The repo expects `idf.py` on `PATH` and `$IDF_PATH` set — neither this CLAUDE.md nor the scripts will export them for you.

## Common commands

All run from the repo root unless noted.

```bash
# One-off setup per chip target
idf.py set-target esp32s3        # or esp32, esp32c3, esp32c6, esp32p4, esp32c5

# Pick the board variant (CONFIG_BOARD_TYPE_*) and any board-specific options
idf.py menuconfig                # see "Xiaozhi Assistant" submenu

# Build / flash / monitor
idf.py build
idf.py -p /dev/cu.usbmodemXXX flash monitor

# Format
clang-format -i path/to/file.cc
find main -iname '*.h' -o -iname '*.cc' | xargs clang-format -i

# Wipe (when switching targets or after Kconfig changes that confuse the build)
idf.py fullclean
```

### Building specific board variants via `scripts/release.py`

`scripts/release.py` is the canonical way to build & package a board variant. It reads each board's `config.json` (target chip, name, `sdkconfig_append`), regenerates `sdkconfig`, runs `idf.py build`, then zips `build/merged-binary.bin` to `releases/v<version>_<name>.zip`.

```bash
python scripts/release.py --list-boards            # list all (board, variant) pairs
python scripts/release.py --list-boards --json     # same, machine-readable (used by CI)
python scripts/release.py <board-dir>              # build all variants for that board
python scripts/release.py <board-dir> --name <variant-name>   # build one variant only
python scripts/release.py all                      # build every variant (slow)
python scripts/release.py                          # zip the current build/ as a release artifact
```

`<board-dir>` is the path under `main/boards/` (e.g. `bread-compact-wifi`, or `waveshare/esp32-p4-nano` for manufacturer-grouped boards).

### Build / flash gotchas

- `scripts/release.py <board>` silently **skips** work if `releases/v<ver>_<board>.zip` already exists — `rm -f releases/v<ver>_<board>.zip` before rebuilding the same variant.
- Easiest flash from outside the build dir: `cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXX -b 460800 write_flash 0x0 merged-binary.bin`. The `@flash_args` recipe uses paths relative to `build/` and fails if invoked from repo root.
- ESP32-S3 with USB-Serial-JTAG enumerates on macOS as `/dev/cu.usbmodem*`; chips with a UART bridge (CP210x/CH340) show as `/dev/cu.usbserial-*`.
- Clang/clangd LSP throws **false-positive** errors on this codebase: `-mlongcalls` / `-fstrict-volatile-bitfields` (xtensa flags), `OledDisplay* → Display*` (inheritance goes via `LvglDisplay`), `bool → ReturnValue` (variant is constructible), `EnterWifiConfigMode` access. The xtensa-gcc build is the source of truth — verify with `idf.py build`, ignore LSP noise.
- The `tny-robotics__sh1106-esp-idf` managed component has a bug: `esp_lcd_panel_mirror()` maps `mirror_x` to `0xA6/0xA7` (display *color* invert), not segment remap. To actually flip X, send `0xA0`/`0xA1` via `esp_lcd_panel_io_tx_param()` directly *after* the LVGL display is constructed (LVGL also calls `panel_mirror()` internally; do it after).
- Wake word is selected at compile-time via `CONFIG_SR_WN_WN9_*`. This fork's default is `NIHAOXIAOZHI_TTS` ("你好小智"). Custom words require training on Espressif's customer portal (days); prebuilt English alternatives include `JARVIS`, `ALEXA`, `LEXIN`.

### CI

`.github/workflows/build.yml` builds all variants on push-to-main. On PRs it diff-filters: changes under `main/<not boards>` or `main/boards/common/` trigger a full matrix; otherwise only the touched board dirs build. Account for that when scoping changes — touching `main/application.cc` rebuilds 100+ variants.

## High-level architecture

Entry point is `main/main.cc → app_main()`, which initializes NVS and calls `Application::GetInstance().Initialize(); Run();`. From there the system is a single-threaded FreeRTOS event loop driven by an event group.

```
                                  ┌─────────────────────────┐
                                  │   Application (singleton)│
                                  │   main/application.{h,cc}│
                                  │                          │
                                  │   FreeRTOS event group:  │
                                  │   - SCHEDULE / SEND_AUDIO│
                                  │   - WAKE_WORD_DETECTED   │
                                  │   - VAD_CHANGE / ERROR   │
                                  │   - NETWORK_CONNECTED/…  │
                                  │   - TOGGLE_CHAT / …      │
                                  └────────────┬─────────────┘
                                               │ owns
        ┌──────────────────────┬───────────────┼───────────────┬──────────────────────┐
        ▼                      ▼               ▼               ▼                      ▼
┌──────────────┐    ┌─────────────────┐  ┌──────────┐   ┌───────────────┐    ┌──────────────────┐
│ DeviceState  │    │  AudioService   │  │ Protocol │   │      Ota      │    │   Board (global) │
│   Machine    │    │ main/audio/     │  │ (abstract)│  │  main/ota.cc  │    │ main/boards/…    │
│ device_state │    │  + codecs/      │  │ ws/mqtt   │  │ talks to OTA   │    │ provides codec,  │
│ _machine.cc  │    │  + wake_words/  │  │ protocols/│  │  endpoint      │    │ display, LED,    │
└──────────────┘    └─────────────────┘  └──────────┘   └───────────────┘    │ buttons, network │
                                                                              └──────────────────┘
```

Other key subsystems off `main/`:

- **`mcp_server.{h,cc}`** — device-side **MCP server** that exposes board capabilities (speaker, LED, servo, GPIO, camera, …) as MCP tools the cloud model can call. Board classes register tools during construction (see `InitializeTools()` in any board file, and `boards/common/press_to_talk_mcp_tool.cc`).
- **`display/`** — UI rendering. `display.cc` is the abstract base; `lcd_display.cc` / `oled_display.cc` / `emote_display.cc` are the variants. LVGL 9.5 + `esp_lvgl_port`. Custom assets (fonts, emojis, backgrounds) live in `main/assets/` and are packed via `scripts/build_default_assets.py` into the `assets` partition (selected by `CONFIG_FLASH_*_ASSETS`).
- **`assets/locales/<lang>/`** — generated localization strings. Don't hand-edit; use `scripts/gen_lang.py`. Default language is `CONFIG_LANGUAGE_*` Kconfig (defaults to `LANGUAGE_ZH_CN`).
- **`partitions/v2/*.csv`** — partition tables, one per flash size (4m/8m/16m/32m). Boards select one through `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in their `sdkconfig_append`.

## Board layout (critical to understand before adding hardware support)

Every board variant is a directory under `main/boards/`. The minimum contents:

```
main/boards/<board>/
  config.h        # pin assignments, sample rates, codec/display constants
  config.json     # target chip + builds[]; consumed by scripts/release.py
  <board>.cc      # board class deriving from WifiBoard or Ml307Board, plus DECLARE_BOARD(...)
  README.md       # board-specific notes (optional but recommended)
```

Boards can be flat (`main/boards/bread-compact-wifi/`) or grouped by manufacturer (`main/boards/waveshare/esp32-p4-nano/`). When grouped, `config.json` **must** set `"manufacturer": "<dir>"` matching the parent directory — `scripts/release.py` enforces this consistency and fails the build otherwise.

The board variant the firmware ships with is selected by Kconfig: `CONFIG_BOARD_TYPE_<UPPER_SNAKE>` is mapped to a `BOARD_TYPE` string in `main/CMakeLists.txt` (the long `if/elseif` chain near the bottom), which in turn becomes a `-DBOARD_TYPE="…"` compile define and adds that board's sources. **If you add a new board, you must add a new `elseif` branch to that chain in `main/CMakeLists.txt`** — otherwise `release.py` will error out with "board_type not found in main/CMakeLists.txt".

Shared base classes and peripherals live in `main/boards/common/`:

- `board.{h,cc}` — `Board` abstract base + `DECLARE_BOARD` macro (singleton registration).
- `wifi_board.cc` — Wi-Fi-only boards.
- `ml307_board.cc` — ML307 4G modem boards.
- `nt26_board.cc` — NT26 modem boards.
- `dual_network_board.cc` — boards that can switch between Wi-Fi and cellular.
- Peripheral helpers: `axp2101` (PMIC), `button`, `knob`, `backlight`, `power_save_timer`, `sleep_timer`, `adc_battery_monitor`, `esp32_camera` / `esp_video`, `afsk_demod`, `blufi`, `press_to_talk_mcp_tool`, `i2c_device`, etc. Reuse these before writing peripheral code from scratch.

## Critical rules when modifying boards

> **Never overwrite the config of an existing board to match different hardware.** OTA channels are keyed off the firmware `name` from `config.json`. If you reuse an existing name for different hardware, OTA will eventually push the stock firmware to your custom device and brick it.
>
> Either create a new board directory, or add a new entry to the `builds` array in `config.json` with a unique `name` and the right `sdkconfig_append`. Then run `python scripts/release.py <board-dir>` to package it.

Other rules worth keeping in mind:

- Don't edit files under `main/assets/locales/` directly — regenerate via `scripts/gen_lang.py`.
- `main/assets/lang_config.h` and `main/mmap_generate_*.h` are generated at build time and listed in `.gitignore` — never commit them.
- Network-mode changes (Wi-Fi performance mode, RX buffers, dynamic buffers) are tuned in `sdkconfig.defaults` for low-RAM chips; if a board needs different values, put them in that board's `sdkconfig_append`, not in the shared defaults.
- The project relies on C++ exceptions and RTTI (`CONFIG_COMPILER_CXX_EXCEPTIONS=y`, `CONFIG_COMPILER_CXX_RTTI=y` in `sdkconfig.defaults`). Don't disable them.

## Docs worth reading before deeper work

All under `docs/` (each has a `_zh.md` Chinese counterpart):

- `custom-board.md` — full walkthrough for adding a new board.
- `mcp-protocol.md` — device-side MCP protocol implementation details.
- `mcp-usage.md` — registering and invoking MCP tools from board code.
- `websocket.md` — WebSocket protocol (`protocols/websocket_protocol.cc`).
- `mqtt-udp.md` — MQTT+UDP hybrid protocol (`protocols/mqtt_protocol.cc`).
- `code_style.md` — clang-format setup and the few project-specific tweaks on top of Google C++ style.

## Agent files

Two parallel agent-guidance files live at the repo root:

- **`CLAUDE.md`** (this file) — guidance for Claude Code.
- **`AGENT.md`** — guidance for Antigravity (Google's coding agent). Tracked in git; near-identical content to this file. Keep both in sync when editing structural sections.

If you find them out of sync, the more recent commit wins — but flag the drift to Mo before reconciling.

## Personal deployment notes (Mo's fork)

This fork runs against Mo's self-hosted backend on a ZimaBoard `mo@192.168.0.35` (hostname `fedora`; SSH key-auth works). Two-container stack at `/home/mo/xiaozhi/`: `xiaozhi-esp32-server` (xinnan-tech) and `litellm` (BerriAI). LLM = `bedrock/converse/au.anthropic.claude-haiku-4-5-20251001-v1:0` via LiteLLM (set in `LLM.ClaudeViaLiteLLM`). STT FunASR (SenseVoiceSmall), TTS **Amazon Polly** (en-GB neural, default voice `Amy`; earlier setup used EdgeTTS `en-AU-NatashaNeural`). Device reaches the server on LAN port `8003` (OTA/HTTP) and `8000` (WebSocket).

**Firmware delta from upstream:** commit `0884f7e` overrides OTA URL → ZimaBoard, default language → `LANGUAGE_EN_US`. Custom boards on `main`:
- `zzpet-s3` — ZZPET-BOT v1.2.8 quadruped pet robot (ESP32-S3, full pinout in memory `[[unusual-esp-board]]`)
- `probe-gpios` — ad-hoc GPIO probing utility board for reverse-engineering unmarked hardware
- Other previously built variants: `esp-hi`, `xmini-c3`

**Server config layout:**
- `/home/mo/xiaozhi/docker-compose.yml` — services + custom volume mounts (auto-managed by `scripts/deploy_to_zimaboard.sh`)
- `/home/mo/xiaozhi/data/.config.yaml` — prompt, LLM provider selection, plugin overrides
- `/home/mo/xiaozhi/data/.agent-base-prompt.txt` — base prompt template wrapping persona switches (preserves ASR-tolerance, direct-reply, TTS negative-constraint rules)
- `/home/mo/xiaozhi/models/SenseVoiceSmall/model.pt` — STT weights
- `/home/mo/xiaozhi/custom_src/` — created by the deploy script; holds Mo's Python overrides bind-mounted into the container

**Upstream image bakes Chinese defaults** for weather (default_location=广州), news (chinanews.com.cn / newsnow.busiyi.world), and music (local Chinese MP3 dir). Override in `data/.config.yaml` via a `plugins:` block plus a filtered `Intent.function_call.functions:` list — anything not listed there is removed from the LLM's tool surface.

### Server-side customizations (xinnan-tech overrides)

Source lives in the **untracked** sibling directory `xiaozhi-esp32-server/` (a clone of `github.com/xinnan-tech/xiaozhi-esp32-server` with Mo's edits). Files are deployed to the ZimaBoard by `scripts/deploy_to_zimaboard.sh`, which copies them to `/home/mo/xiaozhi/custom_src/` and bind-mounts each one into the running container — so changes take effect on `docker compose up -d` without rebuilding the image.

Currently deployed overrides:

- **`core/utils/util.py` — `get_ip_info()`**: queries `ip-api.com` for city + IANA timezone (e.g. `Australia/Sydney`) by client IP, applies via `os.environ['TZ']` + `time.tzset()`, caches result. Runs inside `prompt_manager._get_location_info` via `executor.submit(...)` (worker thread, not asyncio loop — verified safe). Note: `time.tzset()` is process-global; multiple concurrent devices in different timezones will race on it. Wall-clock comparisons that use `time.time()` / `time.monotonic()` are unaffected.
- **`core/providers/asr/base.py`** — Mo's ASR base-class patch (copied + bind-mounted by the deploy script).
- **`plugins_func/functions/change_role.py`**: 4 Australian personas (Dazza, Charlotte, Jack, Sarah) with ~20-word brevity caps, disabled self-intros, AWS Polly voice-syncing. Switched roles are wrapped in `agent-base-prompt.txt`.
- **`plugins_func/functions/change_voice.py`** (`ToolType.SYSTEM_CTL`): mutates `conn.tts.voice` in place — despite the name, **does not** trigger a connection handoff or close the WS.
- **`plugins_func/functions/calculator.py`** (`ToolType.NONE`): AST-based safe arithmetic evaluator. Whitelist on `0123456789+-*/(). ` is the actual injection guard; the `ast.Constant` branch is permissive but unreachable beyond that filter.
- **`plugins_func/functions/system_status.py`** (`ToolType.NONE`): CPU, memory, uptime stats.
- **`plugins_func/functions/run_custom_routine.py`** (`ToolType.SYSTEM_CTL`, async): runs an LLM-supplied sequence of device-tool calls + `delay` steps. No cap on step count or delay duration — LLM-controlled, so an adversarial / hallucinated routine could tie up the connection.

### Deploying changes to the ZimaBoard

```bash
# From the firmware repo root, with the xiaozhi-esp32-server/ clone present:
bash scripts/deploy_to_zimaboard.sh
```

The script: (1) collects custom Python files from `xiaozhi-esp32-server/` into a local staging dir, (2) `scp`s them to `mo@192.168.0.35:/home/mo/xiaozhi/custom_src/`, (3) rewrites `docker-compose.yml` to bind-mount each override, (4) restarts containers, (5) installs `boto3` inside the server container for Polly. Idempotent. Doesn't rebuild the image — overrides are file-level bind mounts.

### Known operational quirks (firmware ↔ server WS)

- **Server idle close**: when no voice activity for `close_connection_no_voice_time` seconds (default 120), the server speaks an end-prompt and closes the WS. Hard ceiling at +60 s via `_check_timeout` in `core/connection.py`.
- **No `ping_interval`/`ping_timeout` override** in `core/websocket_server.py:76` — the `websockets` library defaults apply (20 s ping / 20 s pong). Asyncio loop starvation > 40 s ⇒ lib-level close. The `executor` is `max_workers=5`; long LLM streams hold a worker per active session.
- **Firmware has no auto-reconnect on the WS** (contrast `mqtt_protocol.cc` which does). Any drop returns the device to idle — the user must wake-word again to recover. `application.cc:512-519` handles `on_audio_channel_closed_` → `SetDeviceState(kDeviceStateIdle)`.
- **No firmware-side ping frames** — all keepalive is server→client.
