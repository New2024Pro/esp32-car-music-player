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

// Byte <-> time mapping at a constant bitrate (bits/s). Exact for CBR, an estimate for VBR.
inline uint32_t mp3BytesToSec(uint32_t bytes, uint32_t bitrate) {
  return bitrate ? (uint32_t)((uint64_t)bytes * 8 / bitrate) : 0;
}
inline uint32_t mp3SecToBytes(uint32_t sec, uint32_t bitrate) { return (uint32_t)((uint64_t)sec * bitrate / 8); }
