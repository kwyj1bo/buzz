#include "buzz/p2p.hpp"

#include <utility>

#include "buzz/crypto.hpp"

namespace buzz::p2p {
namespace {

constexpr std::string_view kAadPrefix = "buzz-p2p-v1|";
constexpr size_t kMaxSender = 255;

std::string aad_for(std::string_view sender) {
  std::string aad(kAadPrefix);
  aad.append(sender);
  return aad;
}

// Both sides must build the same bytes, so the pair is ordered by name.
std::string pair_transcript(std::string_view name_a, std::string_view public_a,
                            std::string_view name_b, std::string_view public_b) {
  if (name_b < name_a) {
    std::swap(name_a, name_b);
    std::swap(public_a, public_b);
  }
  std::string out;
  for (auto part : {name_a, public_a, name_b, public_b}) {
    out += std::to_string(part.size());
    out += ':';
    out.append(part);
  }
  return out;
}

}  // namespace

std::string seal_datagram(std::string_view sender, std::string_view key, const Message& message) {
  if (sender.empty() || sender.size() > kMaxSender) throw ProtocolError("bad sender name");
  std::string out;
  out.push_back(static_cast<char>(sender.size()));
  out.append(sender);
  out += crypto::seal(key, encode_body(message), aad_for(sender));
  return out;
}

std::optional<std::string> peek_sender(std::string_view datagram) {
  if (datagram.empty()) return std::nullopt;
  auto length = static_cast<uint8_t>(datagram[0]);
  if (length == 0 || datagram.size() < 1u + length) return std::nullopt;
  return std::string(datagram.substr(1, length));
}

std::optional<Message> open_datagram(std::string_view datagram, std::string_view key) {
  auto sender = peek_sender(datagram);
  if (!sender) return std::nullopt;
  auto plaintext = crypto::open(key, datagram.substr(1 + sender->size()), aad_for(*sender));
  if (!plaintext) return std::nullopt;
  try {
    return decode_body(*plaintext);
  } catch (const ProtocolError&) {
    return std::nullopt;
  }
}

std::optional<std::string> derive_session_key(const crypto::X25519KeyPair& mine,
                                              std::string_view my_name,
                                              std::string_view peer_name,
                                              std::string_view peer_public) {
  auto secret = mine.agree(peer_public);
  if (!secret) return std::nullopt;
  std::string key = crypto::hkdf_sha256(*secret, kAadPrefix,
                                        pair_transcript(my_name, mine.public_key(), peer_name,
                                                        peer_public),
                                        crypto::kKeySize);
  crypto::wipe(*secret);
  return key;
}

std::string safety_number(std::string_view name_a, std::string_view public_a,
                          std::string_view name_b, std::string_view public_b) {
  std::string digest =
      crypto::to_hex(crypto::sha256(pair_transcript(name_a, public_a, name_b, public_b)));
  std::string out;
  for (size_t i = 0; i < 30; i += 5) {
    if (!out.empty()) out += ' ';
    out += digest.substr(i, 5);
  }
  return out;
}

std::string hello_mac(std::string_view key, std::string_view user, std::string_view nonce,
                      std::string_view counter) {
  std::string data = "udp-hello|";
  data.append(user).append("|").append(nonce).append("|").append(counter);
  return crypto::to_hex(crypto::hmac_sha256(key, data));
}

}  // namespace buzz::p2p
