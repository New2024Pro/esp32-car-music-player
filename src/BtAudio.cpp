#include "BtAudio.h"
#include <Preferences.h>
#include <esp_gap_bt_api.h>
#include <esp_wifi.h>
#include <string.h>
#include "Params.h"
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
  if (st == ESP_A2D_CONNECTION_STATE_CONNECTING) s->lastProgress_ = millis();
  if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    s->attempting_ = false;
    s->pairBusy_ = false;
    s->reconnectTries_ = 0;
    s->savePeerPending_ = true;  // offload flash write to supervisor task
  }
  if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    s->pairBusy_ = false;
    s->clearPairPending(nullptr);
  }
}

void BtAudio::setPairPending(PairKind kind, uint32_t code, const uint8_t* addr) {
  xSemaphoreTake(nameMu_, portMAX_DELAY);
  pairKind_ = kind;
  pairCode_ = code;
  pairSince_ = millis();
  memcpy(pairAddr_, addr, ESP_BD_ADDR_LEN);
  pairMsg_.clear();
  xSemaphoreGive(nameMu_);
}

// msg == nullptr keeps the previous outcome message.
void BtAudio::clearPairPending(const char* msg) {
  xSemaphoreTake(nameMu_, portMAX_DELAY);
  pairKind_ = PairKind::None;
  pairCode_ = 0;
  if (msg) pairMsg_ = msg;
  xSemaphoreGive(nameMu_);
}

// GAP security events (Bluetooth stack task). Replies that need the user are left pending;
// answerPairing() sends them. Anything unanswered is rejected by expirePairing().
void BtAudio::onSecurity(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  BtAudio* s = inst_;
  s->lastProgress_ = millis();
  switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT: {
      uint32_t code = param->cfm_req.num_val;
      s->pairBusy_ = true;
      if (BT_AUTO_CONFIRM) {
        Serial.printf("[bt] PAIRING: auto-confirming code %06u (check the navi shows the same)\n", (unsigned)code);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        s->setPairPending(PairKind::Show, code, param->cfm_req.bda);
      } else {
        Serial.printf("[bt] PAIRING: does the navi show %06u? Confirm: BOOT click, web, or serial 'y' ('x' rejects)\n",
                      (unsigned)code);
        s->setPairPending(PairKind::Confirm, code, param->cfm_req.bda);
      }
      break;
    }
    case ESP_BT_GAP_KEY_REQ_EVT:
      s->pairBusy_ = true;
      Serial.println("[bt] PAIRING: type the passkey shown on the navi (web, or serial k<digits> + Enter)");
      s->setPairPending(PairKind::Passkey, 0, param->key_req.bda);
      break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
      s->pairBusy_ = true;
      Serial.printf("[bt] PAIRING: enter %06u on the navi\n", (unsigned)param->key_notif.passkey);
      s->setPairPending(PairKind::Show, param->key_notif.passkey, param->key_notif.bda);
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
      s->pairBusy_ = true;
      bool need16 = param->pin_req.min_16_digit;
      std::string pin;
      xSemaphoreTake(s->nameMu_, portMAX_DELAY);
      pin = s->pin_;
      xSemaphoreGive(s->nameMu_);
      if (!pin.empty() && (!need16 || pin.size() == 16)) {
        Serial.printf("[bt] PAIRING: legacy PIN requested, replying with the pre-entered PIN (%u digits)\n",
                      (unsigned)pin.size());
        esp_bt_pin_code_t code;
        memcpy(code, pin.data(), pin.size());
        esp_bt_gap_pin_reply(param->pin_req.bda, true, pin.size(), code);
        break;
      }
      Serial.printf("[bt] PAIRING: type the %sPIN shown on the navi (web, or serial k<digits> + Enter)\n",
                    need16 ? "16-digit " : "");
      s->setPairPending(PairKind::Pin, 0, param->pin_req.bda);
      break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
      s->pairBusy_ = false;
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        Serial.printf("[bt] PAIRING: authenticated with \"%s\"\n", (const char*)param->auth_cmpl.device_name);
        s->clearPairPending("paired");
      } else {
        // A stale link key (navi deleted this device) fails here forever unless we drop our copy:
        // the next attempt then pairs from scratch.
        Serial.printf("[bt] PAIRING: failed, status %d -> removing bond so the next try pairs fresh\n",
                      (int)param->auth_cmpl.stat);
        esp_bt_gap_remove_bond_device(param->auth_cmpl.bda);
        char m[48];
        snprintf(m, sizeof m, "pairing failed (status %d)", (int)param->auth_cmpl.stat);
        xSemaphoreTake(s->nameMu_, portMAX_DELAY);
        s->pin_.clear();  // likely wrong; ask again next time
        xSemaphoreGive(s->nameMu_);
        s->clearPairPending(m);
      }
      break;
    }
    default:
      break;
  }
}

