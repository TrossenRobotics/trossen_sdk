"""Open the devices a config declares. One implementation, one definition.

Imported by the runner subprocesses (`app.hw_test_runner`,
`app.recorder_runner`, `app.recover_runner`), never by the backend itself — it
touches hardware, and doing that in the uvicorn process is what the runners
exist to avoid.

**Why this module exists.** There were two bodies of code that each decided what
a config *means* — the recorder's `_build_session_manager` and the hardware
test's runner, the latter carrying a docstring admitting it "mirrors" the former.
Keeping two traversals in agreement by hand failed in exactly the way it always
does: the test skipped `hardware.components` entirely, so a Rivet — which
declares its base there rather than in the legacy `mobile_base` slot — passed its
hardware test with an unreachable base. That bug was fixed by teaching the test
about one component type. The next divergence would have been free to happen
again. Here, a caller chooses which *sets* it wants; it does not get to have its
own opinion about how a set is traversed.

The arm retry wrapper had three copies for the same reason, and one of them —
the recovery path, where the arms are by definition in a bad state — was missing
the transient-error filter, so it retried unrecoverable failures three times over
before reporting them. There is one copy here, and it has the filter.

**Timing.** Every device open is timed and reported on stdout as a `[timing]`
line. Nothing in this code path used to measure anything: the constants that
bound it are timeout *budgets* grown out of incidents, so "how long does a
bring-up actually take" had no answer, and the budgets could not be checked
against reality. `[timing]` lines flow through the runner protocol as ordinary
progress, so they reach the operator's output pane and any log capture with no
extra plumbing. They are deliberately not `[error]` / `[critical]`, which the
protocol treats as failure markers.
"""

from __future__ import annotations

import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import Any

import trossen_sdk as ts

# An arm controller accepts a single client and does not release a dead one
# immediately. A prior run's connection — most often a recorder SIGKILLed on a
# fault before its driver could disconnect, or a hardware test that just
# finished — makes the next TCP connect stall its full ~20s timeout and throw.
# The stale client clears controller-side shortly after, so retrying turns the
# old "start fails -> Recover -> try again" dance into a single successful start.
#
# Two retries (three attempts) covers a controller needing more than one timeout
# cycle to release. The bring-up budgets that bound each caller
# (`hw_test.compute_bringup_budget`, `recorder._BOOTSTRAP_TIMEOUT_S`,
# `recover._TIMEOUT_FLOOR_S`) are all sized to allow this.
ARM_CONNECT_RETRIES = 2
ARM_RETRY_BACKOFF_S = 1.0

# Component types under `hardware.components` that own a real device, as opposed
# to wiring teleop over devices created elsewhere.
#
# This distinction earns its keep three times: it is what a connectivity test
# should cover, it is what recovery can clear a latch on, and it is what may be
# opened concurrently with the arms (the wiring components resolve arms out of
# the active registry, so they cannot).
DEVICE_COMPONENT_TYPES = frozenset({"trossen_base", "slate_base"})


@dataclass(frozen=True)
class BringUpSet:
    """Which parts of a config a caller wants opened.

    A caller picks sets; it does not get its own traversal. `wiring_components`
    is separate from `device_components` because the wiring ones resolve the
    arms out of the active registry and so are only meaningful once the arms and
    devices are up.
    """

    arms: bool = True
    cameras: bool = True
    mobile_base: bool = True
    device_components: bool = True
    wiring_components: bool = True

    @property
    def any_components(self) -> bool:
        return self.device_components or self.wiring_components


#: Everything: what a recording session opens.
EVERYTHING = BringUpSet()

#: Devices only: what a connectivity test should prove. Excludes the teleop
#: wiring, which would test the config's plumbing rather than whether anything
#: is plugged in.
DEVICES_ONLY = BringUpSet(wiring_components=False)


