/**
 * @file adamo_smoke_test.cpp
 * @brief EXPERIMENTAL. Smallest possible Adamo SDK smoke test.
 *
 * Verifies that:
 *   - ``ADAMO_API_KEY`` is set in the environment.
 *   - ``adamo::Session::open`` succeeds against the configured Adamo backend
 *     using the requested protocol (default ``quic``).
 *   - ``libadamo.so`` is reachable at runtime via the build-RPATH baked in
 *     by the trossen_sdk CMake gate.
 *
 * On success prints the resolved org name and exits 0.
 * On failure prints the SDK error and exits 1.
 *
 * Usage:
 *   source .env && ./build_adamo/examples/trossen_adamo_remote/adamo_smoke_test
 *   source .env && ./adamo_smoke_test --protocol udp
 *
 * Build-gated behind ``TROSSEN_ENABLE_ADAMO`` in examples/CMakeLists.txt.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "adamo/adamo.hpp"
#include "trossen_adamo/args.hpp"

namespace {

constexpr const char* kUsage =
  "Usage: adamo_smoke_test [--protocol quic|udp|tcp]\n"
  "Reads ADAMO_API_KEY from the environment.\n";

std::string parse_protocol_arg(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--protocol") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
    if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      std::fputs(kUsage, stdout);
      std::exit(0);
    }
  }
  return "quic";
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const std::string protocol_str = parse_protocol_arg(argc, argv);

  const char* api_key = std::getenv("ADAMO_API_KEY");
  if (api_key == nullptr || std::strlen(api_key) == 0) {
    std::fputs("smoke_test: ADAMO_API_KEY is not set. `source .env` and retry.\n", stderr);
    return 1;
  }

  adamo::Protocol proto;
  try {
    proto = trossen_adamo::args::parse_protocol(protocol_str);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "smoke_test: %s\n%s", e.what(), kUsage);
    return 1;
  }

  std::printf("smoke_test: opening adamo::Session (protocol=%s)\n",
              protocol_str.c_str());

  try {
    auto session = adamo::Session::open(api_key, proto);
    const auto org = session.org();
    std::printf("smoke_test: OK  org='%.*s'\n",
                static_cast<int>(org.size()), org.data());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "smoke_test: FAILED  %s\n", e.what());
    return 1;
  }
}
