/**
 * @file teleop_capable.hpp
 * @brief Mixin interfaces for teleop-capable hardware.
 *
 * Teleop capability is structured around four interfaces:
 *
 *   - TeleopSpaceIO          : pure IO contract (`read()` / `write()`) that
 *                              the controller's hot loop talks to.
 *   - TeleopCapable          : base mixin with space-agnostic lifecycle hooks
 *                              and a single `as_space_io(Space)` accessor that
 *                              returns the IO view for the requested space (or
 *                              nullptr if unsupported).
 *   - JointSpaceTeleop       : TeleopCapable + TeleopSpaceIO for joint space.
 *   - CartesianSpaceTeleop   : TeleopCapable + TeleopSpaceIO for cartesian space.
 *
 * A hardware component must implement at least one space child to be usable
 * by the teleop controller. The controller calls `as_space_io(cfg.space)`
 * at construction and throws if the return is null.
 *
 * Single-space hardware inherits the relevant space child directly. The
 * child auto-wires `as_space_io` to return `this` for its space:
 *
 * @code
 *   class MyArm : public HardwareComponent, public JointSpaceTeleop {
 *     std::vector<float> read() override { ... }
 *     void               write(const std::vector<float>& cmd) override { ... }
 *     // optional: override lifecycle hooks from TeleopCapable
 *   };
 * @endcode
 *
 * Multi-space hardware (a component that supports more than one space)
 * inherits TeleopCapable directly and exposes each space through a nested
 * adapter sub-object that implements TeleopSpaceIO. The component overrides
 * `as_space_io` with a switch that dispatches to the correct adapter.
 *
 * ── Adding a new teleop space ─────────────────────────────────────────────
 *
 *  1. Add a new value to the `Space` enum below, before the `Count` sentinel.
 *  2. Add a row to `kSpaceDescriptors` (name + interface name for errors).
 *  3. Add a child class (e.g. `VelocitySpaceTeleop`) that inherits
 *     `TeleopCapable` virtually and `TeleopSpaceIO`, mirroring the existing
 *     joint/cartesian children.
 *  4. Any hardware that should support the new space either inherits the new
 *     child (single-space) or extends its `as_space_io` switch (multi-space).
 *
 * The resolver, error messages, and JSON parse all go through
 * `kSpaceDescriptors` and `as_space_io` — no other layer needs to change.
 */

