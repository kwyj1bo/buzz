#include "buzz/p2p.hpp"

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

std::string hello_mac(std::string_view token, std::string_view user, std::string_view timestamp) {
  std::string data = "udp-hello|";
  data.append(user).append("|").append(timestamp);
  return crypto::to_hex(crypto::hmac_sha256(token, data));
}

}  // namespace buzz::p2p