BtAudio::PairInfo BtAudio::pairing() const {
  PairInfo p;
  if (!nameMu_) return p;
  xSemaphoreTake(nameMu_, portMAX_DELAY);
  p.kind = pairKind_;
  p.code = pairCode_;
  p.msg = pairMsg_;
  xSemaphoreGive(nameMu_);
  return p;
}

bool BtAudio::answerPairing(bool accept, const std::string& code) {
  if (!nameMu_) return false;
  xSemaphoreTake(nameMu_, portMAX_DELAY);
  PairKind kind = pairKind_;
  esp_bd_addr_t addr;
  memcpy(addr, pairAddr_, ESP_BD_ADDR_LEN);
  xSemaphoreGive(nameMu_);

  switch (kind) {
    case PairKind::Confirm:
      esp_bt_gap_ssp_confirm_reply(addr, accept);
      break;
    case PairKind::Passkey:
      if (accept && !isDigitCode(code, 6)) return false;
      esp_bt_gap_ssp_passkey_reply(addr, accept, accept ? (uint32_t)strtoul(code.c_str(), nullptr, 10) : 0);
      break;
    case PairKind::Pin: {
      if (accept && !isDigitCode(code, 16)) return false;
      esp_bt_pin_code_t pin = {0};
      memcpy(pin, code.data(), accept ? code.size() : 0);
      esp_bt_gap_pin_reply(addr, accept, accept ? code.size() : 0, pin);
      if (accept) {
        xSemaphoreTake(nameMu_, portMAX_DELAY);
        pin_ = code;
        xSemaphoreGive(nameMu_);
      }
      break;
    }
    default:
      return false;  // None, or Show (the code is typed on the navi, nothing to send)
  }
  Serial.printf("[bt] PAIRING: %s\n", accept ? "answered, waiting for the navi" : "rejected");
  lastProgress_ = millis();
  clearPairPending(accept ? nullptr : "pairing rejected");
  return true;
}

bool BtAudio::setPin(const std::string& pin) {
  if (!pin.empty() && !isDigitCode(pin, 16)) return false;
  if (!nameMu_) return false;
  xSemaphoreTake(nameMu_, portMAX_DELAY);
  pin_ = pin;
  xSemaphoreGive(nameMu_);
  // Already waiting on a PIN prompt: answer it now.
  if (!pin.empty() && pairing().kind == PairKind::Pin) answerPairing(true, pin);
  return true;
}

// Unanswered prompts would otherwise hang the attempt until the stack's own timeout.
void BtAudio::expirePairing() {
  PairInfo p = pairing();
  if (p.kind == PairKind::None || p.kind == PairKind::Show) return;
  xSemaphoreTake(nameMu_, portMAX_DELAY);
  bool expired = millis() - pairSince_ > PAIRING_TIMEOUT_MS;
  xSemaphoreGive(nameMu_);
  if (!expired) return;
  Serial.println("[bt] PAIRING: prompt not answered in time -> rejecting");
  answerPairing(false, "");
  clearPairPending("pairing timed out");
}

