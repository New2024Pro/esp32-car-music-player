#pragma once
#include <Arduino.h>
#include <MP3DecoderHelix.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string>
#include <vector>
#include "RingBuffer.h"

// Owns every SD file operation and every decoder reset. Writes decoded 44.1 kHz stereo
// 16-bit PCM into the ring. Driven by the Controller only.
class DecoderWorker {
 public:
  enum Result : uint8_t { IDLE, OPENING, PLAYING, ENDED, UNSUPPORTED, IO_ERROR };

  void begin(RingBuffer& ring, const std::vector<std::string>& tracks, uint8_t core = 1);
  void open(int index);              // async: status() becomes OPENING immediately
  void requestStop();                // async: worker closes file, then acks
  bool waitStopped(uint32_t ms);     // true once the worker acked (file closed, idle)
  Result status() const { return status_.load(); }
  std::string lastError() const;

 private:
  static void taskEntry(void* self) { static_cast<DecoderWorker*>(self)->run(); }
  static void onPcm(MP3FrameInfo& info, short* pcm, size_t len, void*);
  void run();
  void playFile(int index);
  void handlePcm(MP3FrameInfo& info, const short* pcm, size_t len);
  void setError(const char* msg);

  RingBuffer* ring_ = nullptr;
  const std::vector<std::string>* tracks_ = nullptr;
  libhelix::MP3DecoderHelix decoder_;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t stopped_ = nullptr;
  SemaphoreHandle_t errMutex_ = nullptr;
  std::string err_;
  std::atomic<Result> status_{IDLE};
  std::atomic<int> openReq_{-1};
  std::atomic<bool> stop_{false};
  std::atomic<bool> abort_{false};
  bool fmtChecked_ = false;
  static DecoderWorker* inst_;
};
