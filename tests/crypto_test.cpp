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

TEST(Crypto, Sha256MatchesKnownVector) {
  EXPECT_EQ(to_hex(sha256("abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, HkdfMatchesRfc5869) {
  // RFC 5869, test case 1.
  auto ikm = *from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
  auto salt = *from_hex("000102030405060708090a0b0c");
  auto info = *from_hex("f0f1f2f3f4f5f6f7f8f9");
  EXPECT_EQ(to_hex(hkdf_sha256(ikm, salt, info, 42)),
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
            "34007208d5b887185865");
}

TEST(Crypto, X25519AgreesBothWays) {
  X25519KeyPair a;
  X25519KeyPair b;
  EXPECT_EQ(a.public_key().size(), 32u);
  EXPECT_EQ(a.agree(b.public_key()), b.agree(a.public_key()));
  EXPECT_FALSE(a.agree("too short").has_value());
}

TEST(Crypto, WipeClears) {
  std::string secret = "password";
  wipe(secret);
  EXPECT_TRUE(secret.empty());
}

}  // namespace
}  // namespace buzz::crypto
