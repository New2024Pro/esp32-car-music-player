#include "Params.h"
#include "mini_test.h"

TEST(parseLong_accepts_plain_integers) {
  long v = -1;
  CHECK(parseLong("0", v) && v == 0);
  CHECK(parseLong("42", v) && v == 42);
  CHECK(parseLong("-7", v) && v == -7);
  CHECK(parseLong("+9", v) && v == 9);
}

TEST(parseLong_rejects_garbage) {
  long v = 123;
  CHECK(!parseLong("", v));
  CHECK(!parseLong("abc", v));
  CHECK(!parseLong("12abc", v));
  CHECK(!parseLong(" 5", v));
  CHECK(!parseLong("5 ", v));
  CHECK(!parseLong("-", v));
  CHECK(!parseLong("1.5", v));
  CHECK(!parseLong("99999999999999999999", v));  // overflow
  CHECK_EQ(v, 123);                              // untouched on failure
}

TEST(clampVolume_bounds) {
  CHECK_EQ(clampVolume(-5), 0);
  CHECK_EQ(clampVolume(0), 0);
  CHECK_EQ(clampVolume(55), 55);
  CHECK_EQ(clampVolume(100), 100);
  CHECK_EQ(clampVolume(101), 100);
  CHECK_EQ(clampVolume(1000000000L), 100);
}

TEST(jsonEscape_valid_json_strings) {
  CHECK_EQ(jsonEscape("a\"b"), std::string("a\\\"b"));
  CHECK_EQ(jsonEscape("a\\b"), std::string("a\\\\b"));
  CHECK_EQ(jsonEscape("a\nb\tc"), std::string("a\\nb\\tc"));
  CHECK_EQ(jsonEscape(std::string("a\x01" "b", 3)), std::string("a\\u0001b"));
  CHECK_EQ(jsonEscape("\xC3\xA9"), std::string("\xC3\xA9"));  // UTF-8 passes through
}
