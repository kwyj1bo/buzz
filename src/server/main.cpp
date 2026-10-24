#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>

#include "buzz/log.hpp"
#include "buzz/server.hpp"
#include "buzz/validate.hpp"

namespace {

buzz::Server* g_server = nullptr;

void on_signal(int) {
  if (g_server) g_server->stop();
}

constexpr const char* kUsage = R"(usage: buzz-server [options]
  --port N               TCP (TLS) port, default 7000
  --udp-port N           UDP port for peer discovery, default same as --port
  --cert FILE            certificate PEM, default certs/server.crt
  --key FILE             private key PEM, default certs/server.key
  --db FILE              SQLite database for accounts and history, default buzz.db
                         (pass "" to keep everything in memory)
  --gen-cert             create a self-signed certificate if the files are missing
  --cert-host NAME       extra DNS name or IP for the generated certificate
  --threads N            password hashing workers, default 4
  --pbkdf2-iterations N  default 600000
  --message-rate N       chat messages per second per connection, default 30
  --message-burst N      burst allowance for --message-rate, default 60
  --session-ttl SEC      how long a dropped session can resume, default 300
  --log-level LEVEL      debug, info, warn or error
)";

struct Options {
  buzz::ServerConfig config;
  bool gen_cert = false;
  bool udp_port_set = false;
  std::string cert_host;
};

[[noreturn]] void usage_error(const std::string& message) {
  std::cerr << "buzz-server: " << message << "\n\n" << kUsage;
  std::exit(2);
}

uint64_t number(const std::string& flag, const std::string& value, uint64_t max) {
  auto parsed = buzz::parse_u64(value);
  if (!parsed || *parsed > max) usage_error("bad value for " + flag + ": " + value);
  return *parsed;
}

Options parse(int argc, char** argv) {
  Options o;
  auto& c = o.config;
  c.db_path = "buzz.db";
  using Setter = std::function<void(const std::string&)>;
  const std::map<std::string, Setter> with_value = {
      {"--port", [&](auto& v) { c.port = static_cast<uint16_t>(number("--port", v, 65535)); }},
      {"--udp-port",
       [&](auto& v) {
         c.udp_port = static_cast<uint16_t>(number("--udp-port", v, 65535));
         o.udp_port_set = true;
       }},
      {"--cert", [&](auto& v) { c.cert_file = v; }},
      {"--key", [&](auto& v) { c.key_file = v; }},
      {"--db", [&](auto& v) { c.db_path = v; }},
      {"--cert-host", [&](auto& v) { o.cert_host = v; }},
      {"--threads", [&](auto& v) { c.worker_threads = number("--threads", v, 256); }},
      {"--pbkdf2-iterations",
       [&](auto& v) { c.pbkdf2_iterations = static_cast<uint32_t>(number("--pbkdf2-iterations", v, 10'000'000)); }},
      {"--message-rate",
       [&](auto& v) { c.message_rate = static_cast<double>(number("--message-rate", v, 1'000'000'000)); }},
      {"--message-burst",
       [&](auto& v) { c.message_burst = static_cast<double>(number("--message-burst", v, 1'000'000'000)); }},
      {"--session-ttl",
       [&](auto& v) { c.session_ttl = std::chrono::seconds(number("--session-ttl", v, 86'400)); }},
      {"--log-level",
       [&](auto& v) {
         auto level = buzz::log::parse_level(v);
         if (!level) usage_error("bad log level " + v);
         buzz::log::set_level(*level);
       }},
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << kUsage;
      std::exit(0);
    }
    if (arg == "--gen-cert") {
      o.gen_cert = true;
      continue;
    }
    auto it = with_value.find(arg);
    if (it == with_value.end()) usage_error("unknown option " + arg);
    if (i + 1 >= argc) usage_error(arg + " needs a value");
    it->second(argv[++i]);
  }
  if (!o.udp_port_set) c.udp_port = c.port;
  return o;
}

void ensure_certificate(const Options& o) {
  namespace fs = std::filesystem;
  const auto& c = o.config;
  if (fs::exists(c.cert_file) && fs::exists(c.key_file)) return;
  for (const auto& file : {c.cert_file, c.key_file}) {
    fs::path parent = fs::path(file).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
  }
  buzz::tls::generate_self_signed(c.cert_file, c.key_file, o.cert_host);
  BUZZ_LOG(Info, "generated self-signed certificate " << c.cert_file);
}

}  // namespace

int main(int argc, char** argv) {
  Options options = parse(argc, argv);
  try {
    if (options.gen_cert) ensure_certificate(options);
    buzz::Server server(options.config);
    server.start();
    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    server.run();
    g_server = nullptr;
  } catch (const std::exception& e) {
    BUZZ_LOG(Error, e.what());
    return 1;
  }
  return 0;
}
