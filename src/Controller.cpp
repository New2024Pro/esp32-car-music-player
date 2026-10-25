#include "Controller.h"
#include "TrackList.h"
#include "Params.h"
#include "config.h"
#include <esp_system.h>

void Controller::begin(DecoderWorker& w, BtAudio& bt, RingBuffer& ring, const std::vector<std::string>& tracks,
                       const std::string& initialError) {
  w_ = &w;
  bt_ = &bt;
  ring_ = &ring;
  tracks_ = &tracks;
  err_ = initialError;
  q_ = xQueueCreate(16, sizeof(Command));
  mu_ = xSemaphoreCreateMutex();
  bt_->setVolume(volume_);
  xTaskCreatePinnedToCore(&Controller::taskEntry, "controller", 6144, this, 2, nullptr, 1);
}

void Controller::setError(const std::string& e) {
  Serial.printf("[ctl] error: %s\n", e.c_str());
  xSemaphoreTake(mu_, portMAX_DELAY);
  err_ = e;
  xSemaphoreGive(mu_);
}

StateSnapshot Controller::snapshot() const {
  StateSnapshot s;
  xSemaphoreTake(mu_, portMAX_DELAY);
  s.play = play_;
  s.track = cur_;
  s.volume = volume_;
  s.error = err_;
  xSemaphoreGive(mu_);
  s.bt = bt_->state();
  s.trackCount = (int)tracks_->size();
  s.underruns = bt_->underruns();
  s.minFreeHeap = ESP.getMinFreeHeap();
  s.resetReason = (int)esp_reset_reason();
  return s;
}

void Controller::run() {
  for (;;) {
    Command c;
    if (xQueueReceive(q_, &c, pdMS_TO_TICKS(50)) == pdTRUE) handle(c);
    tick();
  }
}

// Track change: silence -> stop worker + ack -> drain (wait out in-flight callback) -> flush ->
// open -> prefill -> resume. Each wait is bounded; failures are recoverable (state Stopped).
Controller::OpenResult Controller::openTrack(int idx) {
  bt_->expectAudio(false);
  w_->requestStop();
  if (!w_->waitStopped(WORKER_STOP_TIMEOUT_MS)) {
    setError("decoder did not stop in time");
    return Infra;
  }
  if (!ring_->beginDrain(DRAIN_TIMEOUT_MS)) {
    setError("audio callback busy; could not reset buffer");
    return Infra;
  }
  if (!ring_->flush()) {
    setError("buffer flush refused");
    return Infra;
  }
  w_->open(idx);
  uint32_t t0 = millis();
  while (w_->status() == DecoderWorker::OPENING) {
    if (millis() - t0 > OPEN_TIMEOUT_MS) {
      setError("open timed out");
      return Infra;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  DecoderWorker::Result r = w_->status();
  if (r == DecoderWorker::UNSUPPORTED || r == DecoderWorker::IO_ERROR) {
    setError(w_->lastError() + ": " + (*tracks_)[idx]);
    return Unplayable;
  }
  t0 = millis();
  while (ring_->available() < PREFILL_BYTES && w_->status() == DecoderWorker::PLAYING) {
    if (millis() - t0 > PREFILL_TIMEOUT_MS) {
      setError("prefill timed out");
      return Infra;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  ring_->endDrain();
  return Ok;
}

void Controller::stopPlayback() {
  bt_->expectAudio(false);
  w_->requestStop();
  if (!w_->waitStopped(WORKER_STOP_TIMEOUT_MS)) setError("decoder did not stop in time");
  else if (ring_->beginDrain(DRAIN_TIMEOUT_MS)) ring_->flush();
  xSemaphoreTake(mu_, portMAX_DELAY);
  play_ = PlayState::Stopped;
  xSemaphoreGive(mu_);
  endWaitStart_ = 0;
}

void Controller::startTrack(int idx, int dir) {
  size_t n = tracks_->size();
  if (n == 0) {
    setError("no tracks");
    stopPlayback();
    return;
  }
  size_t probes = 0;
  bool infra = false;
  int i = skipPolicy(idx, dir, n, [&](int k) {
    probes++;
    OpenResult r = openTrack(k);
    if (r == Infra) {
      infra = true;
      return true;  // leave the probe loop; do not keep poking a stuck worker
    }
    return r == Ok;
  });
  if (infra) {
    stopPlayback();
    return;
  }
  if (i < 0) {
    if (probes >= n) setError("no playable tracks");
    stopPlayback();
    return;
  }
  xSemaphoreTake(mu_, portMAX_DELAY);
  cur_ = i;
  play_ = PlayState::Playing;
  err_.clear();
  xSemaphoreGive(mu_);
  endWaitStart_ = 0;
  bt_->expectAudio(true);
}

void Controller::pause() {
  bt_->expectAudio(false);
  if (!ring_->beginDrain(DRAIN_TIMEOUT_MS)) setError("audio callback busy while pausing");  // still muted
  xSemaphoreTake(mu_, portMAX_DELAY);
  play_ = PlayState::Paused;
  xSemaphoreGive(mu_);
}

void Controller::resume() {
  ring_->endDrain();
  bt_->expectAudio(true);
  xSemaphoreTake(mu_, portMAX_DELAY);
  play_ = PlayState::Playing;
  xSemaphoreGive(mu_);
}

void Controller::handle(Command c) {
  switch (c.cmd) {
    case Cmd::Play:
      if (play_ == PlayState::Paused) resume();
      else if (play_ == PlayState::Stopped) startTrack(cur_ < 0 ? 0 : cur_, +1);
      break;
    case Cmd::Pause:
      if (play_ == PlayState::Playing) pause();
      break;
    case Cmd::Next: {
      int i = nextIndex(cur_, tracks_->size());
      if (i < 0) stopPlayback();  // end of list: stop, no wrap
      else startTrack(i, +1);
      break;
    }
    case Cmd::Prev: {
      int i = prevIndex(cur_, tracks_->size());
      if (i < 0) setError("no tracks");
      else startTrack(i, -1);
      break;
    }
    case Cmd::Track:
      if (c.arg < 0 || (size_t)c.arg >= tracks_->size()) setError("bad track index");
      else startTrack(c.arg, +1);
      break;
    case Cmd::Volume:
      volume_ = clampVolume(c.arg);
      bt_->setVolume((uint8_t)volume_);
      break;
  }
}

// Advance when the file ended: stop counting underruns, let the ring play out, then move on.
void Controller::tick() {
  if (play_ != PlayState::Playing) return;
  DecoderWorker::Result r = w_->status();
  if (r != DecoderWorker::ENDED && r != DecoderWorker::IO_ERROR) return;
  if (endWaitStart_ == 0) {
    endWaitStart_ = millis();
    bt_->expectAudio(false);
    if (r == DecoderWorker::IO_ERROR) setError(w_->lastError());
  }
  if (ring_->available() > 0 && millis() - endWaitStart_ < 3000) return;
  int i = nextIndex(cur_, tracks_->size());
  if (i < 0) stopPlayback();
  else startTrack(i, +1);
}

