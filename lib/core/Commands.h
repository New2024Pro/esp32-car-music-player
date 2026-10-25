#pragma once
#include <stdint.h>
#include <string>

enum class Cmd : uint8_t { Play, Pause, Next, Prev, Volume, Track };
struct Command {
  Cmd cmd;
  int arg;  // Volume: 0..100, Track: index; unused otherwise
};

enum class PlayState : uint8_t { Stopped, Playing, Paused };
enum class BtState : uint8_t { Disconnected, Connecting, Connected };

inline const char* toStr(PlayState s) {
  return s == PlayState::Playing ? "playing" : s == PlayState::Paused ? "paused" : "stopped";
}
inline const char* toStr(BtState s) {
  return s == BtState::Connected ? "connected" : s == BtState::Connecting ? "connecting" : "disconnected";
}

struct StateSnapshot {
  PlayState play = PlayState::Stopped;
  BtState bt = BtState::Disconnected;
  int track = -1;
  int trackCount = 0;
  int volume = 50;
  std::string error;
  uint32_t underruns = 0;
  uint32_t minFreeHeap = 0;
  int resetReason = 0;
};

