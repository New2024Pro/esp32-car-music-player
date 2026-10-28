#include <string>
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

TEST(mp3_first_sample_rate) {
  const uint8_t mpeg1_44k[] = {0x00, 0x11, 0xFF, 0xFB, 0x90, 0x64};  // junk, then MPEG1 L3 128k 44.1k
  CHECK_EQ(mp3FirstSampleRate(mpeg1_44k, sizeof mpeg1_44k), 44100u);
  const uint8_t mpeg1_48k[] = {0xFF, 0xFB, 0x94, 0x64};
  CHECK_EQ(mp3FirstSampleRate(mpeg1_48k, sizeof mpeg1_48k), 48000u);
  const uint8_t mpeg2_16k[] = {0xFF, 0xF3, 0x98, 0x64};  // MPEG2 L3, rate index 2
  CHECK_EQ(mp3FirstSampleRate(mpeg2_16k, sizeof mpeg2_16k), 16000u);
  const uint8_t layer2[] = {0xFF, 0xFD, 0x90, 0x64};  // MPEG1 layer II: not accepted as MP3
  CHECK_EQ(mp3FirstSampleRate(layer2, sizeof layer2), 0u);
  const uint8_t none[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0};
  CHECK_EQ(mp3FirstSampleRate(none, sizeof none), 0u);
}

TEST(other_audio_formats_are_named) {
  const uint8_t m4a[] = {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '};
  CHECK(std::string(otherAudioFormat(m4a, sizeof m4a)) == "M4A/AAC");
  const uint8_t adts[] = {0xFF, 0xF1, 0x50, 0x80};
  CHECK(std::string(otherAudioFormat(adts, sizeof adts)) == "AAC");
  const uint8_t wav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0};
  CHECK(std::string(otherAudioFormat(wav, sizeof wav)) == "WAV");
  const uint8_t mp3[] = {0xFF, 0xFB, 0x90, 0x64};  // real MP3 frame: not "other"
  CHECK(otherAudioFormat(mp3, sizeof mp3) == nullptr);
  const uint8_t id3[] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
  CHECK(otherAudioFormat(id3, sizeof id3) == nullptr);
}

TEST(mp3_time_byte_mapping) {
  CHECK_EQ(mp3SecToBytes(10, 128000), 160000u);
  CHECK_EQ(mp3BytesToSec(160000, 128000), 10u);
  CHECK_EQ(mp3BytesToSec(4800000, 320000), 120u);
  CHECK_EQ(mp3BytesToSec(1000, 0), 0u);  // bitrate unknown
  CHECK_EQ(mp3SecToBytes(3600, 320000), 144000000u);
}
