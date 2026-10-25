#include <Arduino.h>
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

#ifndef ENABLE_WIFI
#define ENABLE_WIFI 1
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
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, 20000000)) {
    err = "SD init failed";
    Serial.println("[sd] init failed (check wiring / card)");
    return;
  }
  std::vector<std::string> names;
  File root = SD.open("/");
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    if (!e.isDirectory()) {
      std::string fname = e.name();
      if (isTrackName(fname)) {
        names.push_back(fname);
      }
    }
  }
  tracks = buildTrackList(names);
  if (tracks.empty()) err = "no tracks";
  Serial.printf("[sd] %u tracks\n", (unsigned)tracks.size());
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
  Serial.println("[cmd] p=play s=pause n=next r=prev +=vol up -=vol down");
}

void loop() {
  static uint32_t lastDiag = 0;
  static int vol = 50;
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'p': ctl.enqueue({Cmd::Play, 0}); break;
      case 's': ctl.enqueue({Cmd::Pause, 0}); break;
      case 'n': ctl.enqueue({Cmd::Next, 0}); break;
      case 'r': ctl.enqueue({Cmd::Prev, 0}); break;
      case '+': vol = vol > 90 ? 100 : vol + 10; ctl.enqueue({Cmd::Volume, vol}); break;
      case '-': vol = vol < 10 ? 0 : vol - 10; ctl.enqueue({Cmd::Volume, vol}); break;
    }
  }
  if (millis() - lastDiag >= 10000) {
    lastDiag = millis();
    StateSnapshot s = ctl.snapshot();
    Serial.printf("[diag] t=%us play=%s bt=%s track=%d/%d vol=%d underruns=%u minHeap=%u psram=%u err=\"%s\"\n",
                  (unsigned)(millis() / 1000), toStr(s.play), toStr(s.bt), s.track, s.trackCount, s.volume,
                  (unsigned)s.underruns, (unsigned)s.minFreeHeap, ESP.getFreePsram(), s.error.c_str());
  }
  delay(20);
}

