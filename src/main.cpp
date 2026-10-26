#include <Arduino.h>
#include <SD_MMC.h>
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "BtAudio.h"
#include "Controller.h"
#include "DecoderWorker.h"
#include "RingBuffer.h"
#include "TrackList.h"
#include "config.h"
#include "WebApi.h"
#include "Params.h"

#ifndef ENABLE_WIFI
#define ENABLE_WIFI 1
#endif
#if ENABLE_WIFI
#include <WiFi.h>
#endif

static DecoderWorker worker;
static BtAudio bt;
static Controller ctl;
static std::vector<std::string> tracks;

static void halt(const char* why) {
  Serial.printf("FATAL: %s\n", why);
  for (;;) delay(1000);
}

static void scanSd(std::string& err) {
  bool sdOk = false;

  // 1. Try built-in SD slot via SD_MMC (1-bit mode: CLK=14, CMD=15, D0=2)
  Serial.println("[sd] Probing on-board SD slot (SD_MMC 1-bit mode)...");
  if (SD_MMC.begin("/sdcard", true)) {
    uint8_t cardType = SD_MMC.cardType();
    if (cardType != CARD_NONE) {
      sdOk = true;
      Serial.printf("[sd] On-board SD_MMC detected! Type: %d, Size: %lluMB\n",
                    cardType, SD_MMC.cardSize() / (1024 * 1024));
      std::vector<std::string> names;
      File root = SD_MMC.open("/");
      for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        if (!e.isDirectory()) {
          std::string fname = e.name();
          if (isTrackName(fname)) names.push_back(fname);
        }
      }
      tracks = buildTrackList(names);
    }
  }

  // 2. Fallback to SPI if SD_MMC did not detect card
  if (!sdOk) {
    Serial.println("[sd] Probing SPI SD slot fallback...");
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (SD.begin(PIN_SD_CS, SPI, 4000000)) {
      sdOk = true;
      Serial.println("[sd] SPI SD detected!");
      std::vector<std::string> names;
      File root = SD.open("/");
      for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        if (!e.isDirectory()) {
          std::string fname = e.name();
          if (isTrackName(fname)) names.push_back(fname);
        }
      }
      tracks = buildTrackList(names);
    }
  }

  if (!sdOk) {
    err = "SD init failed";
    Serial.println("[sd] Could not initialize on-board SD_MMC or SPI SD.");
    return;
  }

  if (tracks.empty()) err = "no tracks";
  Serial.printf("[sd] %u tracks loaded\n", (unsigned)tracks.size());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("[boot] flash=%u B psram_found=%d free_psram=%u reset_reason=%d\n", ESP.getFlashChipSize(),
                (int)psramFound(), ESP.getFreePsram(), (int)esp_reset_reason());

  // BOARD_HAS_PSRAM does not prove PSRAM initialized: refuse to run without the ring in PSRAM.
  uint8_t* mem = (uint8_t*)heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
  if (!mem) halt("PSRAM ring allocation failed");
  static RingBuffer ring(mem, RING_BYTES);

  std::string err;
  scanSd(err);

  worker.begin(ring, tracks);
  bool btOk = bt.begin(ring);
  if (!btOk) err = "AIRPODS_NAME not set";
  ctl.begin(worker, bt, ring, tracks, err);
#if ENABLE_WIFI
  if (!webApiBegin(ctl)) Serial.println("[web] not started");
#else
  Serial.println("[web] disabled (ENABLE_WIFI=0): serial console only");
#endif
  pinMode(0, INPUT_PULLUP);  // BOOT button
  Serial.println("[cmd] BOOT btn: click=play/pause (or confirm pairing) hold=next/scan | Serial: p=play s=pause n=next "
                 "r=prev +=vol up -=vol down | pairing: y=confirm x=reject k<digits>+Enter=PIN/passkey");
}

void loop() {
  static uint32_t lastDiag = 0;
  static uint32_t btnDownAt = 0;
  static bool btnHeldHandled = false;
  static bool readingCode = false;  // serial "k<digits>" + Enter
  static std::string code;

  // On-board BOOT button (GPIO 0, active LOW): click = Play/Pause, hold >= 700ms = Next Track (or BT Scan if disconnected)
  if (digitalRead(0) == LOW) {
    if (btnDownAt == 0) {
      btnDownAt = millis();
      btnHeldHandled = false;
    } else if (!btnHeldHandled && millis() - btnDownAt >= 700) {
      btnHeldHandled = true;
      StateSnapshot s = ctl.snapshot();
      if (s.bt != BtState::Connected) ctl.triggerBtScan();
      else ctl.enqueue({Cmd::Next, 0});
    }
  } else if (btnDownAt != 0) {
    uint32_t dur = millis() - btnDownAt;
    btnDownAt = 0;
    if (!btnHeldHandled && dur >= 40) {
      StateSnapshot s = ctl.snapshot();
      if (s.pair == PairKind::Confirm) ctl.answerBtPairing(true, "");  // car navi: "same code?" -> yes
      else ctl.enqueue({s.play == PlayState::Playing ? Cmd::Pause : Cmd::Play, 0});
    }
  }
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (readingCode) {
      if (c >= '0' && c <= '9') {
        if (code.size() < 16) code += c;
      } else if (c == '\r' || c == '\n') {
        readingCode = false;
        bool ok = ctl.answerBtPairing(true, code) || ctl.setBtPin(code);
        Serial.printf("[cmd] code %s %s\n", code.c_str(), ok ? "sent" : "rejected (1-16 digits)");
      }
      continue;
    }
    switch (c) {
      case 'p': ctl.enqueue({Cmd::Play, 0}); break;
      case 's': ctl.enqueue({Cmd::Pause, 0}); break;
      case 'n': ctl.enqueue({Cmd::Next, 0}); break;
      case 'r': ctl.enqueue({Cmd::Prev, 0}); break;
      case '+': ctl.enqueue({Cmd::Volume, clampVolume(ctl.snapshot().volume + 10)}); break;
      case '-': ctl.enqueue({Cmd::Volume, clampVolume(ctl.snapshot().volume - 10)}); break;
      case 'y': if (!ctl.answerBtPairing(true, "")) Serial.println("[cmd] no pairing prompt to confirm"); break;
      case 'x': if (!ctl.answerBtPairing(false, "")) Serial.println("[cmd] no pairing prompt to reject"); break;
      case 'k': readingCode = true; code.clear(); break;
    }
  }
  if (millis() - lastDiag >= 10000) {
    lastDiag = millis();
    StateSnapshot s = ctl.snapshot();
#if ENABLE_WIFI
    int sta = WiFi.softAPgetStationNum();
#else
    int sta = 0;
#endif
    Serial.printf("[diag] t=%us play=%s bt=%s pair=%s track=%d/%d vol=%d underruns=%u heap=%u maxBlk=%u minHeap=%u "
                  "psram=%u phones=%d err=\"%s\"\n",
                  (unsigned)(millis() / 1000), toStr(s.play), toStr(s.bt), toStr(s.pair), s.track, s.trackCount, s.volume,
                  (unsigned)s.underruns, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)s.minFreeHeap,
                  ESP.getFreePsram(), sta, s.error.c_str());
  }
  delay(20);
}

