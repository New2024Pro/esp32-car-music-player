#include "WebApi.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "Mp3Info.h"
#include "Params.h"
#include "SdFiles.h"
#include "TrackList.h"
#include "config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>

static AsyncWebServer server(80);

static void sendJson(AsyncWebServerRequest* r, int code, const std::string& body) {
  r->send(code, "application/json", body.c_str());
}
static void sendErr(AsyncWebServerRequest* r, int code, const char* msg) {
  sendJson(r, code, std::string("{\"error\":\"") + jsonEscape(msg) + "\"}");
}

// Optional/required integer query parameter. 0 = ok (out set or default kept), -1 = malformed.
static int intParam(AsyncWebServerRequest* r, const char* name, long& out, bool required) {
  if (!r->hasParam(name)) return required ? -1 : 0;
  return parseLong(r->getParam(name)->value().c_str(), out) ? 0 : -1;
}

static void enqueue(AsyncWebServerRequest* r, Controller& c, Cmd cmd, int arg = 0) {
  if (!c.enqueue({cmd, arg})) return sendErr(r, 503, "busy, try again");
  sendJson(r, 200, "{\"ok\":true}");
}

static std::string stateJson(const StateSnapshot& s, bool scanning, uint64_t sdFree) {
  char b[560];
  snprintf(b, sizeof b,
           "{\"state\":\"%s\",\"bt\":\"%s\",\"btScanning\":%s,\"btDevice\":\"%s\",\"pair\":\"%s\",\"pairCode\":\"%06u\","
           "\"track\":%d,\"trackCount\":%d,\"pos\":%d,\"dur\":%d,\"shuffle\":%s,\"repeat\":\"%s\",\"volume\":%d,\"underruns\":%u,\"minFreeHeap\":%u,"
           "\"resetReason\":%d,\"pairMsg\":\"",
           toStr(s.play), toStr(s.bt), scanning ? "true" : "false", jsonEscape(s.btDevice).c_str(), toStr(s.pair),
           (unsigned)s.pairCode, s.track, s.trackCount, s.posSec, s.durSec, s.shuffle ? "true" : "false", toStr(s.repeat), s.volume, (unsigned)s.underruns,
           (unsigned)s.minFreeHeap, s.resetReason);
  return std::string(b) + jsonEscape(s.pairMsg) + "\",\"uploading\":" + (s.uploading ? "true" : "false") +
         ",\"sdFreeMB\":" + std::to_string((unsigned)(sdFree >> 20)) +
         ",\"error\":\"" + jsonEscape(s.error) + "\"}";
}

// One upload at a time: written to a temp file through a PSRAM buffer, checked, then renamed into
// place, so an aborted or rejected upload never leaves half a song in the track list.
struct Upload {
  AsyncWebServerRequest* req = nullptr;
  File f;
  std::string name;
  std::string err;
  uint8_t* buf = nullptr;
  size_t used = 0;
  size_t total = 0;
};
static Upload up;
static const char* kUploadTmp = "/.upload.tmp";
// Internal, DMA-capable and word-aligned: the SD driver then writes straight from it. From a PSRAM
// buffer it needs a fresh internal bounce buffer per sector, and with Wi-Fi receiving those
// allocations fail mid-upload (sdmmc_write_blocks failed (257) = ESP_ERR_NO_MEM).
static const size_t kUploadBuf = 4 * 1024;

static bool uploadFlush() {
  if (!up.used) return true;
  bool ok = up.f.write(up.buf, up.used) == up.used;
  up.used = 0;
  return ok;
}

static void uploadFail(const std::string& why) {
  if (up.err.empty()) up.err = why;
  Serial.printf("[sd] upload rejected: %s: %s\n", up.name.c_str(), up.err.c_str());
  if (up.f) up.f.close();
  sdFs().remove(kUploadTmp);
}

static void uploadReset() {
  if (up.f) up.f.close();
  free(up.buf);
  up = Upload();
}

// Plain file name: no folders, ends in .mp3, nothing FAT cannot store.
static bool validUploadName(const std::string& n) {
  if (n.empty() || n.size() > 120 || n[0] == '.' || !isTrackName(n)) return false;
  for (char c : n)
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      return false;
  return true;
}

