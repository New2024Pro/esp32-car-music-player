#pragma once
#include <stddef.h>
#include <stdint.h>

// SD card on VSPI. Verify against your actual dev board schematic.
// Never use GPIO 16/17 (PSRAM) or strapping pins 0, 2, 5, 12, 15.
#define PIN_SD_SCK  18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23
#define PIN_SD_CS   25

// SoftAP hotspot. CHANGE AP_PASS (WPA2 needs >= 8 chars).
#define AP_SSID "esp32-mp3"
#define AP_PASS "change-me-1234"

// Initial Bluetooth discovery name. Placeholder value disables pairing.
// Apostrophe variants (` ' and the typographic one iOS uses) match each other.
#ifndef AIRPODS_NAME
#define AIRPODS_NAME "Saunder`s AirPods Pro"
#endif
#define AIRPODS_NAME_PLACEHOLDER "CHANGE_ME_AIRPODS_NAME"

// PCM ring in PSRAM. Power of two. 256 KiB = ~1.5 s of 44.1 kHz stereo 16-bit.
#define RING_BYTES    (256 * 1024)
#define PREFILL_BYTES (64 * 1024)

// Recoverable-wait timeouts (ms).
#define WORKER_STOP_TIMEOUT_MS 2000
#define DRAIN_TIMEOUT_MS       200
#define PREFILL_TIMEOUT_MS     4000
#define OPEN_TIMEOUT_MS        3000

