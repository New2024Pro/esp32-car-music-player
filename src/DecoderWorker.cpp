#include "DecoderWorker.h"
#include <SD.h>
#include <SD_MMC.h>
#include "Mp3Info.h"
#include <esp_heap_caps.h>

DecoderWorker* DecoderWorker::inst_ = nullptr;

void DecoderWorker::begin(RingBuffer& ring, const std::vector<std::string>& tracks, uint8_t core) {
  ring_ = &ring;
  tracks_ = &tracks;
  inst_ = this;
  stopped_ = xSemaphoreCreateBinary();
  errMutex_ = xSemaphoreCreateMutex();
  decoder_.setDataCallback(&DecoderWorker::onPcm);
  xTaskCreatePinnedToCore(&DecoderWorker::taskEntry, "decoder", 10240, this, 2, &task_, core);
}

void DecoderWorker::setError(const char* msg) {
  xSemaphoreTake(errMutex_, portMAX_DELAY);
  err_ = msg;
  xSemaphoreGive(errMutex_);
}

std::string DecoderWorker::lastError() const {
  xSemaphoreTake(errMutex_, portMAX_DELAY);
  std::string e = err_;
  xSemaphoreGive(errMutex_);
  return e;
}

void DecoderWorker::open(int index, uint32_t byteOffset) {
  status_ = OPENING;
  openOffset_ = byteOffset;
  openReq_ = index;
  xTaskNotifyGive(task_);
}

void DecoderWorker::requestStop() {
  xSemaphoreTake(stopped_, 0);  // drop any stale ack
  stop_ = true;
  xTaskNotifyGive(task_);
}

bool DecoderWorker::waitStopped(uint32_t ms) { return xSemaphoreTake(stopped_, pdMS_TO_TICKS(ms)) == pdTRUE; }

void DecoderWorker::onPcm(MP3FrameInfo& info, short* pcm, size_t len, void*) {
  if (inst_) inst_->handlePcm(info, pcm, len);
}

void DecoderWorker::handlePcm(MP3FrameInfo& info, const short* pcm, size_t len) {
  if (!fmtChecked_) {
    bool bad = info.samprate != 44100 || info.nChans != 2 || info.bitsPerSample != 16;
    // After a seek the decoder can lock onto audio bytes that merely look like a frame header
    // and report a bogus format (e.g. 16 kHz). Drop such frames until a real one turns up.
    if (bad && seeking_ && ++falseFrames_ <= 64) return;
    if (bad) {
      char m[64];
      snprintf(m, sizeof m, "unsupported format: %d Hz, %d ch", info.samprate, info.nChans);
      setError(m);
      status_ = UNSUPPORTED;
      abort_ = true;
      return;
    }
    fmtChecked_ = true;
    // Measured once per track: on VBR files each frame differs, and re-measuring after every
    // seek made positions jump around (55 s -> 55 s -> 81 s -> 151 s -> 106 s for +10 s steps).
    if (info.bitrate > 0 && !seeking_) bitrate_ = (uint32_t)info.bitrate;
  }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(pcm);
  size_t bytes = len * sizeof(short);
  while (bytes > 0 && !stop_ && !abort_) {
    size_t n = ring_->write(p, bytes);
    p += n;
    bytes -= n;
    if (n == 0) vTaskDelay(pdMS_TO_TICKS(5));  // ring full: wait for the A2DP callback to drain
  }
  if (status_ == OPENING) status_ = PLAYING;
}

// Title / artist / album from the ID3v2 tag. Text frames sit at the start of the tag (cover art
// usually follows), so the first 4 KiB is enough. Buffer in PSRAM: the decoder stack is tight.
void DecoderWorker::readTags(File& f, const uint8_t* head, size_t headLen) {
  Id3Tags t;
  uint32_t start = dataStart_.load();
  size_t want = start < 4096 ? start : 4096;
  if (want >= 10 && headLen == 10) {
    uint8_t* tag = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (tag) {
      memcpy(tag, head, 10);
      size_t got = 10 + f.read(tag + 10, want - 10);
      parseId3v2(tag, got, t);
      free(tag);
    }
  }
  xSemaphoreTake(errMutex_, portMAX_DELAY);
  tags_ = t;
  xSemaphoreGive(errMutex_);
}

Id3Tags DecoderWorker::tags() const {
  xSemaphoreTake(errMutex_, portMAX_DELAY);
  Id3Tags t = tags_;
  xSemaphoreGive(errMutex_);
  return t;
}

void DecoderWorker::playFile(int index, uint32_t byteOffset) {
  abort_ = false;
  fmtChecked_ = false;
  seeking_ = byteOffset > 0;
  falseFrames_ = 0;
  if (byteOffset == 0) bitrate_ = 0;  // new track; a seek keeps the known bitrate
  bytePos_ = 0;
  fileSize_ = 0;
  if (index < 0 || (size_t)index >= tracks_->size()) {
    setError("bad track index");
    status_ = IO_ERROR;
    return;
  }
  std::string path = "/" + (*tracks_)[index];
  File f;
  if (SD_MMC.cardType() != CARD_NONE) {
    f = SD_MMC.open(path.c_str(), FILE_READ);
  } else {
    f = SD.open(path.c_str(), FILE_READ);
  }
  if (!f) {
    setError("cannot open file");
    status_ = IO_ERROR;
    return;
  }
  uint8_t buf[512];
  int head = f.read(buf, 10);
  dataStart_ = id3v2Size(buf, head > 0 ? (size_t)head : 0);
  fileSize_ = (uint32_t)f.size();
  if (byteOffset == 0) readTags(f, buf, head > 0 ? (size_t)head : 0);  // a seek keeps the track's tags
  f.seek(byteOffset < fileSize_ ? byteOffset : 0);
  bytePos_ = (uint32_t)f.position();
  decoder_.begin();
  while (!stop_ && !abort_) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    bytePos_ += (uint32_t)n;
    decoder_.write(buf, n);
  }
  bool eof = !stop_ && !abort_;
  decoder_.end();
  f.close();
  if (eof) {
    if (status_ == PLAYING) status_ = ENDED;
    else {
      setError("no decodable audio");
      status_ = IO_ERROR;
    }
  }
}

void DecoderWorker::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    int idx = openReq_.exchange(-1);
    if (idx >= 0) playFile(idx, openOffset_.load());
    if (stop_) {  // covers both "stopped mid-file" and "stop requested while idle"
      if (status_ != UNSUPPORTED && status_ != IO_ERROR && status_ != ENDED) status_ = IDLE;
      stop_ = false;
      xSemaphoreGive(stopped_);
    }
  }
}
