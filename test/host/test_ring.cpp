#include "RingBuffer.h"
#include "mini_test.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>

static std::vector<uint8_t> seq(size_t n, uint8_t start = 0) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(start + i);
  return v;
}

TEST(ring_wrap_preserves_order) {
  uint8_t mem[16];
  RingBuffer r(mem, 16);
  auto a = seq(12);
  CHECK_EQ(r.write(a.data(), 12), 12u);
  uint8_t out[12];
  CHECK_EQ(r.fillPcm(out, 8), 8u);  // consume 8, head/tail now wrapped soon
  auto b = seq(12, 100);
  CHECK_EQ(r.write(b.data(), 12), 12u);  // wraps around the end
  uint8_t got[16];
  CHECK_EQ(r.fillPcm(got, 16), 16u);
  CHECK(memcmp(got, a.data() + 8, 4) == 0);
  CHECK(memcmp(got + 4, b.data(), 12) == 0);
}

TEST(ring_write_accepts_whole_frames_only) {
  uint8_t mem[16];
  RingBuffer r(mem, 16);
  auto a = seq(7);
  CHECK_EQ(r.write(a.data(), 7), 4u);
  CHECK_EQ(r.available(), 4u);
  auto b = seq(64);
  CHECK_EQ(r.write(b.data(), 64), 12u);  // only 12 free
  CHECK_EQ(r.freeSpace(), 0u);
}

TEST(fillPcm_returns_whole_frames_and_zero_fills_remainder) {
  uint8_t mem[64];
  RingBuffer r(mem, 64);
  auto a = seq(12, 1);  // 3 frames
  r.write(a.data(), 12);
  uint8_t out[16];
  memset(out, 0xAA, sizeof out);
  CHECK_EQ(r.fillPcm(out, 16), 12u);
  CHECK(memcmp(out, a.data(), 12) == 0);
  for (int i = 12; i < 16; i++) CHECK_EQ(out[i], 0);
  uint8_t odd[7];  // request not a multiple of 4: still whole frames only
  memset(odd, 0xAA, sizeof odd);
  r.write(a.data(), 12);
  CHECK_EQ(r.fillPcm(odd, 7), 4u);
  for (int i = 4; i < 7; i++) CHECK_EQ(odd[i], 0);
}

TEST(drain_flush_endDrain_cycle) {
  uint8_t mem[64];
  RingBuffer r(mem, 64);
  auto a = seq(32);
  r.write(a.data(), 32);
  CHECK(!r.flush());  // refuses while not draining
  CHECK(r.beginDrain(50));
  CHECK(r.flush());
  CHECK_EQ(r.available(), 0u);
  uint8_t o[8];
  memset(o, 0xAA, 8);
  CHECK_EQ(r.fillPcm(o, 8), 0u);  // silence while draining
  for (int i = 0; i < 8; i++) CHECK_EQ(o[i], 0);
  auto b = seq(8, 200);
  r.write(b.data(), 8);
  r.endDrain();
  CHECK_EQ(r.fillPcm(o, 8), 8u);
  CHECK(memcmp(o, b.data(), 8) == 0);
}

static std::atomic<bool> g_inHook{false}, g_release{false};
static void blockingHook(void*) {
  g_inHook = true;
  while (!g_release) std::this_thread::yield();
}

// Track change during an active read: the in-flight consumer must finish before indices reset.
TEST(track_change_during_active_read) {
  uint8_t mem[4096];
  RingBuffer r(mem, 4096);
  auto a = seq(4096);
  r.write(a.data(), 4096);
  g_inHook = false;
  g_release = false;
  r.midReadHook = blockingHook;
  std::vector<uint8_t> out(1024, 0xEE);
  size_t got = 0;
  std::thread consumer([&] { got = r.fillPcm(out.data(), 1024); });
  while (!g_inHook) std::this_thread::yield();
  CHECK(!r.beginDrain(30));  // detects in-flight read, times out, never blocks forever
  CHECK(!r.flush());         // refuses to reset indices under an active reader
  r.midReadHook = nullptr;
  g_release = true;
  consumer.join();
  CHECK_EQ(got, 1024u);
  CHECK(memcmp(out.data(), a.data(), 1024) == 0);  // in-flight read not corrupted
  CHECK(r.beginDrain(100));                         // retry succeeds after the read finished
  CHECK(r.flush());
  CHECK_EQ(r.available(), 0u);
  uint8_t o[8];
  CHECK_EQ(r.fillPcm(o, 8), 0u);
  r.endDrain();
}

TEST(spsc_stress_in_order) {
  static uint8_t mem[4096];
  RingBuffer r(mem, 4096);
  const size_t total = 1 << 20;
  std::atomic<bool> bad{false};
  std::thread prod([&] {
    size_t sent = 0;
    while (sent < total) {
      uint8_t chunk[252];
      for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = (uint8_t)(sent + i);
      size_t w = r.write(chunk, std::min(sizeof chunk, total - sent));
      sent += w;
      if (!w) std::this_thread::yield();
    }
  });
  size_t recv = 0;
  uint8_t buf[300];
  while (recv < total) {
    size_t g = r.fillPcm(buf, sizeof buf);
    for (size_t i = 0; i < g; i++)
      if (buf[i] != (uint8_t)(recv + i)) bad = true;
    recv += g;
    if (!g) std::this_thread::yield();
  }
  prod.join();
  CHECK(!bad);
}

// Consumer spins while the controller repeatedly drains, flushes and refills.
TEST(drain_stress_no_cross_iteration_reads) {
  static uint8_t mem[1024];
  RingBuffer r(mem, 1024);
  std::atomic<bool> stop{false}, bad{false};
  std::thread consumer([&] {
    uint8_t buf[256];
    while (!stop) {
      size_t g = r.fillPcm(buf, sizeof buf);
      for (size_t i = 1; i < g; i++)
        if (buf[i] != buf[0]) bad = true;  // a read must never span a flush
      if (!g) std::this_thread::yield();
    }
  });
  for (int it = 0; it < 2000; it++) {
    CHECK(r.beginDrain(1000));
    CHECK(r.flush());
    uint8_t blk[64];
    memset(blk, 1 + it % 250, sizeof blk);
    r.write(blk, sizeof blk);
    r.endDrain();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  stop = true;
  consumer.join();
  CHECK(!bad);
}

