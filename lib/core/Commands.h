#pragma once
#include <stdint.h>
#include <string>

enum class Cmd : uint8_t { Play, Pause, Next, Prev, Volume, Track, BtConnect, Seek, Shuffle, Repeat };
struct Command {
  Cmd cmd;
  // Volume: 0..100, Track: index, Seek: seconds into the track, Shuffle: 0/1,
  // Repeat: (int)Repeat; unused otherwise
  int arg;
};

// One: the current track restarts when it ends (manual Next still moves on).
enum class Repeat : uint8_t { Off, All, One };
inline const char* toStr(Repeat r) { return r == Repeat::All ? "all" : r == Repeat::One ? "one" : "off"; }

enum class PlayState : uint8_t { Stopped, Playing, Paused };
enum class BtState : uint8_t { Disconnected, Connecting, Connected };

inline const char* toStr(PlayState s) {
  return s == PlayState::Playing ? "playing" : s == PlayState::Paused ? "paused" : "stopped";
}
inline const char* toStr(BtState s) {
  return s == BtState::Connected ? "connected" : s == BtState::Connecting ? "connecting" : "disconnected";
}

// Pending Bluetooth pairing step (first connection to a car navi / speaker).
//   Confirm: navi shows a 6-digit code and asks if this device shows the same -> accept/reject
//   Passkey: navi shows a 6-digit code that must be typed in here
//   Pin:     legacy pairing, navi shows a fixed PIN (often 0000 / 1234) that must be typed in here
//   Show:    this device shows a code that must be typed into the navi
enum class PairKind : uint8_t { None, Confirm, Passkey, Pin, Show };
inline const char* toStr(PairKind k) {
  switch (k) {
    case PairKind::Confirm: return "confirm";
    case PairKind::Passkey: return "passkey";
    case PairKind::Pin: return "pin";
    case PairKind::Show: return "show";
    default: return "none";
  }
}

struct StateSnapshot {
  PlayState play = PlayState::Stopped;
  BtState bt = BtState::Disconnected;
  PairKind pair = PairKind::None;
  uint32_t pairCode = 0;  // Confirm / Show: the code to compare or type into the navi
  std::string pairMsg;    // last pairing outcome, e.g. "paired" / "pairing failed (status 5)"
  std::string btDevice;
  int track = -1;
  int trackCount = 0;
  bool shuffle = false;
  Repeat repeat = Repeat::Off;
  int posSec = -1;  // heard position in the current track; -1 unknown
  int durSec = -1;  // track length (exact for CBR, estimate for VBR); -1 unknown
  int volume = 50;
  std::string error;
  uint32_t underruns = 0;
  uint32_t minFreeHeap = 0;
  int resetReason = 0;
};

