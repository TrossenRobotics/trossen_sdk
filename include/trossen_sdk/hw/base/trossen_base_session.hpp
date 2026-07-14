/**
 * @file trossen_base_session.hpp
 * @brief Process-wide shared Trossen Base connection for trossen_sdk hardware.
 */

#ifndef TROSSEN_SDK__HW__BASE__TROSSEN_BASE_SESSION_HPP_
#define TROSSEN_SDK__HW__BASE__TROSSEN_BASE_SESSION_HPP_


#include <optional>
#include <string>

namespace trossen::hw::trossen_base {

/**
 * @brief Process-global shared trossen_base controller session.
 *
 * Every component must share one base driver.
 *
 * Ownership model:
 *  - The first component to call `ensure_started(port)` initialize the driver
 *  - Each `ensure_started` pairs with exactly one `release`. When the last
 *    reference goes away the session is stopped.
 *
 * Thread-safety: all public methods are safe to call from any thread.
 */
class TrossenBaseSession {
public:
  /// Access the process-global session instance.
  static TrossenBaseSession& instance();

  /**
   * @brief Idempotently start the trossen driver.
   * @param ip_address IP address of the base controller.
   * @throws std::runtime_error if already running on a different IP.
   */
  void ensure_started(std::string ip);

  /**
   * @brief Decrement the reference count; stops driver
   *
   * Safe to call more times than `ensure_started()`; extra calls are no-ops
   * so teardown code does not need to track its own ownership flag.
   */
  void release();

  TrossenBaseSession(const TrossenBaseSession&)            = delete;
  TrossenBaseSession& operator=(const TrossenBaseSession&) = delete;
  TrossenBaseSession(TrossenBaseSession&&)                 = delete;
  TrossenBaseSession& operator=(TrossenBaseSession&&)      = delete;

private:
  TrossenBaseSession() = default;
  ~TrossenBaseSession();

  std::string  ip_address_;
  std::size_t  ref_count_{0};

};

/**
 * @brief RAII lease on the shared TrossenBaseSession for one hardware component.
 *
 * Acquires a reference on `acquire()`, and releases the reference plus the component's
 * input claims exactly once on `reset()` / destruction. This makes an accidental
 * double-release impossible, so a component can never close the shared
 * connection while another is still using it. Move-only.
 */
class TrossenBaseLease {
public:
  TrossenBaseLease() = default;
  ~TrossenBaseLease() { reset(); }

  TrossenBaseLease(const TrossenBaseLease&)            = delete;
  TrossenBaseLease& operator=(const TrossenBaseLease&) = delete;
  TrossenBaseLease(TrossenBaseLease&& other) noexcept { *this = std::move(other); }
  TrossenBaseLease& operator=(TrossenBaseLease&& other) noexcept {
    if (this != &other) {
      reset();
      held_         = other.held_;
      other.held_   = false;
    }
    return *this;
  }


  /**
   * @brief Start or join the shared session
   */
  void acquire(std::string ip) {
    reset();
    TrossenBaseSession::instance().ensure_started(ip);
    held_ = true;
  }

  /// Release the reference. Idempotent.
  void reset() {
    if (held_) {
      TrossenBaseSession::instance().release();
      held_ = false;
    }
  }

  bool held() const { return held_; }

private:
  bool        held_{false};
};

}  // namespace trossen::hw::trossen_base

#endif  // TROSSEN_SDK__HW__BASE__TROSSEN_BASE_SESSION_HPP_
