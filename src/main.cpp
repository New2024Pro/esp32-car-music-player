#include <Arduino.h>
#include <WiFi.h>
#include <BluetoothA2DPSource.h>
#include <MP3DecoderHelix.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include "config.h"

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("flash=%u psram=%d free_psram=%u reset=%d\n", ESP.getFlashChipSize(),
                psramFound(), ESP.getFreePsram(), (int)esp_reset_reason());
}

void loop() { delay(1000); }
