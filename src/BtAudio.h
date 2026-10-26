#pragma once
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <atomic>
#include <string>
#include <vector>
#include "Commands.h"
#include "RingBuffer.h"

struct DiscoveredBtDevice {
  std::string name;
  esp_bd_addr_t addr;
  int rssi;
};

// Subclass BluetoothA2DPSource to duty-cycle inquiry discovery so 2.4 GHz Wi-Fi SoftAP is never starved.
class CoexA2DPSource : public BluetoothA2DPSource {
 public:
  std::atomic<bool> allowDiscovery{false};
  // Pairing events go here instead of the library's fixed "1234" PIN / silent auto-accept.
  void (*securityHook)(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) = nullptr;

  void bt_app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) override {
    switch (event) {
      case ESP_BT_GAP_PIN_REQ_EVT:
      case ESP_BT_GAP_CFM_REQ_EVT:
      case ESP_BT_GAP_KEY_REQ_EVT:
      case ESP_BT_GAP_KEY_NOTIF_EVT:
      case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (securityHook) {
          securityHook(event, param);
          return;
        }
        break;
      case ESP_BT_GAP_DISC_RES_EVT:
        if (handleResultWithoutEir(param)) return;
        break;
      default:
        break;
    }
    if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED && !allowDiscovery.load()) {
        // Cancel automatic boot-time inquiry so Wi-Fi SoftAP gets 100% quiet radio
        discovery_active = false;
        s_a2d_state = APP_AV_STATE_UNCONNECTED;
        esp_bt_gap_cancel_discovery();
        return;
      }
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
          s_a2d_state != APP_AV_STATE_DISCOVERED) {
        discovery_active = false;
        if (discovery_mode_callback) discovery_mode_callback(ESP_BT_GAP_DISCOVERY_STOPPED);
        s_a2d_state = APP_AV_STATE_UNCONNECTED;
        return;  // Yield 2.4 GHz radio to Wi-Fi SoftAP between scans
      }
    }
    BluetoothA2DPSource::bt_app_gap_callback(event, param);
  }

  void bt_app_av_state_unconnected_hdlr(uint16_t, void*) override {
    // Never auto-start 20s baseband page trains from the 10s timer; supervise() controls connection attempts
  }

  void startDiscovery(uint8_t inqLen = 4) {
    if (is_connected() || discovery_active) return;
    allowDiscovery = true;
    s_a2d_state = APP_AV_STATE_DISCOVERING;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, inqLen, 0);
  }

  void connectPeer(esp_bd_addr_t addr) {
    if (discovery_active) esp_bt_gap_cancel_discovery();
    memcpy(peer_bd_addr, addr, ESP_BD_ADDR_LEN);
    set_last_connection(peer_bd_addr);
    s_a2d_state = APP_AV_STATE_CONNECTING;
    esp_a2d_connect(peer_bd_addr);
  }

  void clearPeer() {
    if (discovery_active) esp_bt_gap_cancel_discovery();
    if (is_connected()) disconnect();
    memset(last_connection, 0, ESP_BD_ADDR_LEN);
    memset(peer_bd_addr, 0, ESP_BD_ADDR_LEN);
    s_a2d_state = APP_AV_STATE_UNCONNECTED;
  }

 private:
  // The library only reads names from the Extended Inquiry Response. Some head units (older
  // navis) send none and put the name in the BDNAME property instead; match those here.
  bool handleResultWithoutEir(esp_bt_gap_cb_param_t* param) {
    uint32_t cod = 0;
    int rssi = -129;
    const char* name = nullptr;
    size_t nameLen = 0;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
      esp_bt_gap_dev_prop_t* p = param->disc_res.prop + i;
      switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_EIR: return false;  // library path handles it
        case ESP_BT_GAP_DEV_PROP_COD: cod = *(uint32_t*)p->val; break;
        case ESP_BT_GAP_DEV_PROP_RSSI: rssi = *(int8_t*)p->val; break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
          name = (const char*)p->val;
          nameLen = p->len;
          break;
        default: break;
      }
    }
    if (!name || !nameLen || !ssid_callback || !esp_bt_gap_is_valid_cod(cod) || !is_valid_cod_service(cod))
      return false;
    if (nameLen > ESP_BT_GAP_MAX_BDNAME_LEN) nameLen = ESP_BT_GAP_MAX_BDNAME_LEN;
    memcpy(s_peer_bdname, name, nameLen);
    s_peer_bdname[nameLen] = 0;
    if (ssid_callback((const char*)s_peer_bdname, param->disc_res.bda, rssi)) {
      s_a2d_state = APP_AV_STATE_DISCOVERED;
      memcpy(peer_bd_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
      set_last_connection(peer_bd_addr);
      esp_bt_gap_cancel_discovery();
    }
    return true;
  }
};

