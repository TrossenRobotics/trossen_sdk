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

- [ ] Retry the open with backoff
- [ ] Close cameras on every exit path

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

**Fix.** Retry the open on the transient codes with backoff (and widen the
bootstrap budget to match, or the parent SIGKILLs the child mid-retry). Plus an
explicit `close()` that runs on SIGTERM instead of only on destruction.

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
