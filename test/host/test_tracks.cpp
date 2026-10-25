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
