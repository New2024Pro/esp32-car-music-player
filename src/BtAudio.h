#pragma once
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <atomic>
#include "Commands.h"
#include "RingBuffer.h"

// A2DP source (SBC). The data callback only drains the ring: no SD, decode, HTTP, or waiting.
class BtAudio {
 public:
  // Returns false (and does not start Bluetooth) while AIRPODS_NAME is still the placeholder.
  bool begin(RingBuffer& ring);
  void setVolume(uint8_t pct0to100);
  BtState state() const;
  uint32_t underruns() const { return underruns_.load(); }
  // Underruns count only while the controller expects audio (playing, prefilled, not draining).
  void expectAudio(bool e) { expect_ = e; }

 private:
  static int32_t onFrames(Frame* frames, int32_t count);
  static void onConnection(esp_a2d_connection_state_t st, void* self);
  static bool onSsid(const char* ssid, esp_bd_addr_t addr, int rssi);
  static void superviseEntry(void* self) { static_cast<BtAudio*>(self)->supervise(); }
  void supervise();
  void savePeer();

  BluetoothA2DPSource a2dp_;
  RingBuffer* ring_ = nullptr;
  std::atomic<uint32_t> underruns_{0};
  std::atomic<bool> expect_{false};
  std::atomic<int> libState_{ESP_A2D_CONNECTION_STATE_DISCONNECTED};
  std::atomic<bool> attempting_{false};
  bool havePeer_ = false;
  static BtAudio* inst_;
};
