/**
 * @file glide_session.hpp
 * @brief Shared input arbitration for Trossen Glide leader handles.
 */

#ifndef TROSSEN_SDK__HW__GLIDE__GLIDE_SESSION_HPP_
#define TROSSEN_SDK__HW__GLIDE__GLIDE_SESSION_HPP_

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace trossen::hw::glide {

/**
 * @brief A logical input on a Glide handle.
 *
 * A Glide is a passive leader arm whose handle also carries an analog
 * joystick and a set of momentary buttons. Several hardware components read
 * the same physical handle for unrelated purposes — the arm joints drive a
 * follower arm, the joystick drives the mobile base, the buttons drive the
 * session — so each declares what it consumes and GlideSession rejects
 * overlaps at configure() time rather than letting two components silently
 * fight over one input.
 */
enum class GlideInput {
  /// The 2-axis analog joystick, claimed as a unit (x and y are never split
  /// between components — nothing sensible reads one axis in isolation).
  kJoystick,

  /// One momentary button, identified by its bit position in the handle's
  /// button bitmask. Claims on this input must carry a `bit`.
  kButton,
};

std::string_view glide_input_name(GlideInput input);

/**
 * @brief One input a component wants to reserve on a handle.
 *
 * For `kButton`, `bit` selects which button — the index of its bit in the
 * handle bitmask. For every other input `bit` is unused and left at -1.
 */
struct GlideClaim {
  GlideInput input;
  int        bit{-1};
};

/// Convenience: a claim on button `bit`.
inline GlideClaim glide_button(int bit) { return GlideClaim{GlideInput::kButton, bit}; }

/**
 * @brief A snapshot of one handle's non-arm inputs.
 *
 * Deliberately an SDK-owned POD rather than the driver's own report type: it
 * keeps libtrossen_arm out of this header, so everything built on top of it is
 * a pure function of a snapshot and compiles on any machine regardless of which
 * driver build is installed.
 */
struct GlideInputSnapshot {
  /// Raw joystick axes, in the handle's native counts (see kJoystickMin/Max).
  std::uint16_t joystick_x{0};
  std::uint16_t joystick_y{0};

  /// Momentary-button bitmask; bit N is set while button N is held.
  std::uint32_t buttons{0};

  /// True if bit `bit` is currently held. Out-of-range bits read as false.
  bool button(int bit) const {
    if (bit < 0 || bit >= 32) return false;
    return (buttons & (1u << static_cast<unsigned>(bit))) != 0u;
  }
};

/// Raw joystick range reported by the handle. Centre is the midpoint, so a
/// resting stick reads ~2047 rather than 0 — every consumer must rescale.
inline constexpr std::uint16_t kJoystickMin = 0;
inline constexpr std::uint16_t kJoystickMax = 4095;

/**
 * @brief Reads one handle's current inputs, or nullopt if unavailable.
 *
 * The indirection that keeps this whole layer driver-agnostic. A handle's
 * joystick and buttons arrive over the same libtrossen_arm driver that streams
 * its joint positions, but that API exists only on driver builds with
 * lightweight-leader support — so nothing here may reference it. Instead the
 * owner of the driver registers a reader, and GlideSession only dispatches.
 */
using GlideInputReader = std::function<std::optional<GlideInputSnapshot>()>;

/// Number of momentary buttons a handle carries.
///
/// The four bits are a physical cross on the handle, and the mapping is not the
/// order one would guess: **0 = top, 1 = left, 2 = bottom, 3 = right.** Bits 1
/// and 3 were documented the wrong way round until 2026-08-07, so a config
/// written against the old note binds left and right swapped. Nothing validates
/// a bit value beyond its range, so that mistake is silent.
inline constexpr int kGlideButtonCount = 4;

/**
 * @brief Process-wide input arbitration for Glide handles.
 *
 * This owns no connection — the driver belongs to the arm component. What it
 * owns is the *right to read a given input* and a registry of per-handle
 * readers, so several components can share one physical handle without a
 * central owner that knows about all of them.
 *
 * Thread-safety: all public methods take an internal mutex and are safe from
 * any thread. Registered readers are invoked outside the lock, so one that
 * blocks on hardware cannot stall an unrelated component's claim.
 */
class GlideSession {
public:
  /// Access the process-global session instance.
  static GlideSession& instance();

  /**
   * @brief Register the input source for `arm_id`, replacing any previous one.
   *
   * Called by whatever owns the handle's driver. Registering is independent of
   * claiming: a reader can exist with nothing claimed, and a claim can be made
   * before its reader is registered (`read_inputs()` simply returns nullopt
   * until it is).
   */
  void register_reader(const std::string& arm_id, GlideInputReader reader);

  /// Drop the reader for `arm_id`. Safe if none is registered.
  void unregister_reader(const std::string& arm_id);

  /**
   * @brief Reserve a set of inputs on one handle for a component.
   *
   * Maintains an `(arm_id, input, bit) -> component_id` table and throws if
   * any requested input is already held by a *different* component. Repeating
   * an identical claim from the same component is idempotent, so a component
   * that is reconfigured does not have to release first.
   *
   * @param arm_id        Hardware id of the handle's arm component.
   * @param component_id  Stable id of the claiming component.
   * @param claims        Inputs to reserve on that handle.
   *
   * @throws std::invalid_argument if either id is empty, or a `kButton` claim
   *         has a bit outside 0..kGlideButtonCount-1.
   * @throws std::runtime_error if an input is already claimed elsewhere. The
   *         message names both components and the contested input.
   */
  void claim_inputs(const std::string& arm_id,
                    const std::string& component_id,
                    std::initializer_list<GlideClaim> claims);

  /// Release every claim held by `component_id`. Safe with no claims
  /// outstanding; typically called from a component's destructor.
  void release_claims(const std::string& component_id);

  /// Component currently holding `claim` on `arm_id`, or nullopt if free.
  std::optional<std::string> claim_holder(const std::string& arm_id,
                                          const GlideClaim& claim) const;

  /**
   * @brief Read a handle's current inputs.
   *
   * Dispatches to the reader registered for `arm_id`. Returns nullopt when no
   * reader is registered or the reader itself reports no data — the caller
   * decides whether that is fatal.
   */
  std::optional<GlideInputSnapshot> read_inputs(const std::string& arm_id) const;

  GlideSession(const GlideSession&)            = delete;
  GlideSession& operator=(const GlideSession&) = delete;
  GlideSession(GlideSession&&)                 = delete;
  GlideSession& operator=(GlideSession&&)      = delete;

private:
  GlideSession() = default;
  ~GlideSession() = default;

  /// `(arm_id, input, bit) -> component_id`.
  struct ClaimKey {
    std::string arm_id;
    GlideInput  input;
    int         bit;
    bool operator==(const ClaimKey& other) const {
      return arm_id == other.arm_id && input == other.input && bit == other.bit;
    }
  };
  struct ClaimKeyHash {
    std::size_t operator()(const ClaimKey& k) const noexcept {
      // Table stays tiny (a handful of handles x a handful of inputs), so a
      // cheap xor-shift combine is ample.
      return std::hash<std::string>{}(k.arm_id) ^
             (std::hash<int>{}(static_cast<int>(k.input)) << 1) ^
             (std::hash<int>{}(k.bit) << 3);
    }
  };

  mutable std::mutex mutex_;
  std::unordered_map<ClaimKey, std::string, ClaimKeyHash> claims_;
  std::unordered_map<std::string, GlideInputReader> readers_;
};

/**
 * @brief RAII holder for one component's Glide input claims.
 *
 * Releases the component's claims exactly once on reset or destruction, so a
 * component cannot leak a reservation and block its own replacement after a
 * reconfigure. Move-only.
 */
class GlideClaimLease {
public:
  GlideClaimLease() = default;
  ~GlideClaimLease() { reset(); }

  GlideClaimLease(const GlideClaimLease&)            = delete;
  GlideClaimLease& operator=(const GlideClaimLease&) = delete;
  GlideClaimLease(GlideClaimLease&& other) noexcept { *this = std::move(other); }
  GlideClaimLease& operator=(GlideClaimLease&& other) noexcept {
    if (this != &other) {
      reset();
      component_id_ = std::move(other.component_id_);
      held_         = other.held_;
      other.held_   = false;
    }
    return *this;
  }

  /// Claim `claims` on `arm_id` and take ownership of their release.
  ///
  /// Additive across calls, because one component may legitimately read
  /// different inputs on different handles — the Rivet base takes a joystick
  /// from each Glide. Release is component-wide, so a single lease covers every
  /// handle. Propagates the conflict exception from `claim_inputs()` without
  /// recording ownership on failure.
  ///
  /// @throws std::invalid_argument if called with a different component_id than
  ///         an earlier call on this lease.
  void add(const std::string& arm_id,
           const std::string& component_id,
           std::initializer_list<GlideClaim> claims) {
    if (held_ && component_id != component_id_) {
      throw std::invalid_argument(
        "GlideClaimLease: lease already holds claims for '" + component_id_ +
        "', cannot also hold them for '" + component_id + "'");
    }
    GlideSession::instance().claim_inputs(arm_id, component_id, claims);
    component_id_ = component_id;
    held_ = true;
  }

  /// Release this component's claims. Idempotent.
  void reset() {
    if (held_) {
      GlideSession::instance().release_claims(component_id_);
      held_ = false;
    }
  }

  bool held() const { return held_; }

private:
  std::string component_id_;
  bool        held_{false};
};

}  // namespace trossen::hw::glide

#endif  // TROSSEN_SDK__HW__GLIDE__GLIDE_SESSION_HPP_
