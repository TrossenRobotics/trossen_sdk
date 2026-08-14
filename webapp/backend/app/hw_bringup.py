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
from collections.abc import Callable, Iterator
from concurrent.futures import ThreadPoolExecutor
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


@dataclass(frozen=True)
class Concurrency:
    """Which device opens may overlap.

    Orthogonal to `BringUpSet`: that says *what* to open, this says *how fast*.
    Staged by confidence rather than turned on wholesale, because the three
    device families carry very different risk.
    """

    devices: bool = True
    """Arms and device components (a swerve base) open together.

    The clear win, and the safe one. Each arm is an independent TCP connection to
    its own controller with its own driver; the base is on a different bus
    entirely. Nothing here shares state, so the cost of a bring-up drops from
    sum(devices) to max(devices) -- on a 4-arm Rivet, four serial ~6s connects
    become one, and the base's mechanical homing overlaps them instead of
    following them.
    """

    cameras: bool = False
    """Cameras open together. OFF by default, and it should stay that way until
    someone measures it on the target rig.

    Three reasons to be suspicious rather than optimistic here:

      - a ZED's Camera::open() does GPU work (it loads and optimises a NEURAL
        depth model), so three concurrent opens may contend on GPU memory rather
        than overlap, and could be slower than serial;
      - RealsenseCameraComponent's destructor heap-corrupts on librealsense 2.56,
        and more concurrent lifecycle churn is not the way to find out how badly;
      - an unclean exit leaks the ZED Argus CameraProvider, and the NEXT open
        then fails with CANNOT_START_CAMERA_STREAM until the rig is rebooted.
        The failure mode is a rig that cannot open its cameras at all, which is
        far worse than a slow bring-up.
    """


#: The default: overlap the arms and the base, keep cameras serial.
CONCURRENT = Concurrency()

#: One device at a time. The old behaviour, kept as an escape hatch for
#: bisecting a rig that misbehaves under concurrent bring-up.
SERIAL = Concurrency(devices=False, cameras=False)


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

    wall_clock_s: float = 0.0
    """How long the whole bring-up actually took. Distinct from the sum of
    `timings`, and the gap between the two is what concurrency bought."""

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
    # Both numbers, because the gap between them IS the win: `total` is what the
    # opens cost added up, `wall_clock` is what the operator actually waited.
    # Serially they match; concurrently the wall clock approaches the slowest
    # single device instead of the sum of all of them.
    print(f"[timing] sum of opens = {total:.2f}s "
          f"across {len(brought.timings)} device(s)", flush=True)
    if brought.wall_clock_s > 0.0:
        saved = total - brought.wall_clock_s
        print(f"[timing] wall clock   = {brought.wall_clock_s:.2f}s "
              f"({saved:+.2f}s vs. opening one at a time)", flush=True)


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


def _run_all(
    tasks: list[Callable[[], Any]],
    *,
    parallel: bool,
    label: str,
) -> list[Any]:
    """Run every task and return the results in the order given.

    `parallel` decides whether they overlap. Either way EVERY task is waited for
    before anything is raised: letting a bring-up unwind while sibling threads
    are still mid-open would tear the process down around drivers that are
    part-way through opening a device, which is how an arm controller or a ZED is
    left holding a connection nobody owns. The first exception is re-raised once
    the dust settles, so a caller still sees the real cause.
    """
    if not parallel or len(tasks) <= 1:
        return [task() for task in tasks]

    with ThreadPoolExecutor(max_workers=len(tasks), thread_name_prefix=label) as pool:
        futures = [pool.submit(task) for task in tasks]
        results: list[Any] = []
        first_exc: BaseException | None = None
        for future in futures:
            try:
                results.append(future.result())
            except BaseException as exc:  # noqa: BLE001 - re-raised below
                results.append(None)
                if first_exc is None:
                    first_exc = exc
    if first_exc is not None:
        raise first_exc
    return results


