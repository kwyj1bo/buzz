#include "buzz/tls.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <stdexcept>

namespace buzz::tls {
namespace {

using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CertPtr = std::unique_ptr<X509, decltype(&X509_free)>;
struct FileCloser {
  void operator()(FILE* file) const { std::fclose(file); }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error(what + ": " + last_error());
}

ContextPtr new_context(const SSL_METHOD* method) {
  ContextPtr ctx(SSL_CTX_new(method));
  if (!ctx) fail("SSL_CTX_new");
  SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
  // Non-blocking writes may be partial and retried from a moved buffer.
  SSL_CTX_set_mode(ctx.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  return ctx;
}

bool is_ip_literal(const std::string& host) {
  in_addr v4{};
  return inet_pton(AF_INET, host.c_str(), &v4) == 1;
}

void add_extension(X509* cert, int nid, const std::string& value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
  if (!ext) fail("certificate extension " + value);
  X509_add_ext(cert, ext, -1);
  X509_EXTENSION_free(ext);
}

std::string subject_alt_names(const std::string& extra_host) {
  std::string names = "DNS:localhost,IP:127.0.0.1";
  if (extra_host.empty() || extra_host == "localhost" || extra_host == "127.0.0.1") return names;
  return names + (is_ip_literal(extra_host) ? ",IP:" : ",DNS:") + extra_host;
}

CertPtr build_certificate(EVP_PKEY* key, const std::string& extra_host, int days) {
  CertPtr cert(X509_new(), X509_free);
  if (!cert) fail("X509_new");
  X509_set_version(cert.get(), 2);
  long serial = 0;
  RAND_bytes(reinterpret_cast<unsigned char*>(&serial), sizeof serial);
  ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial & 0x7fffffff);
  X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(days) * 24 * 3600);
  X509_set_pubkey(cert.get(), key);

  X509_NAME* name = X509_get_subject_name(cert.get());
  const std::string common_name = extra_host.empty() ? "localhost" : extra_host;
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("Buzz"), -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1,
                             0);
  X509_set_issuer_name(cert.get(), name);

  add_extension(cert.get(), NID_basic_constraints, "critical,CA:TRUE");
  add_extension(cert.get(), NID_key_usage, "critical,digitalSignature,keyCertSign");
  add_extension(cert.get(), NID_subject_key_identifier, "hash");
  add_extension(cert.get(), NID_subject_alt_name, subject_alt_names(extra_host));
  if (X509_sign(cert.get(), key, EVP_sha256()) == 0) fail("X509_sign");
  return cert;
}

FilePtr open_for_write(const std::string& path, mode_t mode) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) throw std::runtime_error("cannot write " + path);
  return FilePtr(::fdopen(fd, "w"));
}

}  // namespace

std::string last_error() {
  std::string out;
  unsigned long code = 0;
  while ((code = ERR_get_error()) != 0) {
    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof buffer);
    if (!out.empty()) out += "; ";
    out += buffer;
  }
  return out.empty() ? "unknown TLS error" : out;
}

ContextPtr make_server_context(const std::string& cert_file, const std::string& key_file) {
  ContextPtr ctx = new_context(TLS_server_method());
  if (SSL_CTX_use_certificate_chain_file(ctx.get(), cert_file.c_str()) != 1) {
    fail("load certificate " + cert_file);
  }
  if (SSL_CTX_use_PrivateKey_file(ctx.get(), key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
    fail("load private key " + key_file);
  }
  if (SSL_CTX_check_private_key(ctx.get()) != 1) fail("certificate and key do not match");
  return ctx;
}

ContextPtr make_client_context(const std::string& ca_file) {
  ContextPtr ctx = new_context(TLS_client_method());
  if (ca_file.empty()) {
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    return ctx;
  }
  if (SSL_CTX_load_verify_locations(ctx.get(), ca_file.c_str(), nullptr) != 1) {
    fail("load CA file " + ca_file);
  }
  SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
  return ctx;
}

SslPtr make_client_ssl(SSL_CTX* ctx, int fd, const std::string& host) {
  SslPtr ssl(SSL_new(ctx));
  if (!ssl || SSL_set_fd(ssl.get(), fd) != 1) fail("SSL_new");
  if (is_ip_literal(host)) {
    X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl.get()), host.c_str());
  } else {
    SSL_set_tlsext_host_name(ssl.get(), host.c_str());
    SSL_set1_host(ssl.get(), host.c_str());
  }
  return ssl;
}

void generate_self_signed(const std::string& cert_file, const std::string& key_file,
                          const std::string& extra_host, int days) {
  KeyPtr key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
  if (!key) fail("generate key");
  CertPtr cert = build_certificate(key.get(), extra_host, days);

  FilePtr key_out = open_for_write(key_file, 0600);
  if (PEM_write_PrivateKey(key_out.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    fail("write " + key_file);
  }
  FilePtr cert_out = open_for_write(cert_file, 0644);
  if (PEM_write_X509(cert_out.get(), cert.get()) != 1) fail("write " + cert_file);
}

}  // namespace buzz::tls
