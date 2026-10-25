#include "WebApi.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include "Params.h"
#include "TrackList.h"
#include "config.h"
#include "index_html.h"

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

static std::string stateJson(const StateSnapshot& s) {
  char b[256];
  snprintf(b, sizeof b,
           "{\"state\":\"%s\",\"bt\":\"%s\",\"track\":%d,\"trackCount\":%d,\"volume\":%d,"
           "\"underruns\":%u,\"minFreeHeap\":%u,\"resetReason\":%d,\"error\":\"",
           toStr(s.play), toStr(s.bt), s.track, s.trackCount, s.volume, (unsigned)s.underruns,
           (unsigned)s.minFreeHeap, s.resetReason);
  return std::string(b) + jsonEscape(s.error) + "\"}";
}

bool webApiBegin(Controller& ctl) {
  if (strlen(AP_PASS) < 8) {
    Serial.println("[web] AP_PASS must be >= 8 characters for WPA2");
    return false;
  }
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 2)) {
    Serial.println("[web] softAP failed");
    return false;
  }
  Serial.printf("[web] AP \"%s\" up at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html", (const uint8_t*)INDEX_HTML, sizeof(INDEX_HTML) - 1); });
  server.on("/api/state", HTTP_GET,
            [&ctl](AsyncWebServerRequest* r) { sendJson(r, 200, stateJson(ctl.snapshot())); });
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
  server.on("/api/play", HTTP_POST, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Play); });
  server.on("/api/pause", HTTP_POST, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Pause); });
  server.on("/api/next", HTTP_POST, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Next); });
  server.on("/api/prev", HTTP_POST, [&ctl](AsyncWebServerRequest* r) { enqueue(r, ctl, Cmd::Prev); });
  server.on("/api/volume", HTTP_POST, [&ctl](AsyncWebServerRequest* r) {
    long v = 0;
    if (intParam(r, "v", v, true)) return sendErr(r, 400, "v must be an integer 0-100");
    enqueue(r, ctl, Cmd::Volume, clampVolume(v));
  });
  server.on("/api/track", HTTP_POST, [&ctl](AsyncWebServerRequest* r) {
    long i = 0;
    if (intParam(r, "i", i, true)) return sendErr(r, 400, "i must be an integer");
    if (i < 0 || (size_t)i >= ctl.tracks().size()) return sendErr(r, 400, "track index out of range");
    enqueue(r, ctl, Cmd::Track, (int)i);
  });
  server.onNotFound([](AsyncWebServerRequest* r) { sendErr(r, 404, "not found"); });
  server.begin();
  return true;
}
