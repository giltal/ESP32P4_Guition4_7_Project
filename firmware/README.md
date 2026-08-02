# Prebuilt firmware

Ready-to-flash binaries for the **Guition JC4880P443C-I-W**, so you can run the board without
installing ESP-IDF.

| File | What it is |
|---|---|
| `gution_p4.bin` | the application — this is the one for over-the-air updates |
| `bootloader.bin` | second-stage bootloader |
| `partition-table.bin` | flash layout |
| `ota_data_initial.bin` | tells the bootloader which slot to start from |

Version **1.0.0** · `gution_p4.bin` SHA-256
`33e67b28acd66cd69e97fba1d00f9e63202276b243a628a47a7ad5d3894b3c65`

---

## Updating a board that already runs this firmware

Only `gution_p4.bin` is needed. Pick whichever is convenient:

- **Browser** — open the board's IP address, Firmware tab, upload the file
- **SD card** — copy it onto the card as `firmware.bin` and restart the board

The previous version is kept, and the board falls back to it automatically if the new one fails to
start.

> An update is **refused if it has the same version number** as what is already running — that is
> what stops a board reinstalling the same image forever. To publish a genuine update, raise
> `CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults` before building.

---

## Flashing a blank board

Needs [esptool](https://pypi.org/project/esptool/) (`pip install esptool`) and a USB cable:

```bash
esptool --chip esp32p4 -b 460800 write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000   bootloader.bin \
  0x8000   partition-table.bin \
  0xf000   ota_data_initial.bin \
  0x20000  gution_p4.bin
```

The bootloader sits at **0x2000**, not `0x0` — that is specific to the ESP32-P4. Replace the port
with your own if esptool does not find it (`--port COM11`, or `/dev/ttyUSB0` on Linux).

The ESP32-C6 co-processor is **not** touched by this. It keeps its own firmware, which the
application updates itself when needed.
