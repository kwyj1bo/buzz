#include <gtest/gtest.h>

#include "buzz/crypto.hpp"

namespace buzz::crypto {
namespace {

const std::string kKey(kKeySize, 'k');

TEST(Crypto, HexRoundTrip) {
  std::string bytes("\x00\x01\xab\xff", 4);
  EXPECT_EQ(to_hex(bytes), "0001abff");
  EXPECT_EQ(from_hex("0001ABff"), bytes);
}

TEST(Crypto, HexRejectsBadInput) {
  EXPECT_FALSE(from_hex("abc").has_value());
  EXPECT_FALSE(from_hex("zz").has_value());
}

TEST(Crypto, RandomBytesDiffer) {
  EXPECT_EQ(random_bytes(32).size(), 32u);
  EXPECT_NE(random_bytes(32), random_bytes(32));
}

TEST(Crypto, HmacMatchesRfc4231) {
  EXPECT_EQ(to_hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(Crypto, PasswordVerifies) {
  PasswordHash hash = hash_password("correct horse", 1000);
  EXPECT_TRUE(verify_password("correct horse", hash));
  EXPECT_FALSE(verify_password("wrong horse", hash));
}

TEST(Crypto, PasswordHashesAreSalted) {
  EXPECT_NE(hash_password("same", 1000).hash, hash_password("same", 1000).hash);
}

TEST(Crypto, SealOpenRoundTrip) {
  std::string sealed = seal(kKey, "secret message", "aad");
  EXPECT_EQ(sealed.size(), kNonceSize + 14 + kTagSize);
  EXPECT_EQ(open(kKey, sealed, "aad"), "secret message");
}

TEST(Crypto, SealHandlesEmptyPlaintext) { EXPECT_EQ(open(kKey, seal(kKey, "", ""), ""), ""); }

TEST(Crypto, OpenDetectsTampering) {
  std::string sealed = seal(kKey, "secret message", "aad");
  sealed[kNonceSize] ^= 1;
  EXPECT_FALSE(open(kKey, sealed, "aad").has_value());
}

TEST(Crypto, OpenRejectsWrongKeyOrAad) {
  std::string sealed = seal(kKey, "secret", "aad");
  EXPECT_FALSE(open(std::string(kKeySize, 'x'), sealed, "aad").has_value());
  EXPECT_FALSE(open(kKey, sealed, "other").has_value());
  EXPECT_FALSE(open(kKey, "short", "aad").has_value());
}

TEST(Crypto, WipeClears) {
  std::string secret = "password";
  wipe(secret);
  EXPECT_TRUE(secret.empty());
}

}  // namespace
}  // namespace buzz::crypto
