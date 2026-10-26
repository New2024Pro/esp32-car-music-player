#include "Controller.h"
#include "TrackList.h"
#include "Params.h"
#include "Mp3Info.h"
#include "config.h"
#include <Preferences.h>
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
  Preferences p;
  if (p.begin("mp3", true)) {
    shuffle_ = p.getBool("shuffle", false);
    uint8_t r = p.getUChar("repeat", 0);
    repeat_ = r == (uint8_t)Repeat::All ? Repeat::All : r == (uint8_t)Repeat::One ? Repeat::One : Repeat::Off;
    p.end();
  }
  shuffleShown_ = shuffle_;
  repeatShown_ = repeat_;
  order_ = makeOrder(tracks.size(), shuffle_, -1, -1, [](uint32_t b) { return esp_random() % b; });
  bt_->setVolume(volume_);
  xTaskCreatePinnedToCore(&Controller::taskEntry, "controller", 8192, this, 2, nullptr, 1);
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
  s.shuffle = shuffleShown_;
  s.repeat = repeatShown_;
  s.error = err_;
  xSemaphoreGive(mu_);
  s.bt = bt_->state();
  BtAudio::PairInfo p = bt_->pairing();
  s.pair = p.kind;
  s.pairCode = p.code;
  s.pairMsg = p.msg;
  s.btDevice = bt_->connectedDeviceName();
  s.trackCount = (int)tracks_->size();
  DecoderWorker::Progress wp = w_->progress();
  if (s.play != PlayState::Stopped && s.track >= 0 && wp.bitrate && wp.size > wp.start) {
    s.durSec = (int)mp3BytesToSec(wp.size - wp.start, wp.bitrate);
    // Decoder input runs ahead of the speaker by the PCM still queued in the ring.
    int read = (int)mp3BytesToSec(wp.pos > wp.start ? wp.pos - wp.start : 0, wp.bitrate);
    int queued = (int)(ring_->available() / (44100 * 4));
    int pos = read - queued;
    s.posSec = pos < 0 ? 0 : (pos > s.durSec ? s.durSec : pos);
  }
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
Controller::OpenResult Controller::openTrack(int idx, uint32_t byteOffset) {
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
  w_->open(idx, byteOffset);
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
  bt_->setStreaming(false);
  w_->requestStop();
  if (!w_->waitStopped(WORKER_STOP_TIMEOUT_MS)) setError("decoder did not stop in time");
  else if (ring_->beginDrain(DRAIN_TIMEOUT_MS)) ring_->flush();
  xSemaphoreTake(mu_, portMAX_DELAY);
  play_ = PlayState::Stopped;
  xSemaphoreGive(mu_);
  endWaitStart_ = 0;
}

