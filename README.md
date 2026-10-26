# ESP32 Car Music Player

Play MP3s from an SD card through your car's navi or Bluetooth headphones, with an ESP32 instead of a smartphone.

The ESP32 acts as a Bluetooth audio source, the same role a phone plays when it streams music to a car. It pairs with the navi, streams the SD card's MP3s, and responds to the navi's and the steering wheel's track buttons. Your phone stays free, and the music library lives in the car.

**Status:** working on a Toyota **NSZN-W64T** navi and on AirPods Pro. Other head units are untested, which is where help is most needed (see [Contributors wanted](#contributors-wanted)).

## Features

| Feature | Notes |
|---|---|
| Bluetooth A2DP audio source (SBC) | Streams to car navis, speakers, headphones |
| Car navi pairing | Secure Simple Pairing and legacy PIN; "same code?" can be auto-confirmed |
| Navi-initiated connections | The navi connects to the ESP32 on registration and on every ignition |
| Navi / steering-wheel / AirPods buttons (AVRCP) | Play, stop/pause, next, previous, fast-forward, rewind |
| Automatic reconnect | Backed-off reconnect to the saved device; the navi can also reconnect by itself |
| Web remote over Wi-Fi hotspot | Track list, seek bar, volume, shuffle, repeat (off / all / one), pairing prompts |
| SD card library | MP3s in the card's root folder, 44.1 kHz stereo |
| Physical and serial control | BOOT button: click = play/pause, hold = next / scan; serial console commands |

## Hardware

- **ESP32 with PSRAM** (ESP32-WROVER). The original ESP32 is required: the S2, S3, C3 and C6 have no Bluetooth Classic, so they cannot stream A2DP audio.
- **microSD card**, FAT32, MP3 files in the root folder. Developed on an ESP32 board with an on-board SD slot (SD_MMC, 1-bit); an SPI SD module also works (pins in `include/config.h`).
- **USB power** from the car.

## Build and flash

Uses [PlatformIO](https://platformio.org/).

```sh
cp include/user_config.example.h include/user_config.h   # optional: your device names / navi address
pio run -e esp32 -t upload        # firmware
pio run -e esp32 -t uploadfs      # web page (web/index.html, gzipped into LittleFS)
pio device monitor -b 115200
```

Host-side unit tests (play order, seek maths, ring buffer, parameter parsing):

```powershell
test/host/run.ps1
```

## Pairing with a car navi

1. Optional but fastest: copy the navi's name, Bluetooth address and PIN from its registration screen into `include/user_config.h` (`NAVI_NAME`, `NAVI_ADDR`, `NAVI_PIN`), then flash. The ESP32 then connects to that address directly.
2. On the navi, open Bluetooth device registration (Toyota: 機器登録).
3. Power the ESP32. It connects, pairs, and confirms the code. If the navi then says it will connect from the car side (車載機からの接続に切り替えます), accept; the ESP32 accepts incoming connections.
4. Select Bluetooth Audio on the navi.

Without a preset: join the `ESP32-Music` Wi-Fi, open `http://192.168.4.1`, tap the device pill, **Scan Nearby**, and pick the navi.

[docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md) has the full test procedure.

## Known limitations

- **No song titles on the navi.** The Bluetooth stack in this Arduino core cannot answer the navi's track-info (AVRCP metadata) requests.
- **The web page stalls while audio streams.** Wi-Fi and Bluetooth share one radio and the internal RAM is nearly full. The page works while paused or disconnected.
- **44.1 kHz stereo MP3 only.** Other sample rates are skipped.
- **Positions in VBR files are estimates.** Seek uses the first frame's bitrate.
- **No hands-free calling (HFP).** Navis list the phone service as unsupported; that is expected.

## Contributors wanted

This project aims to replace the smartphone as a car's music source with a small, cheap, always-in-the-car device. Help is welcome, especially from people with experience in:

- **Bluetooth Classic / ESP-IDF Bluedroid:** AVRCP metadata and play-status notifications (song titles on the navi), A2DP source quirks.
- **Car head units:** testing with other navis (Toyota, Honda, Nissan, Pioneer, Kenwood, Panasonic, aftermarket units) and reporting what works.
- **ESP32 memory and Wi-Fi / Bluetooth coexistence:** moving Bluetooth and Wi-Fi buffers to PSRAM (a custom ESP-IDF build), so the web page works while streaming.
- **Audio decoding:** resampling 48 kHz files, accurate VBR seeking (Xing/VBRI headers), more formats.
- **Embedded web UI:** keeping the phone remote small, robust and usable in a car.

To report a head unit, open an issue with the model, the pairing screens you saw, and the serial log (`pio device monitor`). The `[bt]` lines show each pairing and connection step.

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to get started.
