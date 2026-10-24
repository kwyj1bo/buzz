#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "buzz/protocol.hpp"

namespace buzz::p2p {

// Peer datagram layout:
//   u8 sender_length | sender | AES-256-GCM(encode_body(message))
// The sender name is in clear so the receiver can pick the key, and it is
// bound to the ciphertext as associated data.
std::string seal_datagram(std::string_view sender, std::string_view key, const Message& message);
std::optional<std::string> peek_sender(std::string_view datagram);
std::optional<Message> open_datagram(std::string_view datagram, std::string_view key);

// MAC that proves a UDP hello comes from the owner of a session token,
// without sending the token in clear.
std::string hello_mac(std::string_view token, std::string_view user, std::string_view timestamp);

}  // namespace buzz::p2p
