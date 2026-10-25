#pragma once
#include <atomic>
#include <cstring>
#include <cstddef>
#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
inline uint32_t ringNowMs() { return millis(); }
inline void ringPause() { delay(1); }
#else
#include <chrono>
#include <thread>
inline uint32_t ringNowMs() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
inline void ringPause() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
#endif

// SPSC byte ring of 4-byte PCM frames (16-bit stereo).
//   producer (decoder worker): write()
//   consumer (A2DP callback):  fillPcm()  -- never blocks, never waits
//   controller:                beginDrain() / flush() / endDrain()
//
// Reset protocol: beginDrain() raises `draining_`, then waits (bounded) for an in-flight
// fillPcm() to finish. fillPcm() raises `inRead_` BEFORE it checks `draining_` (both seq_cst),
// so either it sees draining and returns silence, or beginDrain() sees inRead and waits.
// flush() only resets indices when draining and no read is in flight. The producer must
// already be stopped and acknowledged (DecoderWorker::waitStopped) before flush().
class RingBuffer {
 public:
  // capacity must be a power of two and a multiple of 4 (free-running 32-bit indices).
  RingBuffer(uint8_t* mem, size_t capacity) : mem_(mem), cap_((uint32_t)capacity) {}

  // Producer only. Accepts whole frames; returns bytes accepted.
  size_t write(const uint8_t* d, size_t n) {
    uint32_t h = head_.load(std::memory_order_relaxed);
    uint32_t t = tail_.load(std::memory_order_acquire);
    size_t space = cap_ - (h - t);
    if (n > space) n = space;
    n &= ~(size_t)3;
    size_t off = h & (cap_ - 1);
    size_t first = n < cap_ - off ? n : cap_ - off;
    memcpy(mem_ + off, d, first);
    memcpy(mem_, d + first, n - first);
    head_.store(h + (uint32_t)n, std::memory_order_release);
    return n;
  }

  // Consumer only. Copies whole frames, zero-fills the rest, returns real PCM bytes copied.
  // While draining it returns 0 (all silence) without touching the indices.
  size_t fillPcm(uint8_t* out, size_t n) {
    inRead_.store(true, std::memory_order_seq_cst);
    if (draining_.load(std::memory_order_seq_cst)) {
      inRead_.store(false, std::memory_order_release);
      memset(out, 0, n);
      return 0;
    }
    uint32_t t = tail_.load(std::memory_order_relaxed);
    uint32_t h = head_.load(std::memory_order_acquire);
    size_t avail = h - t;
    size_t take = (n < avail ? n : avail) & ~(size_t)3;
    if (midReadHook) midReadHook(hookArg);
    size_t off = t & (cap_ - 1);
    size_t first = take < cap_ - off ? take : cap_ - off;
    memcpy(out, mem_ + off, first);
    memcpy(out + first, mem_, take - first);
    tail_.store(t + (uint32_t)take, std::memory_order_release);
    inRead_.store(false, std::memory_order_release);
    if (take < n) memset(out + take, 0, n - take);
    return take;
  }

  size_t available() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
  size_t freeSpace() const { return cap_ - available(); }
  bool draining() const { return draining_.load(std::memory_order_acquire); }

  // Controller only. Returns false if an in-flight read did not finish within timeoutMs;
  // draining stays set (callback keeps emitting silence) so the caller may retry or recover.
  bool beginDrain(uint32_t timeoutMs) {
    draining_.store(true, std::memory_order_seq_cst);
    uint32_t start = ringNowMs();
    while (inRead_.load(std::memory_order_seq_cst)) {
      if (ringNowMs() - start >= timeoutMs) return false;
      ringPause();
    }
    return true;
  }

  // Controller only; producer must be stopped. Refuses unless draining and no real read is in
  // flight. inRead_ is raised briefly even by callbacks that then see `draining_` and back off,
  // so wait (bounded) for it to clear instead of failing on that transient.
  bool flush(uint32_t timeoutMs = 50) {
    if (!draining_.load(std::memory_order_seq_cst)) return false;
    uint32_t start = ringNowMs();
    while (inRead_.load(std::memory_order_seq_cst)) {
      if (ringNowMs() - start >= timeoutMs) return false;
      ringPause();
    }
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_release);
    return true;
  }

  void endDrain() { draining_.store(false, std::memory_order_seq_cst); }

  // Test seam: runs inside fillPcm after the guard is taken. Null in firmware.
  void (*midReadHook)(void*) = nullptr;
  void* hookArg = nullptr;

 private:
  uint8_t* mem_;
  uint32_t cap_;
  std::atomic<uint32_t> head_{0}, tail_{0};
  std::atomic<bool> draining_{false}, inRead_{false};
};
