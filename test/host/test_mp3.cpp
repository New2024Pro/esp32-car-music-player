#include "Mp3Info.h"
#include "mini_test.h"

TEST(id3v2Size_parses_syncsafe_header) {
  const uint8_t tag[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0x02, 0x01};  // body = 2*128 + 1 = 257
  CHECK_EQ(id3v2Size(tag, 10), 267u);
  const uint8_t footer[10] = {'I', 'D', '3', 4, 0, 0x10, 0, 0, 0x02, 0x01};
  CHECK_EQ(id3v2Size(footer, 10), 277u);
}

TEST(id3v2Size_rejects_non_tags) {
  const uint8_t frame[10] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0};
  CHECK_EQ(id3v2Size(frame, 10), 0u);
  const uint8_t badSize[10] = {'I', 'D', '3', 4, 0, 0, 0x80, 0, 0, 0};
  CHECK_EQ(id3v2Size(badSize, 10), 0u);
  const uint8_t tag[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0x02, 0x01};
  CHECK_EQ(id3v2Size(tag, 9), 0u);
}

TEST(mp3_time_byte_mapping) {
  CHECK_EQ(mp3SecToBytes(10, 128000), 160000u);
  CHECK_EQ(mp3BytesToSec(160000, 128000), 10u);
  CHECK_EQ(mp3BytesToSec(4800000, 320000), 120u);
  CHECK_EQ(mp3BytesToSec(1000, 0), 0u);  // bitrate unknown
  CHECK_EQ(mp3SecToBytes(3600, 320000), 144000000u);
}