// Body chunks of POST /api/upload (multipart, field "file"). Runs on the async_tcp task.
static void onUploadChunk(Controller& ctl, AsyncWebServerRequest* r, const String& filename, size_t index,
                          uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    uploadReset();
    up.req = r;
    up.name = filename.c_str();
    // A full card makes every write fail late and slowly (FATFS scans for a free cluster on each
    // one, long enough to trip the async_tcp watchdog), so refuse up front. The request length
    // includes ~200 bytes of multipart framing; keep 64 KB spare for directory and FAT growth.
    uint64_t need = r->contentLength() + 64 * 1024, free = ctl.sdFree();
    if (!ctl.uploading()) {
      up.err = "not in upload mode: call /api/upload/begin first";
    } else if (need > free) {
      char m[128];
      snprintf(m, sizeof m, "not enough space on the SD card: %u MB free, this song needs %u MB",
               (unsigned)(free >> 20), (unsigned)((need >> 20) + 1));
      up.err = m;
    } else if (!validUploadName(up.name)) {
      up.err = "file name must end in .mp3 and contain no / \\ : * ? \" < > |";
    } else if (!(up.buf = (uint8_t*)heap_caps_malloc(kUploadBuf, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))) {
      up.err = "ESP32 out of memory: try again";
    } else {
      sdFs().remove(kUploadTmp);
      up.f = sdFs().open(kUploadTmp, FILE_WRITE);
      if (!up.f) up.err = "cannot write to the SD card";
    }
    Serial.printf("[sd] upload start: %s%s%s\n", up.name.c_str(), up.err.empty() ? "" : " -> ", up.err.c_str());
  }
  if (up.req != r || !up.err.empty()) return;  // rejected: swallow the rest of the body
  ctl.touchUpload();
  while (len > 0) {
    size_t n = len < kUploadBuf - up.used ? len : kUploadBuf - up.used;
    memcpy(up.buf + up.used, data, n);
    up.used += n;
    up.total += n;
    data += n;
    len -= n;
    if (up.used == kUploadBuf && !uploadFlush()) return uploadFail("SD card full or write error");
  }
  if (!final) return;
  if (!uploadFlush()) return uploadFail("SD card full or write error");
  up.f.close();

  // Only 44.1 kHz plays (Bluetooth SBC runs at 44.1 kHz); refuse others instead of skipping them later.
  File chk = sdFs().open(kUploadTmp, FILE_READ);
  uint8_t head[12];
  size_t hn = chk ? chk.read(head, sizeof head) : 0;
  const char* other = otherAudioFormat(head, hn);
  uint32_t start = id3v2Size(head, hn);
  if (chk) chk.seek(start < chk.size() ? start : 0);
  uint32_t rate = 0;
  for (int k = 0; chk && !other && !rate && k < 4; k++) {  // first 16 KB of audio, 4 KB at a time
    size_t sn = chk.read(up.buf, kUploadBuf);
    if (!sn) break;
    rate = mp3FirstSampleRate(up.buf, sn);
  }
  if (chk) chk.close();
  if (rate != 44100) {
    char m[128];
    if (other) snprintf(m, sizeof m, "this is a %s file renamed to .mp3. Convert it to MP3 (44.1 kHz), then upload again.", other);
    else if (rate) snprintf(m, sizeof m, "%u Hz audio: only 44.1 kHz MP3s play. Convert it, then upload again.", (unsigned)rate);
    else snprintf(m, sizeof m, "not an MP3 file (no MPEG audio found in the first 16 KB)");
    return uploadFail(m);
  }
  std::string path = "/" + up.name;
  sdFs().remove(path.c_str());  // replacing a song with the same name
  if (!sdFs().rename(kUploadTmp, path.c_str())) return uploadFail("could not save the file on the SD card");
  Serial.printf("[sd] uploaded %s (%u KB)\n", up.name.c_str(), (unsigned)(up.total / 1024));
}

