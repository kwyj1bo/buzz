#include <iostream>
#include <string>

#include "buzz/log.hpp"
#include "buzz/validate.hpp"
#include "client.hpp"

namespace {

constexpr const char* kUsage = R"(usage: buzz [options]
  --host HOST     server host, default 127.0.0.1
  --port N        server port, default 7000
  --ca FILE       trust this certificate (for example the server's self-signed one)
  --insecure      skip certificate verification (testing only)
)";

[[noreturn]] void usage_error(const std::string& message) {
  std::cerr << "buzz: " << message << "\n\n" << kUsage;
  std::exit(2);
}

buzz::client::ClientConfig parse(int argc, char** argv) {
  buzz::client::ClientConfig config;
  bool insecure = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << kUsage;
      std::exit(0);
    }
    if (arg == "--insecure") {
      insecure = true;
      continue;
    }
    if (i + 1 >= argc) usage_error("unknown option or missing value: " + arg);
    std::string value = argv[++i];
    if (arg == "--host") {
      config.host = value;
    } else if (arg == "--port") {
      auto port = buzz::parse_u64(value);
      if (!port || *port == 0 || *port > 65535) usage_error("bad port " + value);
      config.port = static_cast<uint16_t>(*port);
    } else if (arg == "--ca") {
      config.ca_file = value;
    } else {
      usage_error("unknown option " + arg);
    }
  }
  if (config.ca_file.empty() && !insecure) {
    usage_error("pass --ca <server certificate> or --insecure");
  }
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  auto config = parse(argc, argv);
  buzz::log::set_level(buzz::log::Level::Warn);
  try {
    buzz::client::Client client(config);
    client.run(std::cin);
  } catch (const std::exception& e) {
    std::cerr << "buzz: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
