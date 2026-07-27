# ESP32-P4 Guition — developer notes

Engineering context for this project. **[README.md](README.md) is the student-facing guide** — keep
it task-oriented and put internals, rationale and traps here.

Single-application firmware for the **Guition JC4880P443C-I-W** (ESP32-P4 + ESP32-C6, 4.3"
480×800), ESP-IDF **v5.5.2**, native Windows/PowerShell build.

---

## Build and flash

```powershell
& "$env:USERPROFILE\esp\v5.5.2\esp-idf\export.ps1"
idf.py -p COM11 flash          # confirm the port
idf.py -p COM11 monitor
```

`sdkconfig.defaults` only seeds options **absent** from `sdkconfig`. After editing it, delete
`sdkconfig` and rebuild, or the new values are silently ignored — this has wasted time more than
once.

---

## Hardware

| | |
|---|---|
| SoC | ESP32-P4, dual RISC-V @ 360 MHz, 32 MB PSRAM (HEX 200 MHz), 16 MB flash |
| Panel | ST7701S MIPI-DSI, 480×800 RGB565, backlight GPIO 23 (LEDC) |
| Touch | GT911, I²C **port 1** (SDA 7 / SCL 8), RST & INT not connected |
| Audio | ES8311 — MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 48, PA 11 |
| SD | SDMMC slot 0, 4-bit, on-chip LDO ch 4, `/sd` |
| Radio | ESP32-C6 via ESP-Hosted, SDIO **slot 1** — reset 54, CLK 18, CMD 19, D0–D3 14–17 |

The SDIO pins are **module-internal** to the JC-ESP32P4-M3, identical on every board using it. The
C6's SDIO slave pins are IOMUX-fixed in silicon (CMD 18 / CLK 19 / D0–D3 20–23), so a slave build
cannot get them wrong.

`gt911_touch_init()` resolves the I²C bus by **port number** (`i2c_master_get_bus_handle(1, …)`)
rather than taking a handle, so the shared bus must be created on port 1. `bsp_init()` does that.

---

## Layout

```
components/bsp/          board support + subsystems
  bsp.c                  I2C, PPA, backlight, panel, touch, audio, SD
  bsp_wifi.c             station, NVS credentials, backoff retry, link watchdog
  bsp_wifi_prov.c        SoftAP setup portal + captive DNS
  bsp_wifi_store.c       per-SSID passwords on the SD card
  bsp_ble.c              NimBLE host, advertising, multi-connection
  bsp_ota.c              firmware update: HTTP(S), SD, streaming API
  bsp_ota_web.c          web page: control panel, upload, screenshots
  bsp_lvgl.c             LVGL binding for panel + touch
  bsp_remote.c           shared control state, persisted to SD
  bsp_screenshot.c       lv_snapshot -> PNG
  bsp_copro.c            C6 firmware push over SDIO
components/{st7701_lcd,gt911_touch,ppa_engine,audio,pngdec,pngaux}/
main/{main,ui,ble_control,ble_uart,ble_console}.c
tools/                   PC clients (see below)
```

The C6 slave **source** is not tracked — only the built image
(`components/bsp/network_adapter.bin`), which is what the firmware embeds. See pin #1 for how to
recreate the build tree.

There is deliberately **no `odroid` component**. That layer in `RetroESP32_P4` is an
Odroid-Go/ILI9341 shim for emulators (virtual 320×240 framebuffer, per-emulator draw calls); none
of it applies here. `components/bsp` replaces it at native resolution.

---

## Four things pinned on purpose

Each looks like a defect. Changing any of them breaks something.

### 1. C6 slave firmware stays at 2.0.17