#ifndef TROSSEN_SDK__HW__TELEOP__TELEOP_CAPABLE_HPP_
#define TROSSEN_SDK__HW__TELEOP__TELEOP_CAPABLE_HPP_

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::teleop {

/**
 * @brief Pure IO contract the teleop hot loop calls every tick.
 *
 * Each space child inherits this, allowing the controller to hold a single
 * `TeleopSpaceIO*` regardless of which space is active. The interface is
 * intentionally narrow: only `read()` and `write()` differ per space.
 * Space-agnostic setup (alignment, mode changes) lives on
 * `TeleopCapable::prepare_for_teleop`.
 */
class TeleopSpaceIO {
public:
  virtual ~TeleopSpaceIO() = default;

  /// Return the component's current state in this space (leader role).
  ///
  /// TODO(shantanuparab-tr): revisit with a span / caller-owned output
  /// buffer if profiling shows allocator pressure at high control rates.
  /// Returning a vector by value encourages a fresh allocation per tick,
  /// which is cheap at 1 kHz with small state vectors but becomes
  /// meaningful at higher rates or larger payloads.
  virtual std::vector<float> read() = 0;

  /// Apply a teleop command in this space (follower role).
  virtual void write(const std::vector<float>& cmd) = 0;

  /// Optional. Called once by the controller before the mirror loop starts,
  /// with the follower's current state in this space. Real-hardware leaders
  /// have no internal state to sync, so the default is a no-op. Virtual
  /// leaders override this to align their starting state with the follower
  /// before mirroring begins.
  virtual void sync_to_state(const std::vector<float>& state) {
    (void)state;
  }
};

/**
 * @brief Lifecycle base for teleop-capable hardware.
 *
 * Lifecycle hooks are space-agnostic: the controller invokes them on the
 * owning hardware component (not on a per-space IO view), so shared state
 * and mode changes live in one place. All hooks have no-op defaults;
 * components only override what they need.
 *
 * Subclasses override `as_space_io(Space)` to advertise which spaces they
 * support. A non-null return unlocks that space for this hardware.
 */
class TeleopCapable {
public:
  /// Teleop spaces a component may operate in.
  ///
  /// `Count` is a sentinel (not a real space) used to make the descriptor
  /// table's compile-time check meaningful. When adding a new space,
  /// insert it before `Count` and add a matching row to `kSpaceDescriptors`.
  enum class Space { Joint, Cartesian, Count };

  virtual ~TeleopCapable() = default;

  /// Return the IO view for `space`, or nullptr if this hardware does not
  /// implement that space. Called by TeleopController at construction to
  /// resolve the leader and follower to concrete `TeleopSpaceIO*`s.
  virtual TeleopSpaceIO* as_space_io(Space space) {
    (void)space;
    return nullptr;
  }

  // ── Lifecycle hooks (no-op defaults) ───────────────────────────────────
  //
  // All lifecycle hooks are argument-free. Each implementation reads its
  // own tuning (trajectory times, staging poses, gripper tolerances, etc.)
  // and role state from members populated at configure() time. This keeps
  // the controller's contract minimal and lets hardware with wildly
  // different tuning coexist without interface churn.

  /// Prepare this hardware for teleop in its configured role (leader vs
  /// follower is inferred from the component's own configuration).
  virtual void prepare_for_teleop() {}

  /// Gracefully end teleop: return to rest and release driver resources.
  /// Not called from the controller's destructor — the controller only
  /// joins its mirror thread on destruction, and each hardware component
  /// is responsible for emergency cleanup in its own destructor.
  virtual void end_teleop() {}

  /// Move hardware to its configured staging pose at session start.
  virtual void stage() {}

  // ── Episode hooks (no-op defaults) ─────────────────────────────────────

  /// Called before each teleop cycle (e.g. per-episode hardware prep).
  virtual void pre_episode() {}

  /// Called after each teleop cycle (e.g. enter reset/free-drive mode).
  virtual void post_episode() {}
};

// ── Space metadata table ─────────────────────────────────────────────────
//
// Single source of truth for space names and interface names. All helpers
// below (name lookup, JSON parse) read this table, so adding a new space
// requires only one row here and one enum value above.

struct SpaceDescriptor {
  TeleopCapable::Space space;
  std::string_view     name;        ///< JSON / log name, e.g. "joint".
  std::string_view     iface_name;  ///< C++ interface, e.g. "JointSpaceTeleop".
};

inline constexpr std::array<SpaceDescriptor, 2> kSpaceDescriptors{{
  {TeleopCapable::Space::Joint,     "joint",     "JointSpaceTeleop"},
  {TeleopCapable::Space::Cartesian, "cartesian", "CartesianSpaceTeleop"},
}};

// Compile-time check: every Space enum value has a descriptor row. The
// `Space::Count` sentinel gives this assertion teeth — adding a new enum
// value without adding its row fails to compile here.
static_assert(
  kSpaceDescriptors.size() ==
    static_cast<std::size_t>(TeleopCapable::Space::Count),
  "kSpaceDescriptors must have one row per TeleopCapable::Space value. "
  "Add the new row above; the compiler will then be satisfied.");

/// Lower-case name used in JSON and log output. Returns "unknown" if `s` is
/// absent from `kSpaceDescriptors` (should never happen for a well-formed
/// enum value).
inline std::string_view space_name(TeleopCapable::Space s) {
  for (const auto& d : kSpaceDescriptors) {
    if (d.space == s) return d.name;
  }
  return "unknown";
}

/// C++ interface name used in error messages, e.g. "JointSpaceTeleop".
inline std::string_view space_iface_name(TeleopCapable::Space s) {
  for (const auto& d : kSpaceDescriptors) {
    if (d.space == s) return d.iface_name;
  }
  return "unknown";
}

/// Parse a JSON space string ("joint", "cartesian", …) to the enum.
/// Returns std::nullopt on unknown input.
inline std::optional<TeleopCapable::Space>
space_from_name(std::string_view name) {
  for (const auto& d : kSpaceDescriptors) {
    if (d.name == name) return d.space;
  }
  return std::nullopt;
}

// ── Space child classes ──────────────────────────────────────────────────

/**
 * @brief Joint-space teleop interface. Inherit to unlock joint teleop.
 *
 * Direct inheritance auto-wires `as_space_io(Space::Joint)` to return `this`,
 * so single-space hardware (e.g. SO101 arm) needs no extra plumbing.
 */
class JointSpaceTeleop : public virtual TeleopCapable, public TeleopSpaceIO {
public:
  TeleopSpaceIO* as_space_io(Space s) override {
    return s == Space::Joint ? static_cast<TeleopSpaceIO*>(this) : nullptr;
  }
};

/**
 * @brief Cartesian-space teleop interface. Inherit to unlock cartesian teleop.
 */
class CartesianSpaceTeleop : public virtual TeleopCapable, public TeleopSpaceIO {
public:
  TeleopSpaceIO* as_space_io(Space s) override {
    return s == Space::Cartesian ? static_cast<TeleopSpaceIO*>(this) : nullptr;
  }
};

/**
 * @brief Query a HardwareComponent for the TeleopCapable interface.
 * Returns nullptr if the component does not implement TeleopCapable.
 */
template <typename Capability = TeleopCapable>
std::shared_ptr<Capability> as_capable(const std::shared_ptr<HardwareComponent>& hw) {
  return std::dynamic_pointer_cast<Capability>(hw);
}

}  // namespace trossen::hw::teleop

#endif  // TROSSEN_SDK__HW__TELEOP__TELEOP_CAPABLE_HPP_