@dataclass
class BroughtUp:
    """What a bring-up produced.

    The caller must keep these references for as long as it needs the hardware:
    the SDK holds only weak references via its callbacks, and the components are
    otherwise owned solely by the ActiveHardwareRegistry.
    """

    arms: dict[str, Any] = field(default_factory=dict)
    cameras: dict[str, Any] = field(default_factory=dict)
    camera_cfgs: dict[str, Any] = field(default_factory=dict)
    components: dict[str, Any] = field(default_factory=dict)
    session_controls: list[Any] = field(default_factory=list)
    mobile_base: Any | None = None
    timings: list[tuple[str, float]] = field(default_factory=list)

    device_component_ids: list[str] = field(default_factory=list)
    """Ids from `components` that own a device. Recorded as they are created
    rather than re-derived afterwards: the component object exposes no type, so
    the only reliable source is the config entry we opened it from."""

    def summary(self) -> str:
        """One human line naming what was opened, for a success marker."""
        parts = [f"{len(self.arms)} arm(s)", f"{len(self.cameras)} camera(s)"]
        if self.mobile_base is not None:
            parts.append("1 mobile base")
        if self.device_component_ids:
            parts.append(f"{len(self.device_component_ids)} base/component(s)")
        n_wiring = len(self.components) - len(self.device_component_ids)
        if n_wiring:
            parts.append(f"{n_wiring} teleop component(s)")
        return ", ".join(parts)


def report_timings(brought: BroughtUp) -> None:
    """Print the per-device breakdown and the total.

    Emitted as one block at the end as well as per-device inline, so a reader
    scrolling a long SDK log has somewhere to look that is not interleaved with
    driver chatter.
    """
    if not brought.timings:
        return
    total = sum(seconds for _label, seconds in brought.timings)
    widest = max(len(label) for label, _ in brought.timings)
    print("[timing] breakdown:", flush=True)
    for label, seconds in brought.timings:
        print(f"[timing]   {label.ljust(widest)}  {seconds:6.2f}s", flush=True)
    # `total` is the sum of the individual opens. Under concurrent bring-up it
    # deliberately exceeds the wall-clock it took to do them, and the gap
    # between the two IS the win — so print both rather than only the sum.
    print(f"[timing] sum of opens = {total:.2f}s "
          f"across {len(brought.timings)} device(s)", flush=True)


@contextmanager
def _timed(label: str, sink: list[tuple[str, float]]) -> Iterator[None]:
    """Time one device open, record it, and say so on stdout.

    Records on the failure path too: "the arm that failed took 21s to fail" is
    the single most useful line when diagnosing a connect that is timing out
    rather than being refused.
    """
    start = time.perf_counter()
    try:
        yield
    except BaseException:
        elapsed = time.perf_counter() - start
        sink.append((f"{label} (FAILED)", elapsed))
        print(f"[timing] {label} failed after {elapsed:.2f}s", flush=True)
        raise
    else:
        elapsed = time.perf_counter() - start
        sink.append((label, elapsed))
        print(f"[timing] {label} = {elapsed:.2f}s", flush=True)


def is_transient_connect_error(exc: BaseException) -> bool:
    """Whether `exc` is worth a retry, or is a config error retrying cannot fix.

    Matched on the message because that is all pybind11 gives us: the SDK throws
    `std::runtime_error`, which arrives as a bare `RuntimeError`, so there is no
    exception type to branch on. Deliberately narrow — a wrong IP, an unknown
    model or a bad end-effector must fail on the first attempt, not burn two more
    full connect timeouts first.
    """
    low = str(exc).lower()
    return (
        "connect to the arm controller" in low
        or "temporarily unavailable" in low
        or ("within" in low and "second" in low)
    )


def create_arm(
    arm_id: str,
    arm_json: dict[str, Any],
    *,
    timings: list[tuple[str, float]] | None = None,
) -> Any:
    """Create one `trossen_arm` component, retrying a transient connect failure.

    The single copy of what used to be three, one of which retried
    unrecoverable errors — see the module docstring.
    """
    sink = timings if timings is not None else []
    last_exc: Exception | None = None
    for attempt in range(ARM_CONNECT_RETRIES + 1):
        label = f"arm {arm_id}" if attempt == 0 else f"arm {arm_id} (retry {attempt})"
        try:
            with _timed(label, sink):
                return ts.HardwareRegistry.create("trossen_arm", arm_id, arm_json, True)
        except Exception as exc:  # noqa: BLE001 - pybind11 surfaces C++ throws here
            last_exc = exc
            if attempt >= ARM_CONNECT_RETRIES or not is_transient_connect_error(exc):
                raise
            print(
                f"arm '{arm_id}' connect failed (attempt {attempt + 1} of "
                f"{ARM_CONNECT_RETRIES + 1}) — the controller may still hold a "
                f"prior client; retrying in {ARM_RETRY_BACKOFF_S}s: {exc}",
                flush=True,
            )
            time.sleep(ARM_RETRY_BACKOFF_S)
    assert last_exc is not None  # the loop either returned or re-raised
    raise last_exc


