/**
 * @file adamo_sub_test.cpp
 * @brief EXPERIMENTAL. Subscriber-side verification for AdamoObserver.
 *
 * Subscribes to a single joint topic ``<robot>/<arm>/<leaf>`` (leaf
 * ``state`` or ``effort``), decodes each payload with the
 * ``trossen_adamo::wire`` codec, and prints a one-line summary plus a periodic
 * receive-rate report. Pair it with a publisher (the stationary demo's
 * AdamoObserver) on the same machine to confirm joint data is actually
 * reaching the bus -- the Adamo dashboard renders video tracks but not raw
 * joint pubsub topics, so this is the way to "see" it.
 *
 * Usage:
 *   source .env && ./build_adamo/examples/adamo_sub_test \
 *       --robot trossen_stationary_ai --arm leader_left          # leaf=state
 *   source .env && ./adamo_sub_test --robot trossen_stationary_ai \
 *       --arm follower_left --leaf effort
 *
 * Runs until Ctrl-C.
 *
 * Build-gated behind ``TROSSEN_ENABLE_ADAMO`` in examples/CMakeLists.txt.
 */

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "adamo/adamo.hpp"
#include "trossen_adamo/args.hpp"
#include "trossen_adamo/subscriber.hpp"
#include "trossen_adamo/wire.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true); }

constexpr const char* kUsage =
  "Usage: adamo_sub_test [--robot NAME] [--arm ARM] [--leaf state|effort]\n"
  "                      [--protocol quic|udp|tcp]\n"
  "Subscribes to <robot>/<arm>/<leaf> (the AdamoObserver joint scheme) and\n"
  "decodes each frame. Reads ADAMO_API_KEY from the environment. Ctrl-C to stop.\n"
  "Example: adamo_sub_test --robot trossen_stationary_ai --arm leader_left\n";

std::string arg_value(int argc, char** argv, const char* flag, std::string fallback) {
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
  }
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  // Line-buffer stdout so progress is visible even when piped/redirected and
  // the process is killed abruptly (e.g. SIGTERM) before a normal flush.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      std::fputs(kUsage, stdout);
      return 0;
    }
  }
  const std::string robot    = arg_value(argc, argv, "--robot", "trossen_stationary_ai");
  const std::string arm      = arg_value(argc, argv, "--arm",   "leader_left");
  const std::string leaf     = arg_value(argc, argv, "--leaf",  "state");
  const std::string proto_str = arg_value(argc, argv, "--protocol", "quic");

  const bool is_state = (leaf == "state");
  if (!is_state && leaf != "effort") {
    std::fprintf(stderr, "adamo_sub_test: unknown --leaf '%s'\n%s", leaf.c_str(), kUsage);
    return 1;
  }

  const char* api_key = std::getenv("ADAMO_API_KEY");
  if (api_key == nullptr || std::strlen(api_key) == 0) {
    std::fputs("adamo_sub_test: ADAMO_API_KEY is not set. `source .env` and retry.\n", stderr);
    return 1;
  }

  adamo::Protocol proto;
  try {
    proto = trossen_adamo::args::parse_protocol(proto_str);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "adamo_sub_test: %s\n%s", e.what(), kUsage);
    return 1;
  }

  const std::string key = robot + "/" + arm + "/" + leaf;

  std::signal(SIGINT, on_sigint);

  try {
    auto session = adamo::Session::open(api_key, proto);
    trossen_adamo::LatestSubscriber sub(session, key);
    std::printf("adamo_sub_test: subscribed to '%s' (protocol=%s). Ctrl-C to stop.\n",
                key.c_str(), proto_str.c_str());

    std::vector<std::uint8_t> buf;
    std::uint64_t frames = 0;
    std::uint64_t window = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
      if (!sub.poll(buf)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      ++frames;
      ++window;
      try {
        if (is_state) {
          const auto s = trossen_adamo::wire::decode_state(buf.data(), buf.size());
          if (frames <= 3 || frames % 100 == 0) {
            std::printf("[#%" PRIu64 "] t=%.3f  p0=%.4f p1=%.4f ... p6=%.4f\n",
                        frames, s.timestamp,
                        s.positions[0], s.positions[1], s.positions[6]);
          }
        } else {
          const auto e = trossen_adamo::wire::decode_efforts(buf.data(), buf.size());
          if (frames <= 3 || frames % 100 == 0) {
            std::printf("[#%" PRIu64 "] t=%.3f  e0=%.4f e1=%.4f ... e6=%.4f\n",
                        frames, e.timestamp,
                        e.efforts[0], e.efforts[1], e.efforts[6]);
          }
        }
      } catch (const std::exception& e) {
        std::fprintf(stderr, "adamo_sub_test: decode failed: %s\n", e.what());
      }

      const auto now = std::chrono::steady_clock::now();
      const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count();
      if (secs >= 2) {
        std::printf("  rate: %.1f Hz (%" PRIu64 " total)\n",
                    static_cast<double>(window) / static_cast<double>(secs),
                    frames);
        window = 0;
        last_report = now;
      }
    }

    std::printf("adamo_sub_test: stopping. received %" PRIu64 " frames total.\n",
                frames);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "adamo_sub_test: FAILED  %s\n", e.what());
    return 1;
  }
}
