#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string>
#include <vector>
#include "BtAudio.h"
#include "Commands.h"
#include "DecoderWorker.h"
#include "RingBuffer.h"

// Sole owner of playback decisions. Web handlers and the serial console only enqueue().
class Controller {
 public:
  void begin(DecoderWorker& w, BtAudio& bt, RingBuffer& ring, const std::vector<std::string>& tracks,
             const std::string& initialError);
  bool enqueue(Command c) { return xQueueSend(q_, &c, 0) == pdTRUE; }  // false: queue full
  StateSnapshot snapshot() const;
  const std::vector<std::string>& tracks() const { return *tracks_; }

 private:
  enum OpenResult { Ok, Unplayable, Infra };
  static void taskEntry(void* s) { static_cast<Controller*>(s)->run(); }
  void run();
  void handle(Command c);
  void tick();
  OpenResult openTrack(int idx);
  void startTrack(int idx, int dir);
  void stopPlayback();
  void pause();
  void resume();
  void setError(const std::string& e);

  DecoderWorker* w_ = nullptr;
  BtAudio* bt_ = nullptr;
  RingBuffer* ring_ = nullptr;
  const std::vector<std::string>* tracks_ = nullptr;
  QueueHandle_t q_ = nullptr;
  mutable SemaphoreHandle_t mu_ = nullptr;
  PlayState play_ = PlayState::Stopped;
  int cur_ = -1;
  int volume_ = 50;
  std::string err_;
  uint32_t endWaitStart_ = 0;  // 0 = not waiting for the ring to play out
};