// Form-body parameter first, then query string.
static bool strParam(AsyncWebServerRequest* r, const char* name, String& out) {
  if (r->hasParam(name, true)) out = r->getParam(name, true)->value();
  else if (r->hasParam(name)) out = r->getParam(name)->value();
  else return false;
  return true;
}

bool webApiBegin(Controller& ctl) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  const char* pass = (AP_PASS && AP_PASS[0]) ? AP_PASS : nullptr;
  if (!WiFi.softAP(AP_SSID, pass, 6, 0, 2)) {
    Serial.println("[web] softAP failed");
    return false;
  }
  // Coexistence requirement: 20MHz bandwidth + modem sleep when both Wi-Fi and Bluetooth are active
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  // Bluetooth reconnect paging shares the radio and makes the phone's join/DHCP time out.
  WiFi.onEvent(
      [&ctl](arduino_event_id_t, arduino_event_info_t) {
        Serial.println("[web] phone joined hotspot");
        ctl.btQuietFor(BT_QUIET_AFTER_WIFI_JOIN_MS);
      },
      ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) { Serial.println("[web] phone left hotspot"); },
               ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) { Serial.println("[web] phone got IP address"); },
               ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
  Serial.printf("[web] AP \"%s\" up at %s (%s)\n", AP_SSID, WiFi.softAPIP().toString().c_str(),
                pass ? pass : "OPEN - no password");

  // Page from LittleFS (data/index.html.gz, built from web/). Not mounted -> say how to fix it.
  bool fsOk = LittleFS.begin(false);
  if (!fsOk) Serial.println("[web] LittleFS mount failed: run `pio run -t uploadfs` to upload the web UI");
  server.on("/", HTTP_GET, [fsOk](AsyncWebServerRequest* r) {
    Serial.printf("[web] page requested (free internal heap %u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!fsOk || !LittleFS.exists("/index.html.gz"))
      return r->send(503, "text/plain", "Web UI not uploaded. Run: pio run -t uploadfs");
    AsyncWebServerResponse* res = r->beginResponse(LittleFS, "/index.html.gz", "text/html");
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-cache");  // a stale cached page hid UI updates; it is only ~6 KB
    r->send(res);
  });
  server.on("/api/state", HTTP_GET,
            [&ctl](AsyncWebServerRequest* r) { sendJson(r, 200, stateJson(ctl.snapshot(), ctl.isBtScanning(), ctl.sdFree())); });
  server.on("/api/tracks", HTTP_GET, [&ctl](AsyncWebServerRequest* r) {
    long off = 0, lim = 50;
    if (intParam(r, "offset", off, false) || intParam(r, "limit", lim, false))
      return sendErr(r, 400, "offset and limit must be integers");
    Page p = ctl.tracksPage(off, lim);
    std::string j = "{\"total\":" + std::to_string(p.total) + ",\"offset\":" + std::to_string(p.offset) + ",\"items\":[";
    for (size_t k = 0; k < p.items.size(); k++) {
      if (k) j += ',';
      j += "{\"i\":" + std::to_string(p.items[k].first) + ",\"name\":\"" + jsonEscape(p.items[k].second) + "\"}";
    }
    sendJson(r, 200, j + "]}");
  });
  server.on("/api/play", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Play); });
  server.on("/api/pause", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Pause); });
  server.on("/api/next", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Next); });
  server.on("/api/prev", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Prev); });
  server.on("/api/volume", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    long v = 0;
    if (intParam(r, "v", v, true)) return sendErr(r, 400, "v must be an integer 0-100");
    ctl.setVolumeImmediate(clampVolume(v));
    sendJson(r, 200, "{\"ok\":true}");
  });
  server.on("/api/seek", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    long s = 0;
    if (intParam(r, "s", s, true) || s < 0) return sendErr(r, 400, "s must be seconds >= 0");
    enqueue(r, ctl, Cmd::Seek, (int)s);
  });
  server.on("/api/shuffle", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    long on = 0;
    if (intParam(r, "on", on, true) || (on != 0 && on != 1)) return sendErr(r, 400, "on must be 0 or 1");
    enqueue(r, ctl, Cmd::Shuffle, (int)on);
  });
  // mode: 0 = off, 1 = all, 2 = one
  server.on("/api/repeat", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    long m = 0;
    if (intParam(r, "mode", m, true) || m < 0 || m > 2) return sendErr(r, 400, "mode must be 0, 1 or 2");
    enqueue(r, ctl, Cmd::Repeat, (int)m);
  });
  server.on("/api/track", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    long i = 0;
    if (intParam(r, "i", i, true)) return sendErr(r, 400, "i must be an integer");
    if (i < 0 || (size_t)i >= ctl.trackCount()) return sendErr(r, 400, "track index out of range");
    enqueue(r, ctl, Cmd::Track, (int)i);
  });
  server.on("/api/delete", HTTP_POST, [&ctl](AsyncWebServerRequest* r) {
    long i = 0;
    if (intParam(r, "i", i, true)) return sendErr(r, 400, "i must be an integer");
    if (i < 0 || (size_t)i >= ctl.trackCount()) return sendErr(r, 400, "track index out of range");
    enqueue(r, ctl, Cmd::Delete, (int)i);
  });
  // Upload: begin (stops playback; poll /api/state until "uploading":true), one POST per file, end.
  server.on("/api/upload/begin", HTTP_POST, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::UploadBegin); });
  server.on("/api/upload/end", HTTP_POST, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::UploadEnd); });
  server.on(
      "/api/upload", HTTP_POST,
      [](AsyncWebServerRequest* r) {
        bool mine = up.req == r;
        std::string err = !mine ? "no file received" : up.err;
        std::string name = up.name;
        if (mine) uploadReset();
        if (!err.empty()) return sendErr(r, 400, err.c_str());
        sendJson(r, 200, "{\"ok\":true,\"name\":\"" + jsonEscape(name) + "\"}");
      },
      [&ctl](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        onUploadChunk(ctl, r, filename, index, data, len, final);
      });
  server.on("/api/bt_devices", HTTP_GET, [&ctl](AsyncWebServerRequest* r) {
    auto devs = ctl.btDevices();
    std::string j = std::string("{\"scanning\":") + (ctl.isBtScanning() ? "true" : "false") + ",\"items\":[";
    for (size_t k = 0; k < devs.size(); k++) {
      if (k) j += ',';
      j += "{\"name\":\"" + jsonEscape(devs[k].name) + "\",\"rssi\":" + std::to_string(devs[k].rssi) + "}";
    }
    sendJson(r, 200, j + "]}");
  });
  server.on("/api/bt_scan", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    ctl.triggerBtScan();
    sendJson(r, 200, "{\"ok\":true}");
  });
  // Answer a pending pairing prompt: accept=1|0, code=<digits> for PIN / passkey prompts.
  server.on("/api/bt_pair", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    String accept, code;
    if (!strParam(r, "accept", accept)) return sendErr(r, 400, "accept parameter required");
    strParam(r, "code", code);
    if (!ctl.answerBtPairing(accept == "1", code.c_str()))
      return sendErr(r, 409, "no pairing prompt to answer, or code is not 1-16 digits");
    sendJson(r, 200, "{\"ok\":true}");
  });
  // Pre-enter the PIN an older navi shows on its pairing screen (empty clears it).
  server.on("/api/bt_pin", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    String pin;
    if (!strParam(r, "pin", pin)) return sendErr(r, 400, "pin parameter required");
    if (!ctl.setBtPin(pin.c_str())) return sendErr(r, 400, "pin must be 1-16 digits");
    sendJson(r, 200, "{\"ok\":true}");
  });
  server.on("/api/bt_target", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    String n;
    if (!strParam(r, "name", n)) return sendErr(r, 400, "name parameter required");
    if (n.length() == 0 || n.length() > 60) return sendErr(r, 400, "invalid name");
    ctl.setBtTarget(n.c_str());
    sendJson(r, 200, "{\"ok\":true}");
  });
  server.onNotFound([](AsyncWebServerRequest* r) { sendErr(r, 404, "not found"); });
  server.begin();
  return true;
}
