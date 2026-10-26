#pragma once
// Personal settings. Copy this file to include/user_config.h (git-ignored) and edit it.
// Everything here is optional.

// Name of your headphones / speaker to connect to on first boot, as it appears on a phone.
// #define AIRPODS_NAME "Your AirPods Pro"

// Car navi preset, read off the navi's Bluetooth registration screen (Toyota: 機器登録).
// The ESP32 connects to this address directly, no scan needed. Applied once on the first boot
// after flashing; a device picked later on the web page is kept.
// #define NAVI_NAME "CAR MULTIMEDIA"                          // 機器名称
// #define NAVI_ADDR {0x7C, 0x66, 0x9D, 0x00, 0x00, 0x00}      // Bluetooth アドレス, 6 bytes
// #define NAVI_PIN  "0000"                                    // PINコード (legacy-PIN navis only)
