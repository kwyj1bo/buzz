// buzz-bench: load generator for a running buzz-server.
#include <iostream>
#include <map>
#include <string>

#include "buzz/crypto.hpp"
#include "buzz/log.hpp"
#include "buzz/net.hpp"
#include "buzz/tls.hpp"
#include "buzz/validate.hpp"
#include "scenarios.hpp"

namespace {

constexpr const char* kUsage = R"(usage: buzz-bench <scenario> [options]
scenarios:
  handshakes    open --clients TLS connections, report rate and latency
  connections   hold --clients logged-in connections, report server memory
                and ping round trip while they stay open
  logins        register and log in --clients users (real PBKDF2 cost)
  chat          --rooms x --members clients, --senders per room post for
                --duration seconds; report throughput and latency
  sync          post --messages to a room, time a full replay on join
  p2p           encrypted UDP ping-pong between two local peers
options:
  --port N --ca FILE --clients N --threads N --rooms N --members N
  --senders N --window N --rate R --duration S --payload BYTES
  --messages N --server-pid PID
)";

[[noreturn]] void usage_error(const std::string& message) {
  std::cerr << "buzz-bench: " << message << "\n\n" << kUsage;
  std::exit(2);
}

buzz::bench::Options parse(int argc, char** argv, std::string& scenario) {
  if (argc < 2) usage_error("missing scenario");
  scenario = argv[1];
  buzz::bench::Options o;
  std::map<std::string, int*> ints = {
      {"--clients", &o.clients},   {"--threads", &o.threads}, {"--rooms", &o.rooms},
      {"--members", &o.members},   {"--senders", &o.senders}, {"--window", &o.window},
      {"--payload", &o.payload},   {"--messages", &o.messages},
      {"--server-pid", &o.server_pid}};
  for (int i = 2; i < argc; i += 2) {
    std::string flag = argv[i];
    if (i + 1 >= argc) usage_error(flag + " needs a value");
    std::string value = argv[i + 1];
    if (auto it = ints.find(flag); it != ints.end()) {
      auto number = buzz::parse_i64(value);
      if (!number || *number < 0) usage_error("bad value for " + flag);
      *it->second = static_cast<int>(*number);
    } else if (flag == "--port") {
      o.port = static_cast<uint16_t>(std::stoi(value));
    } else if (flag == "--ca") {
      o.ca_file = value;
    } else if (flag == "--rate") {
      o.rate = std::stod(value);
    } else if (flag == "--duration") {
      o.duration = std::stod(value);
    } else {
      usage_error("unknown option " + flag);
    }
  }
  if (o.threads < 1) o.threads = 1;
  o.prefix = "b" + buzz::crypto::to_hex(buzz::crypto::random_bytes(3)) + "-";
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenario;
  auto options = parse(argc, argv, scenario);
  buzz::net::ignore_sigpipe();
  buzz::net::raise_fd_limit();
  buzz::log::set_level(buzz::log::Level::Warn);
  try {
    if (scenario == "p2p") {
      buzz::bench::run_p2p(options);
      return 0;
    }
    auto ctx = buzz::tls::make_client_context(options.ca_file);
    using Runner = void (*)(const buzz::bench::Options&, SSL_CTX*);
    const std::map<std::string, Runner> runners = {
        {"handshakes", buzz::bench::run_handshakes}, {"connections", buzz::bench::run_connections},
        {"logins", buzz::bench::run_logins},         {"chat", buzz::bench::run_chat},
        {"sync", buzz::bench::run_sync}};
    auto it = runners.find(scenario);
    if (it == runners.end()) usage_error("unknown scenario " + scenario);
    it->second(options, ctx.get());
  } catch (const std::exception& e) {
    std::cerr << "buzz-bench: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