def is_device_component(comp_cfg: Any) -> bool:
    """Whether a `hardware.components` entry owns a device."""
    return comp_cfg.type in DEVICE_COMPONENT_TYPES


def bring_up(
    cfg: Any,
    want: BringUpSet = EVERYTHING,
    *,
    arm_overrides: dict[str, Any] | None = None,
) -> BroughtUp:
    """Open the hardware `cfg` declares, in dependency order.

    `cfg` is a `trossen_sdk.SdkConfig`. `arm_overrides` is merged into every
    arm's JSON before it is created — the hardware test uses it to pin
    `teleop_moving_time_s` so its park-at-zero step always has the same
    wall-clock cost regardless of the operator's setting.

    Order is load-bearing, and is the recorder's original order:

      1. **arms** — nothing else can resolve them until they exist;
      2. **device components** (a Rivet's swerve base) — independent of the arms
         but needed before any wiring that pairs against them;
      3. **wiring components** (`glide_arm_input`, `glide_base`,
         `glide_session_control`) — in declared order, because each resolves
         hardware created above out of the active registry;
      4. **cameras** — depended on by nothing.

    Raises whatever the SDK raises, unchanged. Callers that need a per-device
    verdict instead of fail-fast (recovery) drive `create_arm` themselves.
    """
    brought = BroughtUp()

    if want.arms:
        for arm_id, arm_cfg in cfg.hardware.arms.items():
            arm_json = arm_cfg.to_json()
            if arm_overrides:
                arm_json.update(arm_overrides)
            brought.arms[arm_id] = create_arm(
                arm_id, arm_json, timings=brought.timings
            )

    if want.mobile_base and cfg.hardware.mobile_base is not None:
        # The legacy `hardware.base` slot. A decomposed config declares its base
        # as a component instead, which is why both paths exist.
        with _timed("mobile_base slate_base", brought.timings):
            brought.mobile_base = ts.HardwareRegistry.create(
                "slate_base", "slate_base", cfg.hardware.mobile_base.to_json(), True
            )

    if want.any_components:
        for comp_cfg in cfg.hardware.components:
            device = is_device_component(comp_cfg)
            if device and not want.device_components:
                continue
            if not device and not want.wiring_components:
                continue
            _create_component(comp_cfg, brought)

    if want.cameras:
        for cam_cfg in cfg.hardware.cameras:
            with _timed(f"camera {cam_cfg.id} ({cam_cfg.type})", brought.timings):
                brought.cameras[cam_cfg.id] = ts.HardwareRegistry.create(
                    cam_cfg.type, cam_cfg.id, cam_cfg.to_json()
                )
            brought.camera_cfgs[cam_cfg.id] = cam_cfg

    return brought


def _create_component(comp_cfg: Any, brought: BroughtUp) -> None:
    """Create one `hardware.components` entry and file it under `brought`.

    `comp_cfg.raw` is the entry verbatim — `ComponentConfig::to_json()` returns
    exactly that — and is what the component's own `configure()` parses, so a
    newly registered hardware type needs no change here.
    """
    if comp_cfg.type == "trossen_base":
        # A swerve base re-zeros its pivot modules inside configure(), blocking
        # for as long as the mechanical home takes. Say so BEFORE the call: the
        # operator sees the wheels turn, and without a line here the stream just
        # stalls with no indication of why.
        print(f"base '{comp_cfg.id}': connecting, then homing the swerve modules "
              f"(the wheels will turn on the spot)", flush=True)

    with _timed(f"component {comp_cfg.id} ({comp_cfg.type})", brought.timings):
        component = ts.HardwareRegistry.create(
            comp_cfg.type, comp_cfg.id, comp_cfg.raw, True
        )

    brought.components[comp_cfg.id] = component
    if is_device_component(comp_cfg):
        brought.device_component_ids.append(comp_cfg.id)
    if isinstance(component, ts.SessionControlCapable):
        # Collected by interface rather than by type, so another button source
        # needs no change here. NOT attached to the SessionManager: the episode
        # loop drives sessions from signal events and wires the buttons to those
        # same events, and attaching them here as well would give one session two
        # independent drivers.
        brought.session_controls.append(component)
    print(f"component '{comp_cfg.id}' ({comp_cfg.type}) configured", flush=True)
