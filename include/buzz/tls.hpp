#pragma once

#include <openssl/ssl.h>

#include <memory>
#include <string>

namespace buzz::tls {

struct ContextDeleter {
  void operator()(SSL_CTX* ctx) const { SSL_CTX_free(ctx); }
};
struct SslDeleter {
  void operator()(SSL* ssl) const { SSL_free(ssl); }
};
using ContextPtr = std::unique_ptr<SSL_CTX, ContextDeleter>;
using SslPtr = std::unique_ptr<SSL, SslDeleter>;

// Drains the OpenSSL error queue into one readable string.
std::string last_error();

ContextPtr make_server_context(const std::string& cert_file, const std::string& key_file);
// An empty ca_file disables certificate verification.
ContextPtr make_client_context(const std::string& ca_file);
// Creates a client SSL bound to fd that checks the certificate matches host.
SslPtr make_client_ssl(SSL_CTX* ctx, int fd, const std::string& host);

// Writes a self-signed EC P-256 certificate valid for localhost, 127.0.0.1
// and extra_host (when not empty). The key file gets mode 0600.
void generate_self_signed(const std::string& cert_file, const std::string& key_file,
                          const std::string& extra_host = "", int days = 365);

}  // namespace buzz::tls
