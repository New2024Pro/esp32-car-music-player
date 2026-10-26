#pragma once
#include <stddef.h>
#include <stdint.h>

// SD card on VSPI.
#define PIN_SD_SCK  18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23
#define PIN_SD_CS   5

// SoftAP hotspot (open network: fresh SSID + no WPA2 handshake).
#define AP_SSID "ESP32-Music"
#define AP_PASS ""

// Initial Bluetooth discovery name. Placeholder value disables pairing.
// Apostrophe variants (` ' and the typographic one iOS uses) match each other.
#ifndef AIRPODS_NAME
#define AIRPODS_NAME "Saunders’s AirPods Pro"
#endif
#define AIRPODS_NAME_PLACEHOLDER "CHANGE_ME_AIRPODS_NAME"

// Name this ESP32 shows in the car navi's device list.
#define BT_LOCAL_NAME "ESP32 Music"

// Pairing (first connection to a car navi). The navi shows a code; the ESP32 reports the
// pending step on the web UI and serial and waits for the user:
//   - "Is the code the same?" (Secure Simple Pairing): BOOT click / web Confirm / serial 'y'
//   - PIN / passkey entry: web UI field or serial "k<digits>" + Enter
// BT_AUTO_CONFIRM 1 accepts the "same code?" step without waiting (only the target name is
// ever connected, so this mainly skips a button press).
#define BT_AUTO_CONFIRM 0
// ESP_BT_IO_CAP_IO (display + yes/no) gives the "same code?" flow on SSP navis.
// ESP_BT_IO_CAP_IN (keyboard) makes SSP navis show a code to type in here instead.
#define BT_IO_CAP ESP_BT_IO_CAP_IO
// 1 = advertise as a smartphone. Try this if the navi refuses or ignores the ESP32.
#define BT_COD_AS_PHONE 0

#define PAIRING_TIMEOUT_MS    30000  // user has this long to answer a pairing prompt
#define BT_CONNECT_TIMEOUT_MS 8000   // page/connect attempt without pairing activity
#define BT_RECONNECT_MIN_MS   10000  // saved-peer reconnect backoff (car navi boots slower than us)
#define BT_RECONNECT_MAX_MS   60000
// Give up auto-reconnect after this many tries (~5 min); the navi appears well within that.
// Each try pages for up to ~5 s and starves the Wi-Fi hotspot, so it must not run forever.
#define BT_RECONNECT_MAX_TRIES 8
// No reconnect paging this long after a phone joins the hotspot (join + DHCP + first page load).
#define BT_QUIET_AFTER_WIFI_JOIN_MS 30000

// PCM ring in PSRAM. Power of two. 256 KiB = ~1.5 s of 44.1 kHz stereo 16-bit.
#define RING_BYTES    (256 * 1024)
#define PREFILL_BYTES (64 * 1024)

// Recoverable-wait timeouts (ms).
#define WORKER_STOP_TIMEOUT_MS 2000
#define DRAIN_TIMEOUT_MS       200
#define PREFILL_TIMEOUT_MS     4000
#define OPEN_TIMEOUT_MS        3000

