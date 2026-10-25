# Hardware validation (PENDING: no results supplied yet)

None of the checks below has been run. Smooth playback is **unverified** until you report results.

## Firmware

| File | Use |
|---|---|
| `.pio/build/esp32/firmware.bin` (env `esp32`) | Full product: SD + A2DP + SoftAP + web UI |
| `.pio/build/esp32_nowifi/firmware.bin` (env `esp32_nowifi`) | Acceptance run 1: Wi-Fi off, serial console only |

Build/flash (existing PlatformIO at `C:\Users\Thinkpad\.platformio\penv\Scripts\pio.exe`):

```
pio run -e esp32_nowifi -t upload
pio device monitor -b 115200
```

## Before flashing: edit `include/config.h`

- `AP_PASS`: set your own WPA2 password (>= 8 chars). The default `change-me-1234` is public.
- `AIRPODS_NAME` is set to `Saunder`s AirPods Pro` (backtick as given). The apostrophe variants `` ` ``, `'` and the typographic `’` iOS uses are treated as equal. The rest must match exactly (case-sensitive). If the monitor prints `[bt] discovered "..."` lines that never say `match=1`, copy the printed name into `AIRPODS_NAME`.
- Pins: verify `PIN_SD_*` against your board's schematic.

## Wiring (SD card module, 3.3 V, SPI)

| SD module | ESP32 GPIO |
|---|---|
| SCK | 18 |
| MISO | 19 |
| MOSI | 23 |
| CS | **25** (not a strapping pin) |
| VCC | 3V3 (or 5 V only if the module has a regulator and level shifter) |
| GND | GND |

Do not use GPIO 16/17 (PSRAM) or strapping pins 0, 2, 5, 12, 15. If you use an ESP32-WROVER-KIT, its on-board SD slot is wired to SDMMC pins and the LCD uses other pins; use a separate SPI module on the pins above and check for conflicts.

SD card: FAT32, MP3 files in the root, 44.1 kHz stereo only. Put 3-4 files on it; add one 48 kHz or mono file for the skip test. Ignore `._*` files.

## Gate 0: boot checks (2 min)

1. Flash `esp32_nowifi`, open the monitor.
2. Expected within 2 s of boot: `[boot] flash=4194304 B psram_found=1 free_psram=...` (flash may differ by module), `[sd] N tracks`. If `psram_found=0` or `FATAL: PSRAM ring allocation failed`, stop and report.

## Run 1: audio without Wi-Fi (30 min)

1. Put the AirPods in pairing mode using the procedure for your AirPods model.
2. Wait for `[bt] connection state: 2` (connected). Then send `p` in the monitor.
3. Expected: audio plays, `[diag]` every 10 s shows `bt=connected play=playing underruns=0`.
4. During 30 min: let tracks advance by themselves. Send `n`, `r`, `s`, `p`, `+`, `-` a few times.
5. Record: audible dropouts (count + times), final `underruns`, lowest `minHeap`, any reboot (`[boot]` line appears again).
6. Pass: 0 uncommanded dropouts, 0 steady-state underruns (a few counts right at a track change are excluded: note them separately), 0 resets.
7. Extra check: send `n` or `r` 30 times quickly. Expected: no reset, audio recovers, `err` stays empty or reports a clear message.
8. Reconnection (record separately): close the case lid so AirPods disconnect, then reopen near the ESP32. Report whether `bt=connected` returns by itself and whether audio resumes by itself or waits for `p`. Expected: reconnects; waits for `p` only if you paused.

## Run 2: coexistence with hotspot (60 min)

1. Flash `esp32` (Wi-Fi on). On the iPhone join Wi-Fi `esp32-mp3`, open `http://192.168.4.1`.
2. Expected: page shows track list, `BT: connected`, status refreshes every 2 s. `curl`-style checks from a laptop on the same AP:
   - `GET /api/state` returns JSON.
   - `GET /api/tracks?offset=0&limit=999` returns at most 50 items.
   - `POST /api/volume?v=abc` returns HTTP 400.
3. During 60 min: tap tracks, reload the page, change volume, turn iPhone Wi-Fi off/on a few times, browse tracks.
4. Record and pass criteria: same as Run 1.
5. **Gate:** if you hear dropouts here but not in Run 1, report it. Wi-Fi tuning (TX power, channel, poll interval) is mine to try. If tuning fails I will come back to you before dropping the hotspot.

## What to send back

Per run: duration, dropout count with times, final `underruns`, lowest `minHeap`, resets, reconnection behavior, any `err=` text, and the first 20 lines of the boot log.

