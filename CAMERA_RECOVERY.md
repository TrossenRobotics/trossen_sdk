# Camera recovery & session teardown

A crashed session used to block the next one: the ZED refused to open, and the
old session never closed. Both trace back to the same thing — nothing releases
the hardware when a recorder dies. This file tracks the fixes.

Evidence throughout is from rivet-01 and rivet-02, 2026-08-13.

---

## 1. `spd-say` wedges the session forever

- [x] Remove text-to-speech from the SDK

**Problem.** The recorder's stdout is a pipe. The parent reads it with
`for line in proc.stdout`, which ends only at EOF, and EOF needs *every* holder
of the write end to close it. `announce(msg, block=false)` double-forks an
orphaned `spd-say` that inherits that pipe — and `spd-say` blocks forever when
no speech server is running, which is the case on every rig:

```
PID 5628  PPID 1  ELAPSED 10:24   spd-say Episode 0 started
/proc/5227/fd/1 -> pipe:[80913]      # the recorder
pipe:[80913]  5628  spd-say          # still holding the write end
```

So the pump thread never wakes, `_finalize_crash` never runs, and the session
stays `active`/`paused` with its runner registered. `start_recording` then
refuses with *"already has a running recorder"* until uvicorn restarts. The
wedged recorder is also still holding the cameras, which is how this feeds
problem 2.

**Fix.** Deleted `announce()`, its four callers in `session_manager.cpp`, the
Python binding, its test, and the `speech-dispatcher` install in `setup.sh`. The
webapp already speaks through the browser's Web Speech API
(`frontend/src/lib/announce.ts`), so the SDK-side copy was redundant as well as
harmful. 437/437 tests pass.

---

## 2. ZED will not open after a crash

- [x] Retry the open with backoff
- [x] Close cameras when the recorder is asked to stop
- [x] Verified on rivet-02 (2026-08-13)
- [x] Deployed to rivet-01 (2026-08-13) — recovers, though the retry did not
      need to fire there; see below
- [ ] Same `close()` for the RealSense and OpenCV components

**Problem.** `sl::Camera::close()` is only called from `~ZedCameraComponent`, so
SIGKILL and `std::terminate` skip it. Argus keeps the camera allocated to the
dead client, and the next session's first open fails:

```
nvargus-daemon: WARNING: CameraProvider was not destroyed before client connection terminated
nvargus-daemon: (Argus) Error AlreadyAllocated: Device 0 (of 1) is in use
session.error_message: ... Failed to open ZED camera S/N 51287468: CAMERA STREAM FAILED TO START
```

That string is `sl::ERROR_CODE::CANNOT_START_CAMERA_STREAM = 28`, whose own
header note reads *"make sure your camera is not already used by another
process"*. Nothing retries, so one failed open fails the whole bootstrap. In one
window rivet-02 logged 6 opens and 0 closes.

**Fix**, in two halves — because one of them cannot cover every case.

*Give the camera back.* `HardwareComponent::close()` is now a virtual any
component can implement, `ZedCameraComponent` implements it, and the destructor
just calls it. The recorder handles SIGTERM: it aborts the in-flight episode,
lets the loop shut down normally, and releases every device in the registry —
with an 8s deadline that releases and exits anyway if the wind-down wedges. The
parent now sends SIGTERM and waits 12s before killing, where it used to SIGKILL
on sight of a `[critical]` line. A `terminate called` line is still killed
immediately: that process is already inside abort() and no handler will run.

*Survive one that wasn't given back.* SIGKILL and abort still exist, so
`configure()` retries the open 4 times at 2s intervals on the codes that clear
themselves (`CANNOT_START_CAMERA_STREAM`, `CAMERA_FAILED_TO_SETUP`,
`CAMERA_DETECTION_ISSUE`, `CAMERA_NOT_DETECTED`, `CAMERA_REBOOTING`) and fails
fast on the ones that never will. Both knobs are per-camera config
(`open_retries`, `open_retry_delay_s`). The bring-up budget grew by 8s per
camera to match — without that the parent kills the child in the middle of the
retry that would have worked, which reads as "the camera failed".

A `DRIVER_FAILURE` now says to restart nvargus-daemon, and a busy camera says
so in words rather than only as an error code.

**Verified on rivet-02**, 2026-08-13. `kill -9` on a recorder holding three
ZEDs, next session started the same second:

```
18:33:36  killed recorder pid=6750
18:33:38  camera_main (S/N 56066260) open failed: CAMERA NOT DETECTED — retrying in 2s (1/4)
18:33:45  camera_main opened: 1920x1200 @ 30 FPS
18:33:56  all three open, session active
```

One retry was enough. Note the code was `CAMERA_NOT_DETECTED`, not the
`CANNOT_START_CAMERA_STREAM` seen in the original failures — a camera mid-reap
can report either, so the retry set has to cover both. Retrying only the
originally-observed code would have failed this test.

The killed session also finalised at 18:33:36, in the same second as the kill,
as `error / exited with code -9`. That is the spd-say fix: EOF now arrives, so
the pump wakes and the session closes itself instead of hanging as `active`.

**rivet-01**, same test against the full config (4 arms + base + 3 ZEDs),
recovered with **no retry needed**: killed at 18:53:29, `camera_main opened` at
18:53:39. The difference is bootstrap order — arms connect before cameras, and
those ~10s are enough for Argus to finish reaping the dead client, so the race
never opens. A cameras-only config reaches `open()` about 2s in and lands
squarely in it. Worth remembering when reproducing this: **a config that brings
up arms first will often not show the bug at all**.

---

## 3. Sessions do not close on failure

- [ ] Don't depend on EOF to finalise
- [ ] Reconcile `paused` too, and treat exit code 0 as clean
- [ ] Close the runs stranded by earlier crashes

**Problem.** Three gaps, all downstream of problem 1:

- `reconcile_orphaned_sessions()` only scans `status == "active"`. A session
  that reached `paused` is invisible to it forever — 7 such on rivet-01, 3 on
  rivet-02, with 10 `session_run` rows still open between them.
- It calls `_finalize_crash` for any non-`None` exit code, including 0. Observed:
  `reconciled orphan 920ae1f8: process exited (code=0)` marked a clean run as
  `error`.
- `_runners.pop` lives in the pump's `finally`, which the wedge skips, so the
  stale entry blocks that session from ever starting again.

**Fix.** Poll for liveness rather than waiting on EOF; widen the reconciler and
distinguish a clean exit; pop the runner on every path.
