#include "TrackList.h"
#include "mini_test.h"

using V = std::vector<std::string>;

TEST(tracks_filter_and_sort) {
  V in = {"B.mp3", "a.MP3", "._x.mp3", ".hid.mp3", "c.txt", "readme", "d.mp3x"};
  V out = buildTrackList(in);
  CHECK_EQ(out.size(), 2u);
  CHECK_EQ(out[0], std::string("a.MP3"));
  CHECK_EQ(out[1], std::string("B.mp3"));
}

TEST(tracks_tie_bytewise_deterministic) {
  V in = {"a.mp3", "A.mp3", "A.MP3"};
  V out = buildTrackList(in);
  V rev(in.rbegin(), in.rend());
  CHECK(out == buildTrackList(rev));  // input order irrelevant
  CHECK_EQ(out[0], std::string("A.MP3"));  // ci-equal: byte order, 'A' < 'a', 'M' < 'm'
  CHECK_EQ(out[1], std::string("A.mp3"));
  CHECK_EQ(out[2], std::string("a.mp3"));
}

TEST(tracks_non_ascii_names_sorted_bytewise) {
  V in = {"\xC3\xA9.mp3", "z.mp3", "\xE3\x81\x82.mp3"};
  V out = buildTrackList(in);
  CHECK_EQ(out[0], std::string("z.mp3"));
  CHECK_EQ(out[1], std::string("\xC3\xA9.mp3"));
}

TEST(tracks_empty_list) {
  CHECK(buildTrackList({}).empty());
  Page p = pageTracks({}, 0, 50);
  CHECK_EQ(p.total, 0u);
  CHECK(p.items.empty());
}

TEST(page_clamps_limit_and_offset) {
  V t;
  for (int i = 0; i < 120; i++) t.push_back("t" + std::to_string(i));
  CHECK_EQ(pageTracks(t, 0, 0).items.size(), 1u);
  CHECK_EQ(pageTracks(t, 0, 999).items.size(), 50u);
  CHECK_EQ(pageTracks(t, 100, 50).items.size(), 20u);
  CHECK(pageTracks(t, 120, 50).items.empty());
  CHECK(pageTracks(t, 9999, 50).items.empty());
  Page neg = pageTracks(t, -5, 3);
  CHECK_EQ(neg.offset, 0u);
  CHECK_EQ(neg.items.size(), 3u);
  CHECK_EQ(pageTracks(t, 2, 2).items[0].first, 2u);
}

TEST(next_prev_no_wrap) {
  CHECK_EQ(nextIndex(0, 3), 1);
  CHECK_EQ(nextIndex(2, 3), -1);  // end of list: stop
  CHECK_EQ(prevIndex(0, 3), 0);   // stay put
  CHECK_EQ(prevIndex(2, 3), 1);
  CHECK_EQ(nextIndex(0, 0), -1);
  CHECK_EQ(prevIndex(0, 0), -1);
}

TEST(skip_all_unsupported_returns_minus1_bounded) {
  int calls = 0;
  int r = skipPolicy(0, +1, 3, [&](int) { calls++; return false; });
  CHECK_EQ(r, -1);
  CHECK_EQ(calls, 3);
}

TEST(skip_finds_next_playable_forward_and_back) {
  auto ok2 = [](int i) { return i == 2; };
  CHECK_EQ(skipPolicy(0, +1, 5, ok2), 2);
  CHECK_EQ(skipPolicy(4, -1, 5, ok2), 2);
  CHECK_EQ(skipPolicy(3, +1, 5, ok2), -1);  // no wrap
  CHECK_EQ(skipPolicy(5, +1, 5, ok2), -1);  // out of range start
  CHECK_EQ(skipPolicy(-1, +1, 5, ok2), -1);
}

TEST(names_match_apostrophe_variants) {
  CHECK(namesMatch("Saunder`s AirPods Pro", "Saunder's AirPods Pro"));
  CHECK(namesMatch("Saunder`s AirPods Pro", "Saunder\xE2\x80\x99s AirPods Pro"));
  CHECK(!namesMatch("Saunder`s AirPods Pro", "Saunder`s AirPods Pro 2"));
  CHECK(!namesMatch("Saunder`s AirPods Pro", "Other AirPods Pro"));
  CHECK(!namesMatch("", "x"));
  CHECK(namesMatch("Saunder`s AirPods Pro", "saunder's airpods pro") == false);  // case-sensitive
}


static uint32_t lcg(uint32_t bound) {  // deterministic stand-in for esp_random()
  static uint32_t x = 12345;
  x = x * 1103515245u + 12345u;
  return (x >> 8) % bound;
}

static bool isPermutation(std::vector<int> o, size_t n) {
  if (o.size() != n) return false;
  std::sort(o.begin(), o.end());
  for (size_t i = 0; i < n; i++)
    if (o[i] != (int)i) return false;
  return true;
}

TEST(makeOrder_identity_without_shuffle) {
  std::vector<int> o = makeOrder(4, false, 2, -1, lcg);
  CHECK(o == std::vector<int>({0, 1, 2, 3}));
  CHECK(makeOrder(0, true, -1, -1, lcg).empty());
}

TEST(makeOrder_shuffle_is_permutation_with_first_in_front) {
  for (int round = 0; round < 50; round++) {
    std::vector<int> o = makeOrder(7, true, 3, -1, lcg);
    CHECK(isPermutation(o, 7));
    CHECK_EQ(o[0], 3);
  }
}

TEST(makeOrder_reshuffle_avoids_back_to_back_repeat) {
  for (int round = 0; round < 200; round++) {
    std::vector<int> o = makeOrder(3, true, -1, 2, lcg);
    CHECK(isPermutation(o, 3));
    CHECK(o[0] != 2);
  }
}

TEST(makeOrder_shuffle_actually_varies) {
  bool differs = false;
  std::vector<int> first = makeOrder(8, true, -1, -1, lcg);
  for (int round = 0; round < 10 && !differs; round++) differs = makeOrder(8, true, -1, -1, lcg) != first;
  CHECK(differs);
}

TEST(orderPosOf_finds_track) {
  std::vector<int> o = {2, 0, 1};
  CHECK_EQ(orderPosOf(o, 1), 2);
  CHECK_EQ(orderPosOf(o, 5), -1);
}

TEST(nextPos_repeat_modes) {
  CHECK_EQ(nextPos(0, 3, Repeat::Off, true), 1);
  CHECK_EQ(nextPos(2, 3, Repeat::Off, true), -1);  // end: stop
  CHECK_EQ(nextPos(2, 3, Repeat::All, true), 0);   // end: wrap
  CHECK_EQ(nextPos(1, 3, Repeat::One, true), 1);   // track ended: replay
  CHECK_EQ(nextPos(1, 3, Repeat::One, false), 2);  // manual Next still moves on
  CHECK_EQ(nextPos(2, 3, Repeat::One, false), 0);
  CHECK_EQ(nextPos(-1, 3, Repeat::Off, false), 0);  // nothing played yet
  CHECK_EQ(nextPos(0, 0, Repeat::All, false), -1);
}

TEST(prevPos_repeat_modes) {
  CHECK_EQ(prevPos(2, 3, Repeat::Off), 1);
  CHECK_EQ(prevPos(0, 3, Repeat::Off), 0);  // stay put
  CHECK_EQ(prevPos(0, 3, Repeat::All), 2);  // wrap to last
  CHECK_EQ(prevPos(0, 3, Repeat::One), 0);
  CHECK_EQ(prevPos(0, 0, Repeat::All), -1);
}