// A2DP source (SBC). The data callback only drains the ring: no SD, decode, HTTP, or waiting.
class BtAudio {
 public:
  // Returns false (and does not start Bluetooth) while AIRPODS_NAME is still the placeholder.
  bool begin(RingBuffer& ring);
  void setVolume(uint8_t pct0to100);
  BtState state() const;
  std::string connectedDeviceName() const;
  void setTargetDevice(const std::string& name);
  void triggerScan();
  bool isScanning() const { return a2dp_.is_discovery_active(); }
  std::vector<DiscoveredBtDevice> discoveredDevices() const;
  uint32_t underruns() const { return underruns_.load(); }
  // Underruns count only while the controller expects audio (playing, prefilled, not draining).
  void expectAudio(bool e) { expect_ = e; }

  struct PairInfo {
    PairKind kind = PairKind::None;
    uint32_t code = 0;
    std::string msg;
  };
  PairInfo pairing() const;
  // Answers the pending pairing step. `code` is the digits for Pin / Passkey. False when nothing
  // is pending, the code is malformed, or the step cannot be answered from this side (Show).
  bool answerPairing(bool accept, const std::string& code);
  // PIN for legacy pairing, entered before connecting (older navis show it up front). RAM only:
  // once paired, the link key is stored by the Bluetooth stack.
  bool setPin(const std::string& pin);
  // Suspends background reconnect paging for `ms` (a phone is joining the Wi-Fi hotspot).
  void quietFor(uint32_t ms) { quietUntil_ = millis() + ms; }

 private:
  static int32_t onFrames(Frame* frames, int32_t count);
  static void onConnection(esp_a2d_connection_state_t st, void* self);
  static bool onSsid(const char* ssid, esp_bd_addr_t addr, int rssi);
  static void onSecurity(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
  static void superviseEntry(void* self) { static_cast<BtAudio*>(self)->supervise(); }
  void supervise();
  void savePeer();
  void setPairPending(PairKind kind, uint32_t code, const uint8_t* addr);
  void clearPairPending(const char* msg);
  void expirePairing();

  mutable CoexA2DPSource a2dp_;
  RingBuffer* ring_ = nullptr;
  std::atomic<uint32_t> underruns_{0};
  std::atomic<bool> expect_{false};
  std::atomic<int> libState_{ESP_A2D_CONNECTION_STATE_DISCONNECTED};
  std::atomic<bool> attempting_{false};
  std::atomic<bool> havePeer_{false};
  std::atomic<bool> savePeerPending_{false};
  std::atomic<bool> scanRequested_{false};
  std::atomic<bool> connectRequested_{false};
  esp_bd_addr_t savedPeerAddr_ = {0};
  std::string targetName_;
  std::string connectedName_;
  std::vector<DiscoveredBtDevice> discovered_;
  // Time of the last connect/pairing progress; the attempt timeout counts from here.
  std::atomic<uint32_t> lastProgress_{0};
  // From the first pairing event until authentication completes: allows PAIRING_TIMEOUT_MS.
  std::atomic<bool> pairBusy_{false};
  std::atomic<uint32_t> quietUntil_{0};
  // Background reconnect tries since the last connection or user request.
  std::atomic<uint32_t> reconnectTries_{0};
  // Pairing fields below are guarded by nameMu_.
  PairKind pairKind_ = PairKind::None;
  uint32_t pairCode_ = 0;
  uint32_t pairSince_ = 0;
  esp_bd_addr_t pairAddr_ = {0};
  std::string pairMsg_;
  std::string pin_;
  mutable SemaphoreHandle_t nameMu_ = nullptr;
  static BtAudio* inst_;
};

