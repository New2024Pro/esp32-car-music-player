# Contributing

Thanks for helping. Reports from real cars are as valuable as code.

## Reporting a car navi or head unit

Open an issue titled with the head unit model (for example "Toyota NSZN-W64T") and include:

- Which pairing screen appeared (PIN, "same code?", or a code to type in) and what the navi said afterwards. Photos of the screens help.
- The serial log from boot until the failure or success (`pio device monitor -b 115200`). The `[bt]` lines show each step: link up/down with reason codes, pairing, audio stream state, and every remote button press.
- What works: audio, buttons (next / previous / play / stop / fast-forward / rewind), reconnect after ignition off and on.

## Code changes

1. Fork, create a branch, keep changes focused.
2. Build both firmware variants: `pio run -e esp32 -e esp32_nowifi`.
3. Run the host tests: `test/host/run.ps1` (needs clang++). Put new pure logic in `lib/core/` with a test in `test/host/`.
4. If you changed `web/index.html`, test it in a browser and upload it with `pio run -t uploadfs`.
5. Describe in the pull request what you tested on hardware, with which head unit or headphones.

## Code layout

| Path | What |
|---|---|
| `src/BtAudio.*` | Bluetooth: A2DP source, pairing, reconnect, AVRCP buttons, incoming connections |
| `src/Controller.*` | Playback decisions: play order, shuffle/repeat, seek, remote button mapping |
| `src/DecoderWorker.*` | SD reading and MP3 decoding into the PCM ring buffer |
| `src/WebApi.cpp`, `web/index.html` | Wi-Fi hotspot, HTTP API and the phone web page |
| `lib/core/` | Hardware-free logic (host-tested) |
| `include/config.h` | Pins, timeouts, Bluetooth options; personal values go in `include/user_config.h` |

Do not commit `include/user_config.h`: it holds your device names and your car's Bluetooth address.
