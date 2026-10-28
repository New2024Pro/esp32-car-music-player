#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string>
#include <vector>
#include "BtAudio.h"
#include "Commands.h"
#include "DecoderWorker.h"
#include "RingBuffer.h"
#include "TrackList.h"
#include "config.h"

// Sole owner of playback decisions. Web handlers and the serial console only enqueue().
class Controller {
 public:
  // `tracks` is rescanned in place after uploads / deletes (controller task, decoder stopped).
  void begin(DecoderWorker& w, BtAudio& bt, RingBuffer& ring, std::vector<std::string>& tracks,
             const std::string& initialError);
  bool enqueue(Command c) { return xQueueSend(q_, &c, 0) == pdTRUE; }  // false: queue full
  void setVolumeImmediate(int v);
  void setBtTarget(const std::string& name) { bt_->setTargetDevice(name); }
  void triggerBtScan() { bt_->triggerScan(); }
  bool isBtScanning() const { return bt_->isScanning(); }
  std::vector<DiscoveredBtDevice> btDevices() const { return bt_->discoveredDevices(); }
  bool answerBtPairing(bool accept, const std::string& code) { return bt_->answerPairing(accept, code); }
  bool setBtPin(const std::string& pin) { return bt_->setPin(pin); }
  void btQuietFor(uint32_t ms) { bt_->quietFor(ms); }
  // AVRCP button from the AirPods / navi. Called on the Bluetooth task: enqueues only.
  void onRemoteKey(uint8_t keyCode);
  StateSnapshot snapshot() const;
  // Track list access from other tasks (web); the list changes after uploads and deletes.
  Page tracksPage(long offset, long limit) const;
  size_t trackCount() const;

  // Upload mode (Cmd::UploadBegin): playback stopped, SD card free for the web upload handler.
  bool uploading() const { return uploading_; }
  // Free space, refreshed at start and after every rescan (reading it is too slow for the web task).
  uint64_t sdFree() const { return sdFree_; }
  // Ends by itself after 60 s without data. Keeps Bluetooth reconnect paging off the radio
  // meanwhile: it steals the airtime the Wi-Fi upload needs.
  void touchUpload() {
    uploadActiveAt_ = millis();
    bt_->quietFor(15000);
  }

 private:
  enum OpenResult { Ok, Unplayable, Infra };
  static void taskEntry(void* s) { static_cast<Controller*>(s)->run(); }
  void run();
  void handle(Command c);
  void tick();
  OpenResult openTrack(int idx, uint32_t byteOffset = 0);
  void seek(int sec);
  void startTrack(int pos, int dir);
  int currentPos() const;
  void advance(bool autoAdvance);
  void setShuffle(bool on);
  void setRepeat(Repeat r);
  void saveModes();
  void publishTrack();
  void beginUpload();
  void endUpload();
  void deleteTrack(int idx);
  void rescan(const std::string& keepCurrent);
  void stopPlayback();
  void pause();
  void resume();
  void setError(const std::string& e);

  DecoderWorker* w_ = nullptr;
  BtAudio* bt_ = nullptr;
  RingBuffer* ring_ = nullptr;
  std::vector<std::string>* tracks_ = nullptr;  // changed only under mu_, read freely on this task
  QueueHandle_t q_ = nullptr;
  mutable SemaphoreHandle_t mu_ = nullptr;
  PlayState play_ = PlayState::Stopped;
  int cur_ = -1;  // track index (into tracks_), not order position
  // Play order (track indices) and modes: controller task only. *Shown_ are the mutex-guarded
  // copies snapshot() reads.
  std::vector<int> order_;
  bool shuffle_ = false, shuffleShown_ = false;
  Repeat repeat_ = Repeat::Off, repeatShown_ = Repeat::Off;
  int volume_ = DEFAULT_VOLUME;
  std::atomic<uint32_t> volumeChangedAt_{0};  // 0 = saved; else millis() of the last change
  std::string err_;
  uint32_t endWaitStart_ = 0;  // 0 = not waiting for the ring to play out
  bool autoPlayed_ = false;
  std::atomic<bool> uploading_{false};
  std::atomic<uint32_t> uploadActiveAt_{0};
  std::atomic<uint64_t> sdFree_{0};
  bool resumeAfterUpload_ = false;  // what was playing when the upload started
  std::string resumeName_;
  int resumePosSec_ = 0;
};

