#include "BtAudio.h"
#include <Preferences.h>
#include <string.h>
#include "TrackList.h"
#include "config.h"

BtAudio* BtAudio::inst_ = nullptr;

// Bounded and non-blocking: ring->fillPcm never waits. Always returns `count` frames
// (silence-padded) so the A2DP stream keeps running.
int32_t BtAudio::onFrames(Frame* frames, int32_t count) {
  BtAudio* s = inst_;
  size_t bytes = (size_t)count * sizeof(Frame);
  size_t got = s->ring_->fillPcm(reinterpret_cast<uint8_t*>(frames), bytes);
  if (got < bytes && s->expect_ && !s->ring_->draining()) s->underruns_++;
  return count;
}

void BtAudio::onConnection(esp_a2d_connection_state_t st, void*) {
  BtAudio* s = inst_;
  s->libState_ = st;
  Serial.printf("[bt] connection state: %d\n", (int)st);
  if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    s->attempting_ = false;
    s->savePeerPending_ = true;  // offload flash write to supervisor task
  }
}

bool BtAudio::onSsid(const char* ssid, esp_bd_addr_t, int rssi) {
  bool ok = namesMatch(AIRPODS_NAME, ssid ? ssid : "");
  Serial.printf("[bt] discovered \"%s\" rssi=%d match=%d\n", ssid ? ssid : "?", rssi, (int)ok);
  return ok;
}

void BtAudio::savePeer() {
  esp_bd_addr_t* a = a2dp_.get_last_peer_address();
  if (!a) return;
  Preferences p;
  if (p.begin("mp3", false)) {
    p.putBytes("peer", *a, ESP_BD_ADDR_LEN);
    p.end();
    havePeer_ = true;
  }
}

BtState BtAudio::state() const {
  if (libState_ == ESP_A2D_CONNECTION_STATE_CONNECTED) return BtState::Connected;
  if (libState_ == ESP_A2D_CONNECTION_STATE_CONNECTING || attempting_) return BtState::Connecting;
  return BtState::Disconnected;
}

void BtAudio::setVolume(uint8_t pct) { a2dp_.set_volume((uint8_t)((pct > 100 ? 100 : pct) * 127 / 100)); }

bool BtAudio::begin(RingBuffer& ring) {
  ring_ = &ring;
  inst_ = this;
  if (strcmp(AIRPODS_NAME, AIRPODS_NAME_PLACEHOLDER) == 0) {
    Serial.println("[bt] AIRPODS_NAME is the placeholder: pairing disabled. Set it in include/config.h");
    return false;
  }
  esp_bd_addr_t addr;
  Preferences p;
  if (p.begin("mp3", true)) {
    havePeer_ = p.getBytes("peer", addr, ESP_BD_ADDR_LEN) == ESP_BD_ADDR_LEN;
    p.end();
  }
  a2dp_.set_on_connection_state_changed(&BtAudio::onConnection);
  a2dp_.set_ssid_callback(&BtAudio::onSsid);
  if (havePeer_) a2dp_.set_auto_reconnect(addr);
  attempting_ = true;
  a2dp_.start(&BtAudio::onFrames);
  xTaskCreatePinnedToCore(&BtAudio::superviseEntry, "btsup", 4096, this, 1, nullptr, 1);
  return true;
}

// Reconnect with exponential backoff 1 s -> 30 s. An attempt that is neither connected nor
// completed after 15 s is declared failed. Without a saved peer the library's discovery loop
// (name match via onSsid) does the finding; with one we also call reconnect().
void BtAudio::supervise() {
  uint32_t backoff = 1000, lastAttempt = millis();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    if (savePeerPending_.exchange(false)) {
      savePeer();
    }
    if (libState_ == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      backoff = 1000;
      lastAttempt = millis();
      continue;
    }
    uint32_t since = millis() - lastAttempt;
    if (attempting_ && since > 15000) {
      attempting_ = false;
      lastAttempt = millis();
    }
    if (!attempting_ && (millis() - lastAttempt) > backoff) {
      lastAttempt = millis();
      backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
      attempting_ = true;
      Serial.printf("[bt] retry (next backoff %u ms)\n", (unsigned)backoff);
      if (havePeer_) a2dp_.reconnect();
    }
  }
}

