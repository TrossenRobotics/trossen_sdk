/**
 * @file vr_mdns_advertiser.hpp
 * @brief Native mDNS advertiser so the Meta Quest app can discover the host.
 *
 * The Quest VR app scans the local network for services of type
 * `_trossen-vr._tcp.local.` carrying the TXT record
 * `app=trossen_vr_teleop`. The C++ `trossen_vr` WebSocket server opens
 * its port but does not advertise itself; until that upstream gap is
 * closed, this class is the C++ replacement for
 * `examples/trossen_vr_stationary/mdns_helper.py`.
 *
 * Uses Avahi (libavahi-client) under the hood — the canonical Linux
 * mDNS responder. Requires `avahi-daemon` to be running on the host
 * (default on Ubuntu / Jetson images). Avahi headers are hidden
 * behind a pimpl so the SDK's public surface stays dependency-clean.
 */

#ifndef TROSSEN_SDK__HW__VR__VR_MDNS_ADVERTISER_HPP_
#define TROSSEN_SDK__HW__VR__VR_MDNS_ADVERTISER_HPP_

#include <cstdint>
#include <memory>
#include <string>

namespace trossen::hw::vr {

/**
 * @brief Registers a `_trossen-vr._tcp` mDNS service for the Quest.
 *
 * Typical usage — construct once, start with the WebSocket port, stop
 * on shutdown:
 *
 * @code
 *   VrMdnsAdvertiser adv;
 *   adv.start(5432);                          // advertises "TrossenVR"
 *   // ... run demo ...
 *   adv.stop();                               // also happens in dtor
 * @endcode
 *
 * Thread-safety: `start()`, `stop()`, and `is_running()` are serialized
 * internally. Safe to construct and destroy from the main thread only.
 */
class VrMdnsAdvertiser {
public:
  VrMdnsAdvertiser();
  ~VrMdnsAdvertiser();

  VrMdnsAdvertiser(const VrMdnsAdvertiser&)            = delete;
  VrMdnsAdvertiser& operator=(const VrMdnsAdvertiser&) = delete;
  VrMdnsAdvertiser(VrMdnsAdvertiser&&)                 = delete;
  VrMdnsAdvertiser& operator=(VrMdnsAdvertiser&&)      = delete;

  /**
   * @brief Begin advertising the service. Idempotent.
   *
   * Registers an Avahi entry group with service type
   * `_trossen-vr._tcp`, the given port, and the `app=trossen_vr_teleop`
   * TXT record so the Quest app recognizes the host.
   *
   * @param port           WebSocket port the Quest should connect to.
   * @param instance_name  Name shown in the Quest server picker
   *                       (default "TrossenVR"). If another Avahi
   *                       responder on the LAN already owns the name,
   *                       Avahi appends a numeric suffix — no collision
   *                       behavior the caller needs to handle.
   *
   * @throws std::runtime_error if the Avahi daemon is not reachable or
   *         the service cannot be registered.
   */
  void start(std::uint16_t port,
             const std::string& instance_name = "TrossenVR");

  /**
   * @brief Deregister the service and release the Avahi client.
   *
   * Idempotent. Automatically invoked by the destructor.
   */
  void stop();

  /// @brief True if the service is currently advertised.
  bool is_running() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace trossen::hw::vr

#endif  // TROSSEN_SDK__HW__VR__VR_MDNS_ADVERTISER_HPP_
