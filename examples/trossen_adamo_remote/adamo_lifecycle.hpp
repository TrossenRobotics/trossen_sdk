/**
 * @file adamo_lifecycle.hpp
 * @brief Tiny Adamo control channel to coordinate the remote-teleop leader and
 *        follower lifecycles. EXPERIMENTAL. Header-only; example-local.
 *
 * Each process publishes its run-state on its own topic and tracks the peer's
 * latest state from the peer topic:
 *   - leader  publishes <robot>/leader/lifecycle,   subscribes <robot>/follower/lifecycle
 *   - follower publishes <robot>/follower/lifecycle, subscribes <robot>/leader/lifecycle
 *
 * Payload is a single byte (LifeState). READY is meant to be republished as a
 * heartbeat so a late-joining peer still catches it; STOPPING is sent once at
 * graceful shutdown (with a brief flush before the session closes). This lets
 * the follower wait until the leader is online, and lets either side terminate
 * when the other ends its session.
 */

#ifndef TROSSEN_ADAMO_REMOTE_ADAMO_LIFECYCLE_HPP_
#define TROSSEN_ADAMO_REMOTE_ADAMO_LIFECYCLE_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "adamo/adamo.hpp"
#include "trossen_adamo/subscriber.hpp"

namespace trossen_adamo_remote {

/// Run-state exchanged over the lifecycle topic (1-byte payload).
enum class LifeState : std::uint8_t {
  kUnknown  = 0,
  kReady    = 1,  ///< process is up and running (republished as a heartbeat)
  kStopping = 2,  ///< process is shutting down (sent once at teardown)
};

inline std::string leader_lifecycle_topic(const std::string& robot) {
  return robot + "/leader/lifecycle";
}
inline std::string follower_lifecycle_topic(const std::string& robot) {
  return robot + "/follower/lifecycle";
}

/**
 * @brief Publishes this process's lifecycle state and tracks the peer's.
 *
 * Not thread-safe: drive it from a single thread at a time. (The follower hands
 * it from the main thread, used only for the startup wait, to a single heartbeat
 * thread; the direct leader uses it from its one publish-loop thread.)
 */
class LifecycleLink {
public:
  LifecycleLink(adamo::Session& session,
                const std::string& own_topic,
                const std::string& peer_topic)
    : pub_(session.publisher(own_topic)),
      sub_(session, peer_topic) {}

  /// Publish this process's current state.
  void announce(LifeState state) {
    const std::uint8_t byte = static_cast<std::uint8_t>(state);
    pub_.put(&byte, 1);
  }

  /// Drain the latest peer sample (if a fresh one arrived) and return the most
  /// recent peer state seen so far.
  LifeState poll_peer() {
    if (sub_.poll(buf_) && !buf_.empty()) {
      last_peer_ = static_cast<LifeState>(buf_[0]);
    }
    return last_peer_;
  }

  /// Last peer state observed, without polling.
  LifeState peer() const { return last_peer_; }

private:
  adamo::Publisher pub_;
  trossen_adamo::LatestSubscriber sub_;
  std::vector<std::uint8_t> buf_;
  LifeState last_peer_{LifeState::kUnknown};
};

}  // namespace trossen_adamo_remote

#endif  // TROSSEN_ADAMO_REMOTE_ADAMO_LIFECYCLE_HPP_
