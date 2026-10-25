# ESP32 WROVER-E MP3 Player: Design (rev 2)

## Intent
Play MP3 files from an SD card on an ESP32-WROVER-E and stream them to Apple AirPods over Bluetooth Classic (A2DP Source, SBC). Control it from an iPhone browser via a page hosted by the ESP32 on its own Wi-Fi hotspot. The user's PRD diagram is reference only: the ESP32-S3 plus external BT module is replaced by the WROVER-E, which has A2DP built in.

## Goals
- MP3 files in the SD root play in order.
- Web UI: play/pause, next, previous, volume, track list (tap to play), BT state.
- Pair with AirPods by name once, then reconnect to the remembered address.
- One PlatformIO project, no external BT hardware.

## Non-goals
- No SD file upload/management, no playlists beyond folder order, no ID3 art, no OTA, no hotspot auth beyond WPA2.
- MP3 only. No FLAC/AAC/WAV input.
- No internet for the phone while on the hotspot.

## Anti-goals
- Audio that stutters because Wi-Fi/HTTP/SD work blocks the audio path.
- Using GPIO 16/17 (PSRAM) or any strapping pin (0, 2, 5, 12, 15) for SD.

## Constraints
- Hardware: ESP32-WROVER-E (flash size and PSRAM must be verified at boot; see Boot checks). Classic BT and Wi-Fi share one radio. Espressif lists SoftAP + BT Classic as supported with unstable performance; a larger buffer cannot guarantee smooth RF.
- Codec choice: this project uses Bluetooth Classic A2DP with SBC. (AirPods also support AAC; SBC is our choice.)
- Audio format: 44.1 kHz, stereo MP3 → signed 16-bit stereo PCM (the A2DP library's source baseline). Any other sample rate or channel count is rejected with a clear message in `/api/state` and the serial log, and the track is skipped. Conversion is out of scope.
- SD on SPI (VSPI): SCK=18, MISO=19, MOSI=23, **CS=25** (non-strapping, exposed). Pin set lives in `config.h`; verify against the actual dev board schematic. 3.3 V card module.
- SoftAP: SSID `esp32-mp3`, WPA2 password in `config.h`, address `192.168.4.1`.

## Approaches
1. **Arduino + libs (chosen):** `ESP32-A2DP` (pschatzmann), `arduino-audio-tools` + `libhelix`, `ESP32Async/ESPAsyncWebServer` + `AsyncTCP`. Smallest code; less tuning control than IDF.
2. ESP-IDF native: more control, ~3x code. Rejected for size.
3. ESP32-S3 + external BT module: more cost and wiring, no gain. Rejected.

## Design

### Tasks and ownership
- **Decoder worker task** (FreeRTOS, pinned to core 1): owns SD reads and MP3 decode. Writes PCM into a ring buffer in PSRAM. Never touched by HTTP or the A2DP callback.
- **A2DP data callback**: only copies already decoded PCM out of the ring buffer. It never waits on SD, decode, or HTTP. On underrun it outputs silence and increments an underrun counter.
- **Player controller task**: sole owner of file changes, pause/resume, decoder reset, and ring buffer flush. It consumes a command queue.
- **Web handlers** (async): only enqueue commands (`PLAY`, `PAUSE`, `NEXT`, `PREV`, `VOLUME n`, `TRACK i`) and read a small state snapshot. They never touch the decoder, files, or buffer.

### Resource ownership (precise)
The controller decides what happens. The decoder worker performs all file operations and decoder resets. Track change sequence:
1. Controller sets `worker.stopRequested` and the A2DP source to output silence (ring buffer marked `draining`).
2. Worker finishes its current decode step, closes the file, and sends an acknowledgement (task notification) to the controller.
3. Controller, after the ack, calls `ring.flush()` (safe: producer is stopped, consumer only reads the `draining` flag and outputs silence).
4. Controller sends `OPEN(index)` to the worker; worker opens the file and decodes the first frames.
5. Controller waits for prefill (ring at or above `PREFILL_BYTES`), then clears `draining` and resumes.
The A2DP callback never blocks and never touches files, the decoder, or HTTP at any point in this sequence.

### Ring buffer synchronization
- Single-producer (decoder worker) / single-consumer (A2DP callback) byte ring in PSRAM with atomic head/tail indices (acquire/release ordering). Capacity is a multiple of the 4-byte frame size.
- `write()` is called only by the worker; `read()` only by the callback; `flush()` only by the controller and only while the worker is stopped and acknowledged (see above), so reads, writes and flushes never race.
- On underrun the callback copies the available complete PCM frames (whole 4-byte frames only) and zero-fills the remainder. An underrun is counted only when playback is expected to supply audio (state `PLAYING`, not `draining`, not `PAUSED`, not during prefill).

### Track list and navigation
- Tracks are the `*.mp3` files in the SD root (case-insensitive), sorted deterministically by filename: case-insensitive byte order, ties broken by exact byte order. Hidden/dot files (including macOS `._*`) are ignored.
- `GET /api/tracks?offset=0&limit=50` returns `{total, offset, items:[{i,name}]}`. `limit` is clamped to 1..50; `offset >= total` returns an empty `items`. Loaded once on page load and on demand, separately from the 2 s `/api/state` polling.
- End of list: after the last track, playback stops (state `STOPPED`); `NEXT` on the last track and `PREV` on the first stay put. No wrap-around.
- Bounded skip policy: an unsupported or unreadable track is skipped with an error recorded. If skipping reaches `total` consecutive failures, playback stops with error `no playable tracks` instead of cycling.

### Bluetooth connection
- `AIRPODS_NAME` in `config.h` is used for initial discovery only. After the first successful connection, store the peer address in NVS and reconnect to it.
- Retry with exponential backoff (1 s up to 30 s). State is reported as `disconnected`, `connecting`, or `connected` separately.
- First pairing needs AirPods in pairing mode: enter pairing mode using the procedure for the actual AirPods model.
- Headphone reconnection is recorded separately in acceptance: whether playback resumes automatically or waits for Play. Default: waits for Play (controller state stays `PAUSED`).

### Web UI
- `GET /` embedded HTML (PROGMEM), `GET /api/state` (track, play state, volume, BT state, error, diagnostics), `POST /api/play|pause|next|prev`, `POST /api/volume?v=0-100`, `POST /api/track?i=N`.
- Page polls `/api/state` every 2 s. Compact, no JS libraries. Built only after the audio path is proven.

### Build config
- `platformio.ini`: `framework = arduino`, `board = esp-wrover-kit` (valid ID; verify it suits the actual dev board), `huge_app.csv` partitions, `-DBOARD_HAS_PSRAM`.
- Pin the PlatformIO platform, Arduino core, ESP32-A2DP, AudioTools, libhelix, ESP32Async/ESPAsyncWebServer, and AsyncTCP versions after a minimal compatibility build succeeds. No floating versions in the final `platformio.ini`.

### Boot checks
Log at boot: flash size, `psramFound()` and free PSRAM, SD init result, track count. `BOARD_HAS_PSRAM` alone does not prove PSRAM initialized; refuse to start playback if the ring buffer allocation in PSRAM fails.

### Diagnostics
Track and expose (serial + `/api/state`): PCM underrun count, minimum free internal heap, reset reason, current BT state.

## Build order (audio path first)
1. Minimal compatibility build (pin versions).
2. Boot checks + SD scan.
3. MP3 → ring buffer → A2DP → AirPods, no Wi-Fi.
4. Controller task and command queue.
5. SoftAP + web UI.

## Acceptance
Hardware runs are required before claiming reliable playback. The 10-minute run is a smoke test only.
1. MP3 → AirPods, Wi-Fi disabled: 30 min.
2. Same playback with SoftAP, iPhone connected, 2 s polling: 60 min. During it, exercise track browsing, page reloads, volume changes, and phone Wi-Fi reconnects.
3. Record audible dropouts, PCM underruns, minimum free internal heap, and resets.

**Pass criteria** for runs 1 and 2: zero uncommanded audible dropouts, zero steady-state PCM underruns, zero crashes/resets. Intentional pauses and track transitions are excluded. Headphone reconnection is recorded separately (auto-resume vs waits for Play).

**First hardware gate: SoftAP + A2DP coexistence.** Wi-Fi tuning (polling interval, TX power, channel) is a builder decision. The hotspot is a product requirement: if coexistence testing fails after tuning, dropping it returns to the user for a decision.

## Left to builder
Buffer size tuning, Wi-Fi channel, HTML styling, exact pinned versions.

## Calls I made (not user decisions)
- CS=25 pending schematic check; `huge_app.csv`; SSID `esp32-mp3`; polling instead of WebSocket; decoder pinned to core 1; NVS for the peer address; backoff 1–30 s.
- Self-reviewed the doc rather than dispatching a builder-check subagent.