bool BtAudio::onSsid(const char* ssid, esp_bd_addr_t addr, int rssi) {
  BtAudio* s = inst_;
  if (!ssid || !ssid[0]) return false;
  std::string target;
  if (s->nameMu_) {
    xSemaphoreTake(s->nameMu_, portMAX_DELAY);
    target = s->targetName_;
    bool found = false;
    for (auto& d : s->discovered_) {
      if (namesMatch(d.name, ssid)) {
        d.name = ssid;
        memcpy(d.addr, addr, ESP_BD_ADDR_LEN);
        d.rssi = rssi;
        found = true;
        break;
      }
    }
    if (!found && s->discovered_.size() < 10) {
      DiscoveredBtDevice d;
      d.name = ssid;
      memcpy(d.addr, addr, ESP_BD_ADDR_LEN);
      d.rssi = rssi;
      s->discovered_.push_back(d);
    }
    xSemaphoreGive(s->nameMu_);
  }
  bool ok = namesMatch(target, ssid);
  Serial.printf("[bt] discovered \"%s\" rssi=%d match=%d\n", ssid, rssi, (int)ok);
  if (ok && s->nameMu_) {
    xSemaphoreTake(s->nameMu_, portMAX_DELAY);
    s->connectedName_ = ssid;
    xSemaphoreGive(s->nameMu_);
  }
  return ok;
}

void BtAudio::savePeer() {
  esp_bd_addr_t* a = a2dp_.get_last_peer_address();
  if (!a) return;
  memcpy(savedPeerAddr_, *a, ESP_BD_ADDR_LEN);
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
  nameMu_ = xSemaphoreCreateMutex();

  Preferences p;
  char storedName[64] = {0};
  if (p.begin("mp3", false)) {
    if (p.getString("target_bt", storedName, sizeof(storedName)) > 0) {
      targetName_ = storedName;
    } else {
      targetName_ = AIRPODS_NAME;
    }
    p.end();
  } else {
    targetName_ = AIRPODS_NAME;
  }

  if (targetName_ == AIRPODS_NAME_PLACEHOLDER) {
    Serial.println("[bt] AIRPODS_NAME is the placeholder: pairing disabled.");
    return false;
  }
  if (p.begin("mp3", true)) {
    havePeer_ = p.getBytes("peer", savedPeerAddr_, ESP_BD_ADDR_LEN) == ESP_BD_ADDR_LEN;
    p.end();
  }
  a2dp_.set_on_connection_state_changed(&BtAudio::onConnection);
  a2dp_.set_ssid_callback(&BtAudio::onSsid);
  a2dp_.securityHook = &BtAudio::onSecurity;
  a2dp_.set_local_name(BT_LOCAL_NAME);
  // Keep Bluetooth IDLE at boot so Wi-Fi SoftAP serves HTML immediately without page-train jamming;
  // supervise() reconnects to a saved peer itself.
  a2dp_.set_auto_reconnect(false);
  a2dp_.start(&BtAudio::onFrames);

  // start() leaves bluedroid enabled, so GAP security/CoD settings apply from here on.
  esp_bt_io_cap_t iocap = BT_IO_CAP;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
  esp_bt_pin_code_t noPin = {0};
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, noPin);  // legacy PIN: always ask (PIN_REQ)
#if BT_COD_AS_PHONE
  esp_bt_cod_t cod = {};
  cod.major = ESP_BT_COD_MAJOR_DEV_PHONE;
  cod.minor = 0x03;  // smartphone
  cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY;
  esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
#endif

  xTaskCreatePinnedToCore(&BtAudio::superviseEntry, "btsup", 4096, this, 1, nullptr, 1);
  return true;
}

std::string BtAudio::connectedDeviceName() const {
  std::string n;
  if (nameMu_) {
    xSemaphoreTake(nameMu_, portMAX_DELAY);
    if (libState_ == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      n = connectedName_.empty() ? targetName_ : connectedName_;
    } else {
      n = targetName_;
    }
    xSemaphoreGive(nameMu_);
  }
  return n;
}

void BtAudio::setTargetDevice(const std::string& name) {
  if (name.empty()) return;
  bool sameTarget = false;
  bool haveKnownAddr = false;
  esp_bd_addr_t knownAddr = {0};
  if (nameMu_) {
    xSemaphoreTake(nameMu_, portMAX_DELAY);
    sameTarget = namesMatch(targetName_, name);
    targetName_ = name;
    connectedName_.clear();
    for (const auto& d : discovered_) {
      if (namesMatch(d.name, name)) {
        memcpy(knownAddr, d.addr, ESP_BD_ADDR_LEN);
        haveKnownAddr = true;
        connectedName_ = d.name;
        break;
      }
    }
    xSemaphoreGive(nameMu_);
  }
  Preferences p;
  if (p.begin("mp3", false)) {
    p.putString("target_bt", name.c_str());
    if (!sameTarget) p.remove("peer");
    p.end();
  }
  if (!sameTarget) havePeer_ = false;
  reconnectTries_ = 0;
  a2dp_.clearPeer();
  attempting_ = true;
  if (haveKnownAddr) {
    Serial.printf("[bt] connecting directly to discovered \"%s\"\n", name.c_str());
    a2dp_.connectPeer(knownAddr);
  } else if (sameTarget && havePeer_) {
    Serial.printf("[bt] reconnecting to saved peer \"%s\"\n", name.c_str());
    connectRequested_ = true;
  } else {
    scanRequested_ = true;
    Serial.printf("[bt] scanning for \"%s\" (5s)...\n", name.c_str());
  }
}

