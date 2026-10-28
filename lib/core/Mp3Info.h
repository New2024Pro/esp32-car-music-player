#pragma once
#include <cstddef>
#include <cstdint>

// Length of a leading ID3v2 tag (header + body + optional footer) from the first file bytes;
// 0 when there is none. Audio frames start at this offset.
inline uint32_t id3v2Size(const uint8_t* h, size_t n) {
  if (n < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
  if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;  // size bytes must be syncsafe
  uint32_t body = ((uint32_t)h[6] << 21) | ((uint32_t)h[7] << 14) | ((uint32_t)h[8] << 7) | h[9];
  return 10 + body + ((h[5] & 0x10) ? 10 : 0);
}

// Sample rate (Hz) of the first MPEG layer III frame header in p[0..n), 0 when none is found.
// Used to reject uploads the player cannot decode (only 44.1 kHz stereo plays).
inline uint32_t mp3FirstSampleRate(const uint8_t* p, size_t n) {
  // [version bits][rate index]: version 0 = MPEG 2.5, 1 = reserved, 2 = MPEG 2, 3 = MPEG 1
  static const uint32_t rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
  for (size_t i = 0; i + 4 <= n; i++) {
    if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) continue;
    uint8_t ver = (p[i + 1] >> 3) & 3, layer = (p[i + 1] >> 1) & 3;
    uint8_t br = p[i + 2] >> 4, sr = (p[i + 2] >> 2) & 3;
    if (ver == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) continue;  // not a layer III header
    return rates[ver][sr];
  }
  return 0;
}

// Common audio formats that arrive renamed to .mp3 (YouTube converters, mail attachments). Returns
// the format's name from the first bytes of the file, or nullptr when it is none of them.
inline const char* otherAudioFormat(const uint8_t* p, size_t n) {
  if (n >= 8 && p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p') return "M4A/AAC";
  if (n >= 4 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F') return "WAV";
  if (n >= 4 && p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') return "OGG";
  if (n >= 4 && p[0] == 'f' && p[1] == 'L' && p[2] == 'a' && p[3] == 'C') return "FLAC";
  if (n >= 4 && p[0] == 0x1A && p[1] == 0x45 && p[2] == 0xDF && p[3] == 0xA3) return "WebM/Opus";
  if (n >= 2 && p[0] == 0xFF && (p[1] & 0xF6) == 0xF0) return "AAC";  // ADTS: layer bits 00
  return nullptr;
}

// Byte <-> time mapping at a constant bitrate (bits/s). Exact for CBR, an estimate for VBR.
inline uint32_t mp3BytesToSec(uint32_t bytes, uint32_t bitrate) {
  return bitrate ? (uint32_t)((uint64_t)bytes * 8 / bitrate) : 0;
}
inline uint32_t mp3SecToBytes(uint32_t sec, uint32_t bitrate) { return (uint32_t)((uint64_t)sec * bitrate / 8); }
