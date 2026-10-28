#pragma once
#include <Arduino.h>
#include <MP3DecoderHelix.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string>
#include <vector>
#include <FS.h>
#include "Id3.h"
#include "RingBuffer.h"

// Owns every SD file operation and every decoder reset. Writes decoded 44.1 kHz stereo
// 16-bit PCM into the ring. Driven by the Controller only.
class DecoderWorker {
 public:
  enum Result : uint8_t { IDLE, OPENING, PLAYING, ENDED, UNSUPPORTED, IO_ERROR };

  void begin(RingBuffer& ring, const std::vector<std::string>& tracks, uint8_t core = 1);
  // async: status() becomes OPENING immediately. byteOffset > 0 starts mid-file (seek); the
  // decoder resyncs on the next frame header.
  void open(int index, uint32_t byteOffset = 0);
  void requestStop();                // async: worker closes file, then acks
  bool waitStopped(uint32_t ms);     // true once the worker acked (file closed, idle)
  Result status() const { return status_.load(); }
  std::string lastError() const;
  // ID3 title / artist / album of the current track (empty fields when the file has none).
  Id3Tags tags() const;

  // Current file, for position / duration / seek. bitrate 0 until the first frame decodes.
  struct Progress {
    uint32_t pos = 0;      // bytes read so far (decoder input, ahead of what is heard)
    uint32_t size = 0;     // file size
    uint32_t start = 0;    // first audio byte (after ID3v2)
    uint32_t bitrate = 0;  // bits/s of the first frame
  };
  Progress progress() const { return {bytePos_.load(), fileSize_.load(), dataStart_.load(), bitrate_.load()}; }

 private:
  static void taskEntry(void* self) { static_cast<DecoderWorker*>(self)->run(); }
  static void onPcm(MP3FrameInfo& info, short* pcm, size_t len, void*);
  void run();
  void playFile(int index, uint32_t byteOffset);
  void readTags(fs::File& f, const uint8_t* head, size_t headLen);
  void handlePcm(MP3FrameInfo& info, const short* pcm, size_t len);
  void setError(const char* msg);

  RingBuffer* ring_ = nullptr;
  const std::vector<std::string>* tracks_ = nullptr;
  libhelix::MP3DecoderHelix decoder_;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t stopped_ = nullptr;
  SemaphoreHandle_t errMutex_ = nullptr;
  std::string err_;
  Id3Tags tags_;  // guarded by errMutex_
  std::atomic<Result> status_{IDLE};
  std::atomic<int> openReq_{-1};
  std::atomic<uint32_t> openOffset_{0};
  std::atomic<uint32_t> bytePos_{0}, fileSize_{0}, dataStart_{0}, bitrate_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> abort_{false};
  bool fmtChecked_ = false;
  bool seeking_ = false;     // file opened mid-way: tolerate false frame syncs before the first real frame
  int falseFrames_ = 0;
  static DecoderWorker* inst_;
};