[esp-hosted-mcu #115](https://github.com/espressif/esp-hosted-mcu/issues/115) (open) breaks NimBLE
on slave firmware **newer than 2.5.1**: WiFi keeps working while every HCI command times out
(`BLE_HS_ETIMEOUT_HCI`, `ogf=0x03 ocf=0x0003` = HCI_Reset), so BLE never advertises. Confirmed on
this board — broken on 2.12.11 in both SDIO modes, working on 2.0.17.

The transport logs `Version mismatch: Host [2.12.0] > Co-proc [0.0.0] ==> Upgrade co-proc`. It is
**cosmetic**; following its advice reintroduces the bug.

The built image is tracked at `components/bsp/network_adapter.bin`, so the project builds without
the slave source. The source trees are **not** in the repo (third-party, and large). To rebuild:

```powershell
# 1. Fetch esp_hosted 2.0.17 via a throwaway project
mkdir c6fetch; cd c6fetch
# main/idf_component.yml:  dependencies: {espressif/esp_hosted: "<2.6.0"}
idf.py set-target esp32c6      # resolves to 2.0.17

# 2. Copy slave/ and common/ out of managed_components/espressif__esp_hosted
#    into a sibling layout: <work>/slave and <work>/common
# 3. In slave/sdkconfig.defaults.esp32c6 add:
#      CONFIG_ESP_SDIO_STREAMING_MODE=n      # must match the host, see pin #2
# 4. idf.py set-target esp32c6 && idf.py build
```

Then copy `build/network_adapter.bin` to `components/bsp/`, set `TARGET_*` in `bsp_copro.c` to the
version built, flash the host, and `POST /copro`. `TARGET_*` must equal the embedded image's
version or the auto-updater pushes it again on every boot.

### 2. SDIO packet mode on both sides

Host `CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_NONE`, slave `CONFIG_ESP_SDIO_STREAMING_MODE=n`.
Change them **together, slave first**: `transport_drv.c` does `assert(0)` — a boot loop, not a
warning — when the slave is streaming and the host is not. Host-streaming with a packet slave is
tolerated, which is what makes that ordering safe.

### 3. Never block inside an LVGL callback

`lv_timer` callbacks and widget event handlers run inside `lv_timer_handler()` **holding the LVGL
lock**. Anything slow there — an `esp_wifi_*` call (an RPC over SDIO to the C6), file I/O — holds
that lock while it waits. Every other task blocks on it, and because the idle tasks keep running
**no watchdog fires**: the board stops answering, stops logging, never reboots. It looks dead.

This happened: the Home tab's refresh timer called `bsp_wifi_get_rssi()` once a second. RSSI is now
sampled by `bsp_wifi`'s link monitor and `bsp_wifi_get_rssi()` returns a cached value, so it cannot
block whoever calls it.

The lock is recursive (`xSemaphoreTakeRecursive`), so re-entering it is fine. Blocking while
holding it is not.

### 4. Watch the stack in BLE callbacks

GATT callbacks run on the **NimBLE host task**. Ours reach into LVGL to draw the BLE monitor, and
that chain is deep. A single ~1 KB stack buffer in the log repaint overflowed the 4 KB default:

```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
app: Boot #85, last reset: PANIC (4)
```

The board rebooted, which from a connected PC is indistinguishable from the *client* crashing —
it sent debugging to the wrong machine for several rounds. Fixed with a `static` buffer plus
`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192`. Prefer static or heap over large stack buffers there.

---

## Lock ordering

Two paths touch both the NimBLE host lock and the LVGL lock:

- **Receive:** NimBLE task holds NimBLE lock → calls `ui_ble_log()` → takes LVGL lock
- **Send:** would be LVGL handler holds LVGL lock → calls NimBLE → wants NimBLE lock

Opposite orders deadlock. `ble_console_send()` therefore **queues**; a worker task holds neither
when it starts and takes them one at a time (NimBLE, then LVGL). Anything new that sends from the
UI must go through that queue.

---

## Diagnostics

Every boot logs why the last one ended, plus a counter in NVS:

```
app: Boot #85, last reset: PANIC (4)
app: alive 60 s | wifi 3 192.168.1.100 recov 0 | heap int 302339 (min 280435) | psram 32534128
```

`PANIC`, `TASK WATCHDOG`, `INTERRUPT WATCHDOG`, `BROWNOUT` are the ones that matter. The 30 s
heartbeat exists because console silence is otherwise ambiguous — an idle board and a wedged one
look identical. `recov` counts link-watchdog rescues.

Core dumps are **disabled** and there is no coredump partition, so serial is the only evidence.

> **When a client and the device interact, capture device serial before concluding anything about
> the host.** Two long debugging detours in this project came from trusting the symptom's location
> — a Windows app — while the device log said `PANIC`.

---

## Flash layout

16 MB, no factory partition: a factory image cannot be replaced over the air, and its presence
gives the bootloader somewhere to fall back to instead of the A/B pair.

| Partition | Offset | Size |
|---|---|---|
| bootloader | `0x2000` | *(P4-specific, not `0x0`)* |
| partition table | `0x8000` | |
| `nvs` | `0x9000` | 24 K |
| `otadata` | `0xF000` | 8 K |
| `phy_init` | `0x11000` | 4 K |
| `ota_0` | `0x20000` | 6 M |
| `ota_1` | `0x620000` | 6 M |
| `storage` (FAT) | `0xC20000` | 3968 K |

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a new image boots **once** on trial and must confirm
itself via `bsp_ota_mark_valid()`, or the bootloader reverts. `confirm_or_rollback()` in `main.c`
does that after a health check. Confirming is also a **precondition for the next update** —
`esp_ota_set_boot_partition()` refuses while the current image is on trial — so a build that never
marks itself valid can never be replaced over the air either.

**Bump `CONFIG_APP_PROJECT_VER` every release.** An image whose version matches the running one is
refused with `ESP_ERR_INVALID_VERSION`, which stops update loops but also blocks real updates if
the version never changes.

---

## Implementation notes

**Screenshots** are PNG written with *stored* (uncompressed) deflate blocks, one per row. The
bundled `pngdec` is decode-only (`inflate`, no `deflate`), and pulling in a compressor just for
screenshots was not worth it — the cost is ~1.1 MB per 480×800 shot. The P4 has a hardware JPEG
encoder if size ever matters, at the cost of artefacts on UI text.

**LVGL memory**: `CONFIG_LV_USE_CLIB_MALLOC` + `CONFIG_SPIRAM_USE_MALLOC`, so LVGL spills into
PSRAM. Do **not** switch to LVGL's built-in pool — `LV_MEM_SIZE_KILOBYTES` is a fixed array in
`.bss` and at any useful size the link fails with `--enable-non-contiguous-regions discards
section ...`, an internal-SRAM message that never mentions LVGL.

**Draw buffers** are two 100-line PSRAM buffers with partial rendering. Tear-free mode needs the
panel created with ≥2 framebuffers; `st7701_lcd_init()` creates one. To enable: raise `num_fbs` to
2 and set `avoid_tearing` in `bsp_lvgl.c` (+768 KB PSRAM).

**Advertising** carries flags, TX power and the name; the 128-bit service UUID goes in the **scan
response** because a 128-bit UUID needs 18 of the 31 bytes and the name another 11. Only one UUID
fits, and the NUS one earns it — terminal apps use it to decide whether to offer a console.

**NimBLE 128-bit UUIDs** are stored least-significant byte first, so `BLE_UUID128_DECLARE` takes
them reversed from how they are written.

---

## Known log noise

| Message | Meaning |
|---|---|
| `esp_lcd_panel_swap_xy(50): swap_xy is not supported` | `esp_lvgl_port` applies rotation config unconditionally; harmless |
| `ldo: The voltage value 0 is out of the recommended range` | `sd_pwr_ctrl_ldo_config_t` in 5.5.2 exposes only `ldo_chan_id`; the card mounts fine |
| `spi_flash: Detected boya flash chip but using generic driver` | enable `SPI_FLASH_SUPPORT_BOYA_CHIP` for optimal handling |
| `Version mismatch: Host [2.12.0] > Co-proc [0.0.0]` | expected — see pin #1 |
| `pngdec` `-Wimplicit-fallthrough` warnings | vendored zlib, pre-existing |

---

## PC tools

`tools/gution_terminal.py` → `tools/dist/GutionTerminal.exe` (PyInstaller, `--onefile
--windowed`). `tools/ble_client.py` is the CLI equivalent. Both need `pip install bleak`.

Three things that are load-bearing in the desktop client:

- **`winrt={"use_cached_services": False}`** — the nesting matters. Passed as a bare keyword it is
  silently ignored, and Windows serves a cached GATT database that can be stale or partial
  (recorded when an earlier connection was cut short mid-discovery). The symptom is
  `Characteristic ... was not found` for a service that plainly exists.
- **All BLE work serialised** behind an `asyncio.Lock` with ~50 ms pacing. Overlapping GATT
  operations on one client destabilise the WinRT backend.
- **Crash logging** to `GutionTerminal.log` via `report_callback_exception`, `sys.excepthook` and
  `threading.excepthook` — a windowed build has nowhere to print a traceback.

**Windows cannot bond a generic GATT peripheral.** Settings connects, enumerates and drops with
HCI `0x13` without ever attempting to pair; requesting pairing from the device side is *rejected*
and produces a tight connect/disconnect loop (tested — `bsp_ble_config_t.require_pairing` exists
but is `false` for that reason). Clients must talk to it unbonded. iOS is unaffected.

Regenerate documentation screenshots with the console's `tab <0-4>` command plus `GET /shot.png`.

---

## Not done

- Web page is **unauthenticated** and exposes firmware upload; `bsp_ota_web_config_t` takes
  credentials, they are simply unset
- `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` and images are unsigned — bench configuration
- No domain-specific application yet; this is the platform