void BtAudio::triggerScan() {
  attempting_ = true;
  scanRequested_ = true;
}

std::vector<DiscoveredBtDevice> BtAudio::discoveredDevices() const {
  std::vector<DiscoveredBtDevice> out;
  if (nameMu_) {
    xSemaphoreTake(nameMu_, portMAX_DELAY);
    out = discovered_;
    xSemaphoreGive(nameMu_);
  }
  return out;
}

// Connect/scan on demand, plus a backed-off reconnect to the saved peer (so the car navi is picked
// up whenever it powers on). No continuous page trains: Wi-Fi SoftAP keeps getting airtime.
void BtAudio::supervise() {
  // First reconnect shortly after boot; the navi usually needs longer, the backoff covers that.
  uint32_t nextReconnect = havePeer_ ? millis() + 2000 : 0;
  uint32_t backoff = BT_RECONNECT_MIN_MS;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    expirePairing();
    if (savePeerPending_.exchange(false)) {
      savePeer();
    }
    if (connectRequested_.exchange(false)) {
      lastProgress_ = millis();
      attempting_ = true;
      a2dp_.connectPeer(savedPeerAddr_);
      continue;
    }
    if (scanRequested_.exchange(false)) {
      lastProgress_ = millis();
      attempting_ = true;
      Serial.println("[bt] starting on-demand discovery scan (5s)...");
      a2dp_.startDiscovery(4);
      continue;
    }
    if (libState_ == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      attempting_ = false;
      backoff = BT_RECONNECT_MIN_MS;
      nextReconnect = 0;
      continue;
    }
    uint32_t now = millis();
    // Counted from the last progress (connect start, pairing prompt, answer), not from the scan:
    // a first-time pairing waits on the user reading the code off the navi.
    uint32_t limit = pairBusy_ ? PAIRING_TIMEOUT_MS : BT_CONNECT_TIMEOUT_MS;
    if (attempting_ && !a2dp_.is_discovery_active() && now - lastProgress_ > limit) {
      attempting_ = false;
      pairBusy_ = false;
      if (libState_ == ESP_A2D_CONNECTION_STATE_CONNECTING) {
        Serial.printf("[bt] connect timed out after %us -> releasing radio for Wi-Fi\n", (unsigned)(limit / 1000));
        a2dp_.clearPeer();
      }
    }
    if (havePeer_ && !attempting_ && !a2dp_.is_discovery_active() &&
        libState_ == ESP_A2D_CONNECTION_STATE_DISCONNECTED && reconnectTries_ < BT_RECONNECT_MAX_TRIES &&
        (int32_t)(now - quietUntil_) >= 0) {
      if (nextReconnect == 0) {
        nextReconnect = now + backoff;  // link dropped or last attempt failed
      } else if ((int32_t)(now - nextReconnect) >= 0) {
        uint32_t tries = ++reconnectTries_;
        Serial.printf("[bt] reconnecting to saved peer (try %u/%u, backoff %us)\n", (unsigned)tries,
                      (unsigned)BT_RECONNECT_MAX_TRIES, (unsigned)(backoff / 1000));
        if (tries == BT_RECONNECT_MAX_TRIES)
          Serial.println("[bt] last auto-reconnect try; use the web page or hold BOOT to connect later");
        connectRequested_ = true;
        nextReconnect = 0;
        backoff = backoff * 2 > BT_RECONNECT_MAX_MS ? BT_RECONNECT_MAX_MS : backoff * 2;
      }
    }
  }
}

