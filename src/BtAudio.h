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
  // Button presses from the sink (AirPods stem, navi steering wheel / screen): AVRCP key code.
  static inline void (*remoteHook)(uint8_t keyCode) = nullptr;

  // The library only runs AVRCP as controller. Bring up the target role too, so the sink's
  // play/pause/next/prev reach us. Must happen before A2DP init, which the library does on
  // stack-up (event 0 = BT_APP_EVT_STACK_UP, private to the library's .cpp).
  void bt_av_hdl_stack_evt(uint16_t event, void* p_param) override {
    if (event == 0) initRemoteControl();
    BluetoothA2DPSource::bt_av_hdl_stack_evt(event, p_param);
    // The library leaves us non-connectable. Toyota navis connect *to* the device after
    // registration ("車載機からの接続に切り替えます") and on every ignition, so accept that.
    // Not discoverable: the navi already knows our address.
    if (event == 0) esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  }

  // Link-level trace for diagnosing head units: which side connected, and why links dropped.
  void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) override {
    if (event == ESP_A2D_CONNECTION_STATE_EVT)
      Serial.printf("[bt] a2dp link: state=%d disc_reason=%d\n", (int)param->conn_stat.state,
                    (int)param->conn_stat.disc_rsn);
    else if (event == ESP_A2D_AUDIO_STATE_EVT)
      Serial.printf("[bt] a2dp audio: state=%d (2 = streaming)\n", (int)param->audio_stat.state);
    else if (event == ESP_A2D_AUDIO_CFG_EVT)
      Serial.println("[bt] a2dp audio config received (SBC)");
    BluetoothA2DPSource::bt_app_a2d_cb(event, param);
  }

  void bt_app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) override {
    if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT)
      Serial.printf("[bt] acl link up: status=%d handle=%u\n", (int)param->acl_conn_cmpl_stat.stat,
                    (unsigned)param->acl_conn_cmpl_stat.handle);
    else if (event == ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT)
      Serial.printf("[bt] acl link down: reason=0x%02x\n", (unsigned)param->acl_disconn_cmpl_stat.reason);
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

  void bt_app_av_state_unconnected_hdlr(uint16_t event, void* param) override {
    // Never auto-start 20s baseband page trains from the 10s timer; supervise() controls connection attempts.
    // A connection the navi opened to us arrives here: adopt it like an outgoing one would be.
    if (event != ESP_A2D_CONNECTION_STATE_EVT) return;
    esp_a2d_cb_param_t* a2d = static_cast<esp_a2d_cb_param_t*>(param);
    if (a2d->conn_stat.state != ESP_A2D_CONNECTION_STATE_CONNECTED) return;
    memcpy(peer_bd_addr, a2d->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
    set_last_connection(peer_bd_addr);
    s_a2d_state = APP_AV_STATE_CONNECTED;
    s_media_state = 0;  // APP_AV_MEDIA_STATE_IDLE (enum private to the library .cpp)
  }

  // Suspend / restart the A2DP media stream (AVDTP SUSPEND / START), as phones do on pause/play.
  // The library starts the stream once after connecting and then sits in its STARTED state, which
  // ignores these acks, so we can drive it from here. Toyota navis drop the whole link when they
  // send STOP and the source keeps streaming.
  void setStreaming(bool on) {
    if (!is_connected()) return;
    esp_a2d_media_ctrl(on ? ESP_A2D_MEDIA_CTRL_START : ESP_A2D_MEDIA_CTRL_SUSPEND);
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
    if (is_connected()) {
      disconnect();
    } else {
      // Abort a connect still running in the stack: otherwise it keeps paging for ~30 s and
      // the stack ignores every new connect request until it gives up.
      static const esp_bd_addr_t none = {0};
      if (memcmp(peer_bd_addr, none, ESP_BD_ADDR_LEN) != 0) esp_a2d_source_disconnect(peer_bd_addr);
    }
    memset(last_connection, 0, ESP_BD_ADDR_LEN);
    memset(peer_bd_addr, 0, ESP_BD_ADDR_LEN);
    s_a2d_state = APP_AV_STATE_UNCONNECTED;
  }

 private:
  static void onAvrcTarget(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) {
    if (event == ESP_AVRC_TG_CONNECTION_STATE_EVT)
      Serial.printf("[bt] avrcp remote-control link: %s\n", param->conn_stat.connected ? "up" : "down");
    else if (event == ESP_AVRC_TG_REMOTE_FEATURES_EVT)
      Serial.printf("[bt] avrcp remote features: 0x%x\n", (unsigned)param->rmt_feats.feat_mask);
    // Act on the press; the release that follows carries no new information.
    if (event == ESP_AVRC_TG_PASSTHROUGH_CMD_EVT && param->psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED &&
        remoteHook)
      remoteHook(param->psth_cmd.key_code);
  }

  void initRemoteControl() {
    if (esp_avrc_tg_init() != ESP_OK) {
      Serial.println("[bt] AVRCP target init failed: remote buttons disabled");
      return;
    }
    esp_avrc_tg_register_callback(&CoexA2DPSource::onAvrcTarget);
    esp_avrc_psth_bit_mask_t cmds = {0};
    for (esp_avrc_pt_cmd_t c : {ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STOP,
                                ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_FAST_FORWARD,
                                ESP_AVRC_PT_CMD_REWIND})
      esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmds, c);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmds);
  }

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
  // Pause / resume the Bluetooth audio stream itself (frees the radio while paused).
  void setStreaming(bool on) { a2dp_.setStreaming(on); }

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
  // Receives AVRCP key codes pressed on the sink (Bluetooth stack task: enqueue only).
  void setRemoteHandler(void (*h)(uint8_t keyCode)) { CoexA2DPSource::remoteHook = h; }
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
  // Scan matched the target while the stack was still busy with an older connect (which can run
  // ~60 s and cannot be cancelled): connect to pendingAddr_ once that attempt ends.
  std::atomic<bool> pendingConnect_{false};
  esp_bd_addr_t pendingAddr_ = {0};  // guarded by nameMu_
  // Time of the last connect/pairing progress; the attempt timeout counts from here.
  std::atomic<uint32_t> lastProgress_{0};
  // From the first pairing event until authentication completes: allows PAIRING_TIMEOUT_MS.
  std::atomic<bool> pairBusy_{false};
  std::atomic<uint32_t> quietUntil_{0};
  std::atomic<bool> retrySoon_{false};  // authentication failed: reconnect in ~2 s, not after backoff
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

