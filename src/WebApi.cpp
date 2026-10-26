#include "WebApi.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "Params.h"
#include "TrackList.h"
#include "config.h"
#include <LittleFS.h>

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

static std::string stateJson(const StateSnapshot& s, bool scanning) {
  char b[480];
  snprintf(b, sizeof b,
           "{\"state\":\"%s\",\"bt\":\"%s\",\"btScanning\":%s,\"btDevice\":\"%s\",\"pair\":\"%s\",\"pairCode\":\"%06u\","
           "\"track\":%d,\"trackCount\":%d,\"volume\":%d,\"underruns\":%u,\"minFreeHeap\":%u,\"resetReason\":%d,\"pairMsg\":\"",
           toStr(s.play), toStr(s.bt), scanning ? "true" : "false", jsonEscape(s.btDevice).c_str(), toStr(s.pair),
           (unsigned)s.pairCode, s.track, s.trackCount, s.volume, (unsigned)s.underruns, (unsigned)s.minFreeHeap,
           s.resetReason);
  return std::string(b) + jsonEscape(s.pairMsg) + "\",\"error\":\"" + jsonEscape(s.error) + "\"}";
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
    res->addHeader("Cache-Control", "max-age=300");  // reloads skip the busy radio for 5 min
    r->send(res);
  });
  server.on("/api/state", HTTP_GET,
            [&ctl](AsyncWebServerRequest* r) { sendJson(r, 200, stateJson(ctl.snapshot(), ctl.isBtScanning())); });
  server.on("/api/tracks", HTTP_GET, [&ctl](AsyncWebServerRequest* r) {
    long off = 0, lim = 50;
    if (intParam(r, "offset", off, false) || intParam(r, "limit", lim, false))
      return sendErr(r, 400, "offset and limit must be integers");
    Page p = pageTracks(ctl.tracks(), off, lim);
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
  server.on("/api/track", HTTP_ANY, [&ctl](AsyncWebServerRequest* r) {
    long i = 0;
    if (intParam(r, "i", i, true)) return sendErr(r, 400, "i must be an integer");
    if (i < 0 || (size_t)i >= ctl.tracks().size()) return sendErr(r, 400, "track index out of range");
    enqueue(r, ctl, Cmd::Track, (int)i);
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
