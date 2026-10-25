#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "buzz/crypto.hpp"
#include "buzz/protocol.hpp"

namespace buzz::p2p {

// Peer datagram layout:
//   u8 sender_length | sender | AES-256-GCM(encode_body(message))
// The sender name is in clear so the receiver can pick the key, and it is
// bound to the ciphertext as associated data.
std::string seal_datagram(std::string_view sender, std::string_view key, const Message& message);
std::optional<std::string> peek_sender(std::string_view datagram);
std::optional<Message> open_datagram(std::string_view datagram, std::string_view key);

// Session key for a pair of peers: HKDF over their X25519 shared secret,
// bound to both names and public keys. The server only forwards the public
// keys, so it cannot derive this key. Nullopt for an invalid peer key.
std::optional<std::string> derive_session_key(const crypto::X25519KeyPair& mine,
                                              std::string_view my_name,
                                              std::string_view peer_name,
                                              std::string_view peer_public);

// Short code both users can compare out of band. It matches only when
// nobody, including the server, swapped the public keys.
std::string safety_number(std::string_view name_a, std::string_view public_a,
                          std::string_view name_b, std::string_view public_b);

// MAC that proves a UDP hello comes from the owner of a session. The key is
// SHA-256 of the session token; `nonce` comes from the server's Welcome and
// `counter` increases with each hello, so captured hellos cannot be replayed.
std::string hello_mac(std::string_view key, std::string_view user, std::string_view nonce,
                      std::string_view counter);

}  // namespace buzz::p2p
