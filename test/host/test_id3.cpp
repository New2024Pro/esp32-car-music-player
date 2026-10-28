#include <vector>
#include "Id3.h"
#include "mini_test.h"

// Builds an ID3v2.3/2.4 tag from (id, encoding, raw text bytes) frames.
static std::vector<uint8_t> makeTag(uint8_t ver, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& frames) {
  std::vector<uint8_t> body;
  for (auto& fr : frames) {
    size_t sz = fr.second.size();
    body.insert(body.end(), fr.first.begin(), fr.first.end());
    if (ver == 4) {
      body.push_back((sz >> 21) & 0x7F); body.push_back((sz >> 14) & 0x7F);
      body.push_back((sz >> 7) & 0x7F);  body.push_back(sz & 0x7F);
    } else {
      body.push_back((sz >> 24) & 0xFF); body.push_back((sz >> 16) & 0xFF);
      body.push_back((sz >> 8) & 0xFF);  body.push_back(sz & 0xFF);
    }
    body.push_back(0); body.push_back(0);  // flags
    body.insert(body.end(), fr.second.begin(), fr.second.end());
  }
  for (int i = 0; i < 20; i++) body.push_back(0);  // padding
  size_t s = body.size();
  std::vector<uint8_t> tag = {'I', 'D', '3', ver, 0, 0, (uint8_t)((s >> 21) & 0x7F), (uint8_t)((s >> 14) & 0x7F),
                              (uint8_t)((s >> 7) & 0x7F), (uint8_t)(s & 0x7F)};
  tag.insert(tag.end(), body.begin(), body.end());
  return tag;
}

static std::vector<uint8_t> latin1(const char* s) {
  std::vector<uint8_t> v = {0};
  for (; *s; s++) v.push_back((uint8_t)*s);
  return v;
}

TEST(id3_v23_latin1_frames) {
  auto tag = makeTag(3, {{"TIT2", latin1("MAPA")}, {"TPE1", latin1("SB19")}, {"TALB", latin1("Pagsibol")}});
  Id3Tags t;
  CHECK(parseId3v2(tag.data(), tag.size(), t));
  CHECK(t.title == "MAPA");
  CHECK(t.artist == "SB19");
  CHECK(t.album == "Pagsibol");
}

TEST(id3_v24_utf8_and_utf16_japanese) {
  // "夜に駆ける" as UTF-8 (enc 3) and as UTF-16LE with BOM (enc 1)
  const char* utf8 = "\xE5\xA4\x9C\xE3\x81\xAB\xE9\xA7\x86\xE3\x81\x91\xE3\x82\x8B";
  std::vector<uint8_t> t8 = {3};
  for (const char* p = utf8; *p; p++) t8.push_back((uint8_t)*p);
  std::vector<uint8_t> t16 = {1, 0xFF, 0xFE, 0x1C, 0x59, 0x6B, 0x30, 0xC6, 0x99, 0x51, 0x30, 0x8B, 0x30, 0, 0};
  auto tag = makeTag(4, {{"TIT2", t8}, {"TPE1", t16}});
  Id3Tags t;
  CHECK(parseId3v2(tag.data(), tag.size(), t));
  CHECK(t.title == utf8);
  CHECK(t.artist == utf8);
}

TEST(id3_latin1_high_bytes_become_utf8) {
  auto tag = makeTag(3, {{"TIT2", {0, 'C', 'a', 'f', 0xE9}}});  // "Café"
  Id3Tags t;
  CHECK(parseId3v2(tag.data(), tag.size(), t));
  CHECK(t.title == "Caf\xC3\xA9");
}

TEST(id3_partial_buffer_and_missing_tag) {
  auto tag = makeTag(3, {{"TIT2", latin1("First")}, {"TPE1", latin1("Second")}});
  Id3Tags t;
  CHECK(parseId3v2(tag.data(), 10 + 10 + 6, t));  // only the first frame fits
  CHECK(t.title == "First");
  CHECK(t.artist.empty());
  const uint8_t mp3[10] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0};
  Id3Tags none;
  CHECK(!parseId3v2(mp3, sizeof mp3, none));
}

TEST(title_from_file_name_and_utf8_truncate) {
  CHECK(titleFromFileName("/SB19 'MAPA'.mp3") == "SB19 'MAPA'");
  CHECK(titleFromFileName("no_extension") == "no_extension");
  std::string jp = "\xE5\xA4\x9C\xE3\x81\xAB";  // 2 characters, 6 bytes
  CHECK(utf8Truncate(jp, 5) == "\xE5\xA4\x9C");  // never cuts inside a character
  CHECK(utf8Truncate("abc", 10) == "abc");
}

TEST(file_name_standard_parses_number_artist_title) {
  std::string t, a;
  parseFileName("/01 - SB19 - MAPA.mp3", t, a);
  CHECK(t == "MAPA");
  CHECK(a == "SB19");
  t.clear(); a.clear();
  parseFileName("SB19 - Gento.mp3", t, a);
  CHECK(t == "Gento");
  CHECK(a == "SB19");
  t.clear(); a.clear();
  parseFileName("07. Bazinga.mp3", t, a);
  CHECK(t == "Bazinga");
  CHECK(a.empty());
  t.clear(); a.clear();
  parseFileName("Kwarta_Sang_Iban_2026-10-25T084527.mp3", t, a);  // no standard: whole name
  CHECK(t == "Kwarta_Sang_Iban_2026-10-25T084527");
  CHECK(a.empty());
  t.clear(); a.clear();
  parseFileName("2026.mp3", t, a);  // only digits: keep them as the title
  CHECK(t == "2026");
}

TEST(nav_safe_text_drops_emoji_keeps_japanese) {
  CHECK(navSafeText("MAPA \xF0\x9F\x94\xA5") == "MAPA");                  // trailing fire emoji
  CHECK(navSafeText("\xE2\x9D\xA4\xEF\xB8\x8F Love") == "\xE2\x9D\xA4 Love");  // heart kept, selector dropped
  CHECK(navSafeText("Kwarta_Sang__Iban") == "Kwarta Sang Iban");
  CHECK(navSafeText("\xE5\xA4\x9C\xE3\x81\xAB") == "\xE5\xA4\x9C\xE3\x81\xAB");  // Japanese untouched
  CHECK(navSafeText("\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5").empty());
}
