#include <gtest/gtest.h>

#include "buzz/validate.hpp"

namespace buzz {
namespace {

TEST(Validate, Usernames) {
  EXPECT_TRUE(valid_username("alice"));
  EXPECT_TRUE(valid_username("a_b-9"));
  EXPECT_FALSE(valid_username("ab"));
  EXPECT_FALSE(valid_username(std::string(33, 'a')));
  EXPECT_FALSE(valid_username("al ice"));
  EXPECT_FALSE(valid_username("alice!"));
}

TEST(Validate, Passwords) {
  EXPECT_TRUE(valid_password("12345678"));
  EXPECT_FALSE(valid_password("1234567"));
  EXPECT_FALSE(valid_password(std::string(129, 'a')));
}

TEST(Validate, Rooms) {
  EXPECT_TRUE(valid_room("g"));
  EXPECT_TRUE(valid_room("general-2"));
  EXPECT_FALSE(valid_room(""));
  EXPECT_FALSE(valid_room("#general"));
}

TEST(Validate, TextRejectsControlCharacters) {
  EXPECT_TRUE(valid_text("hello, world"));
  EXPECT_TRUE(valid_text("caf\xc3\xa9"));
  EXPECT_FALSE(valid_text(""));
  EXPECT_FALSE(valid_text("\x1b[2Jcleared"));
  EXPECT_FALSE(valid_text("line\nbreak"));
  EXPECT_FALSE(valid_text(std::string(kMaxTextSize + 1, 'a')));
}

TEST(Validate, ParseNumbers) {
  EXPECT_EQ(parse_u64("42"), 42u);
  EXPECT_FALSE(parse_u64("").has_value());
  EXPECT_FALSE(parse_u64("-1").has_value());
  EXPECT_FALSE(parse_u64("12x").has_value());
  EXPECT_EQ(parse_i64("-5"), -5);
}

}  // namespace
}  // namespace buzz