def bring_up(
    cfg: Any,
    want: BringUpSet = EVERYTHING,
    *,
    arm_overrides: dict[str, Any] | None = None,
    concurrency: Concurrency = CONCURRENT,
) -> BroughtUp:
    """Open the hardware `cfg` declares, in dependency order.

    `cfg` is a `trossen_sdk.SdkConfig`. `arm_overrides` is merged into every
    arm's JSON before it is created — the hardware test uses it to pin
    `teleop_moving_time_s` so its park-at-zero step always has the same
    wall-clock cost regardless of the operator's setting.

    Three stages, and the boundaries between them are dependencies rather than
    taste:

      1. **arms + device components** (a Rivet's swerve base, a legacy
         `hardware.base`). Mutually independent — separate controllers, separate
         buses — so these open concurrently. This is where the latency win is.
      2. **wiring components** (`glide_arm_input`, `glide_base`,
         `glide_session_control`), serially and in declared order. Each resolves
         hardware from stage 1 out of the active registry, so none of it can
         start until stage 1 has finished, and reordering them can break a
         component that resolves another.
      3. **cameras**, serial unless `concurrency.cameras` — see `Concurrency`
         for why that default is off.

    Raises whatever the SDK raises, unchanged. Callers that need a per-device
    verdict instead of fail-fast (recovery) drive `create_arm` themselves.
    """
    brought = BroughtUp()
    started = time.perf_counter()

    # --- stage 1: arms and device components, together ---------------------
    #
    # Built as a task list first so the concurrent and serial paths are the same
    # code with one flag between them, rather than two traversals that can drift.
    stage1: list[Callable[[], Any]] = []

    if want.arms:
        for arm_id, arm_cfg in cfg.hardware.arms.items():
            arm_json = arm_cfg.to_json()
            if arm_overrides:
                arm_json.update(arm_overrides)
            stage1.append(
                # Bind the loop variables as defaults: a closure over `arm_id`
                # would see whatever the last iteration left behind, and every
                # task would open the same arm.
                lambda aid=arm_id, js=arm_json: (
                    "arm", aid, create_arm(aid, js, timings=brought.timings)
                )
            )

    if want.mobile_base and cfg.hardware.mobile_base is not None:
        # The legacy `hardware.base` slot. A decomposed config declares its base
        # as a component instead, which is why both paths exist.
        base_json = cfg.hardware.mobile_base.to_json()
        stage1.append(
            lambda js=base_json: (
                "mobile_base", "slate_base", _create_timed(
                    "slate_base", "slate_base", js,
                    "mobile_base slate_base", brought.timings,
                )
            )
        )

    device_cfgs = (
        [c for c in cfg.hardware.components if is_device_component(c)]
        if want.device_components else []
    )
    for comp_cfg in device_cfgs:
        stage1.append(
            lambda cc=comp_cfg: ("component", cc.id, _create_component(cc, brought))
        )

    results = _run_all(stage1, parallel=concurrency.devices, label="bringup-device")

    # Filed after joining, in config order, so the dicts and the summary read the
    # same regardless of which thread finished first.
    for kind, key, component in results:
        if kind == "arm":
            brought.arms[key] = component
        elif kind == "mobile_base":
            brought.mobile_base = component
        # "component" entries file themselves in _create_component, which also
        # records device-ness and session-control capability.

    # --- stage 2: teleop wiring, serial and in declared order --------------
    if want.wiring_components:
        for comp_cfg in cfg.hardware.components:
            if not is_device_component(comp_cfg):
                _create_component(comp_cfg, brought)

    # --- stage 3: cameras --------------------------------------------------
    if want.cameras:
        camera_tasks = [
            (lambda cc=cam_cfg: (
                cc.id, _create_timed(
                    cc.type, cc.id, cc.to_json(),
                    f"camera {cc.id} ({cc.type})", brought.timings,
                    # Marked active like everything else: nothing else holds a
                    # camera, so the ActiveHardwareRegistry is its only owner and
                    # clear() is what closes it while CUDA is still up. Skipping
                    # that leaves the close to static teardown, where a ZED fails
                    # with "cuCtxSetCurrent failed (error 4)".
                    mark_active=True,
                )
            ))
            for cam_cfg in cfg.hardware.cameras
        ]
        opened = _run_all(
            camera_tasks, parallel=concurrency.cameras, label="bringup-camera"
        )
        for (cam_id, component), cam_cfg in zip(opened, cfg.hardware.cameras):
            brought.cameras[cam_id] = component
            brought.camera_cfgs[cam_id] = cam_cfg

    brought.wall_clock_s = time.perf_counter() - started
    return brought


def _create_timed(
    hw_type: str,
    hw_id: str,
    config: dict[str, Any],
    label: str,
    timings: list[tuple[str, float]],
    *,
    mark_active: bool = True,
) -> Any:
    """`HardwareRegistry.create` with the open timed under `label`."""
    with _timed(label, timings):
        return ts.HardwareRegistry.create(hw_type, hw_id, config, mark_active)


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