// `pos` indexes the play order (shuffled or not); unplayable files are skipped along the order.
void Controller::startTrack(int pos, int dir) {
  size_t n = tracks_->size();
  if (n == 0) {
    setError("no tracks");
    stopPlayback();
    return;
  }
  int targetPos = (pos >= 0 && (size_t)pos < n) ? pos : 0;
  xSemaphoreTake(mu_, portMAX_DELAY);
  cur_ = order_[targetPos];
  play_ = PlayState::Playing;
  err_.clear();
  xSemaphoreGive(mu_);

  size_t probes = 0;
  bool infra = false;
  int i = skipPolicy(targetPos, dir, n, [&](int k) {
    probes++;
    OpenResult r = openTrack(order_[k]);
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
  cur_ = order_[i];
  play_ = PlayState::Playing;
  err_.clear();
  xSemaphoreGive(mu_);
  Serial.printf("[ctl] playing track %d (%d of %u in %s order)\n", cur_ + 1, i + 1, (unsigned)n,
                shuffle_ ? "shuffled" : "normal");
  endWaitStart_ = 0;
  bt_->setStreaming(true);
  bt_->expectAudio(true);
}

// Order position to resume from: the current track, else the start of the order.
int Controller::currentPos() const {
  int p = cur_ < 0 ? -1 : orderPosOf(order_, cur_);
  return p < 0 ? 0 : p;
}

// Moves to the next position; at the end of a shuffled list, reshuffles before wrapping.
void Controller::advance(bool autoAdvance) {
  size_t n = tracks_->size();
  int from = cur_ < 0 ? -1 : orderPosOf(order_, cur_);
  int p = nextPos(from, n, repeat_, autoAdvance);
  if (p < 0) {
    stopPlayback();  // end of list, repeat off
    return;
  }
  if (shuffle_ && from >= 0 && p == 0 && (size_t)from + 1 >= n) {
    order_ = makeOrder(n, true, -1, cur_, [](uint32_t b) { return esp_random() % b; });
  }
  startTrack(p, +1);
}

void Controller::setShuffle(bool on) {
  shuffle_ = on;
  order_ = makeOrder(tracks_->size(), on, cur_, -1, [](uint32_t b) { return esp_random() % b; });
  saveModes();
}

void Controller::setRepeat(Repeat r) {
  repeat_ = r;
  saveModes();
}

void Controller::saveModes() {
  xSemaphoreTake(mu_, portMAX_DELAY);
  shuffleShown_ = shuffle_;
  repeatShown_ = repeat_;
  xSemaphoreGive(mu_);
  Preferences p;
  if (p.begin("mp3", false)) {
    p.putBool("shuffle", shuffle_);
    p.putUChar("repeat", (uint8_t)repeat_);
    p.end();
  }
  Serial.printf("[ctl] shuffle=%s repeat=%s\n", shuffle_ ? "on" : "off", toStr(repeat_));
}

// Same reset sequence as a track change, reopening the current file at the byte offset for
// `sec`. A paused track stays paused at the new position.
void Controller::seek(int sec) {
  if (play_ == PlayState::Stopped || cur_ < 0) {
    setError("nothing playing to seek");
    return;
  }
  DecoderWorker::Progress p = w_->progress();
  if (!p.bitrate || p.size <= p.start) {
    setError("track length unknown; cannot seek yet");
    return;
  }
  uint32_t dur = mp3BytesToSec(p.size - p.start, p.bitrate);
  uint32_t s = sec < 0 ? 0 : (uint32_t)sec;
  if (s + 2 > dur) s = dur > 2 ? dur - 2 : 0;  // keep a little audio so "end" still advances
  bool wasPaused = play_ == PlayState::Paused;
  Serial.printf("[ctl] seek to %us of %us (%u kbps)\n", (unsigned)s, (unsigned)dur, (unsigned)(p.bitrate / 1000));
  OpenResult r = openTrack(cur_, p.start + mp3SecToBytes(s, p.bitrate));
  if (r != Ok) {
    stopPlayback();  // openTrack already reported why
    return;
  }
  endWaitStart_ = 0;
  if (wasPaused) pause();
  else bt_->expectAudio(true);
}

void Controller::pause() {
  bt_->expectAudio(false);
  bt_->setStreaming(false);
  if (!ring_->beginDrain(DRAIN_TIMEOUT_MS)) setError("audio callback busy while pausing");  // still muted
  xSemaphoreTake(mu_, portMAX_DELAY);
  play_ = PlayState::Paused;
  endWaitStart_ = 0;
  xSemaphoreGive(mu_);
}

void Controller::resume() {
  ring_->endDrain();
  bt_->setStreaming(true);
  bt_->expectAudio(true);
  xSemaphoreTake(mu_, portMAX_DELAY);
  play_ = PlayState::Playing;
  xSemaphoreGive(mu_);
}

void Controller::setVolumeImmediate(int arg) {
  int v = clampVolume(arg);
  xSemaphoreTake(mu_, portMAX_DELAY);
  volume_ = v;
  xSemaphoreGive(mu_);
  bt_->setVolume((uint8_t)v);
}

void Controller::onRemoteKey(uint8_t key) {
  const char* name = "?";
  bool ok = true;
  switch (key) {
    // We cannot report play status back (no AVRCP play-status support in this stack), so sinks
    // guess: AirPods send "pause" on every press. A request that matches the current state is
    // therefore taken as a toggle.
    case ESP_AVRC_PT_CMD_PLAY:
    case ESP_AVRC_PT_CMD_PAUSE:
    case ESP_AVRC_PT_CMD_STOP: {
      bool playing = snapshot().play == PlayState::Playing;
      bool wantPlay = key == ESP_AVRC_PT_CMD_PLAY ? !playing : !playing && key == ESP_AVRC_PT_CMD_PAUSE;
      name = wantPlay ? "play" : "pause";
      ok = enqueue({wantPlay ? Cmd::Play : Cmd::Pause, 0});
      break;
    }
    case ESP_AVRC_PT_CMD_FORWARD: name = "next"; ok = enqueue({Cmd::Next, 0}); break;
    case ESP_AVRC_PT_CMD_BACKWARD: name = "previous"; ok = enqueue({Cmd::Prev, 0}); break;
    case ESP_AVRC_PT_CMD_FAST_FORWARD:
    case ESP_AVRC_PT_CMD_REWIND: {
      name = key == ESP_AVRC_PT_CMD_FAST_FORWARD ? "fast-forward" : "rewind";
      int pos = snapshot().posSec;
      if (pos >= 0) ok = enqueue({Cmd::Seek, pos + (key == ESP_AVRC_PT_CMD_FAST_FORWARD ? 10 : -10)});
      break;
    }
    default:
      break;
  }
  Serial.printf("[bt] remote button 0x%02x (%s)%s\n", key, name, ok ? "" : " dropped: queue full");
}

void Controller::handle(Command c) {
  switch (c.cmd) {
    case Cmd::BtConnect:
      bt_->triggerScan();
      break;
    case Cmd::Play:
      if (play_ == PlayState::Paused) resume();
      else if (play_ == PlayState::Stopped) startTrack(currentPos(), +1);
      break;
    case Cmd::Pause:
      if (play_ == PlayState::Playing) pause();
      break;
    case Cmd::Next:
      advance(false);
      break;
    case Cmd::Prev: {
      // Like a phone: more than 3 s in, Previous restarts the track; otherwise goes back one.
      if (play_ != PlayState::Stopped && snapshot().posSec > 3) {
        seek(0);
        break;
      }
      int i = prevPos(cur_ < 0 ? 0 : orderPosOf(order_, cur_), tracks_->size(), repeat_);
      if (i < 0) setError("no tracks");
      else startTrack(i, -1);
      break;
    }
    case Cmd::Track:
      if (c.arg < 0 || (size_t)c.arg >= tracks_->size()) setError("bad track index");
      else startTrack(orderPosOf(order_, c.arg), +1);
      break;
    case Cmd::Shuffle:
      setShuffle(c.arg != 0);
      break;
    case Cmd::Repeat:
      setRepeat(c.arg == (int)Repeat::All ? Repeat::All : c.arg == (int)Repeat::One ? Repeat::One : Repeat::Off);
      break;
    case Cmd::Volume: {
      setVolumeImmediate(c.arg);
      break;
    }
    case Cmd::Seek:
      seek(c.arg);
      break;
  }
}

// Advance when the file ended: stop counting underruns, let the ring play out, then move on.
void Controller::tick() {
  if (!autoPlayed_ && bt_->state() == BtState::Connected) {
    autoPlayed_ = true;
    if (play_ == PlayState::Stopped && !tracks_->empty()) {
      Serial.println("[ctl] Bluetooth connected -> auto-starting playback");
      startTrack(currentPos(), +1);
      return;
    }
  }
  if (play_ != PlayState::Playing) return;
  DecoderWorker::Result r = w_->status();
  if (r != DecoderWorker::ENDED && r != DecoderWorker::IO_ERROR) return;
  if (endWaitStart_ == 0) {
    endWaitStart_ = millis();
    bt_->expectAudio(false);
    if (r == DecoderWorker::IO_ERROR) setError(w_->lastError());
  }
  if (ring_->available() > 0 && millis() - endWaitStart_ < 3000) return;
  advance(r == DecoderWorker::ENDED);  // a read error moves on even with repeat-one
}

