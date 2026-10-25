#include "DecoderWorker.h"
#include <SD.h>

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

void DecoderWorker::open(int index) {
  status_ = OPENING;
  openReq_ = index;
  xTaskNotifyGive(task_);
}

void DecoderWorker::requestStop() {
  xSemaphoreTake(stopped_, 0);  // drop any stale ack
  stop_ = true;
  xTaskNotifyGive(task_);
}

bool DecoderWorker::waitStopped(uint32_t ms) { return xSemaphoreTake(stopped_, pdMS_TO_TICKS(ms)) == pdTRUE; }

void DecoderWorker::onPcm(libhelix::MP3FrameInfo& info, short* pcm, size_t len, void*) {
  if (inst_) inst_->handlePcm(info, pcm, len);
}

void DecoderWorker::handlePcm(libhelix::MP3FrameInfo& info, const short* pcm, size_t len) {
  if (!fmtChecked_) {
    if (info.samprate != 44100 || info.nChans != 2 || info.bitsPerSample != 16) {
      char m[64];
      snprintf(m, sizeof m, "unsupported format: %d Hz, %d ch", info.samprate, info.nChans);
      setError(m);
      status_ = UNSUPPORTED;
      abort_ = true;
      return;
    }
    fmtChecked_ = true;
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

void DecoderWorker::playFile(int index) {
  abort_ = false;
  fmtChecked_ = false;
  if (index < 0 || (size_t)index >= tracks_->size()) {
    setError("bad track index");
    status_ = IO_ERROR;
    return;
  }
  std::string path = "/" + (*tracks_)[index];
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    setError("cannot open file");
    status_ = IO_ERROR;
    return;
  }
  decoder_.begin();
  uint8_t buf[512];
  while (!stop_ && !abort_) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
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
    if (idx >= 0) playFile(idx);
    if (stop_) {  // covers both "stopped mid-file" and "stop requested while idle"
      if (status_ != UNSUPPORTED && status_ != IO_ERROR && status_ != ENDED) status_ = IDLE;
      stop_ = false;
      xSemaphoreGive(stopped_);
    }
  }
}
