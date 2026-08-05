# Rig Bring-up — native build on a Jetson Orin

Getting `run-native.sh` to build and serve on a fresh Jetson. This is the
**native** path (one uvicorn process, system-installed libraries); for the Docker
path see [README.md](README.md).

Verified 2026-08-05 on an AGX Orin (`workbench-00`): Ubuntu 22.04.5, glibc 2.35,
GCC 11.4, cmake 3.22.1, CUDA 12.6, 12 cores, ZED X cameras.

Every entry below is a failure that actually happened on that bring-up, in the
order it happened.

---

## The build sits for minutes with no output

**Symptom.** `./run-native.sh --rivet --zed --no-realsense` prints
`first run compiles the SDK extension and takes several minutes` and then
nothing. It never errors and never finishes.

**Cause.** Not a slow compile — a *blocked* one. `--rivet` sets
`TROSSEN_ENABLE_RIVET=ON`, and when `trossen_base` is not installed CMake
falls back to `FetchContent` and clones it from a **private** repo over HTTPS.
Git wants credentials, has no TTY to ask on, and waits forever.

**Diagnose.** The giveaway is the load average. A real compile pins the cores; a
credential wait uses none:

```bash
uptime                       # 0.03 on a 12-core rig means blocked, not slow
ps -eo pid,ppid,etime,pcpu,stat,wchan:20,args --sort=-pcpu | head -20
```

Walk the chain to the leaf. It ends at
`git clone ... trossen_base.git` → `git-remote-https`, at 0% CPU.

**Fix.**

- On a Workbench or Solo Glide, **omit `--rivet`** — `run-native.sh --help` says
  so. Only a Rivet needs the mobile base.
- If you do need it, install `trossen_base` first (below). CMake then prints
  `using system trossen_base` and never reaches the clone.
- On every rig, make this class of hang impossible:

  ```bash
  echo 'export GIT_TERMINAL_PROMPT=0' >> ~/.bashrc
  ```

  An unauthenticated fetch then fails in a second with `Authentication failed`
  instead of hanging silently. Confirm a repo is genuinely private rather than
  the network being broken:

  ```bash
  GIT_TERMINAL_PROMPT=0 git ls-remote https://github.com/TrossenRobotics/trossen_base.git
  ```

Note `find_package(libtrossen_arm REQUIRED)` sits *after* the Rivet block in
`CMakeLists.txt`, so a configure that stalls on `trossen_base` has not yet
checked the arm driver at all — don't conclude anything about it from this
failure.

---

## Do not build libtrossen_arm — CI already did

**Symptom.** Building the arm driver from source takes 30–45 minutes. It vendors
pinocchio, which is what costs the time.

**Cause.** Unnecessary. The repo's CI builds aarch64 natively on a self-hosted
runner and uploads a per-arch artifact on every push and PR.

**Fix.** Take the artifact for the commit you want:

```bash
gh run list -R TrossenRobotics/trossen_arm-source --branch actuate-demo --limit 5
# confirm the run's head_sha is the commit you want:
gh api repos/TrossenRobotics/trossen_arm-source/actions/runs/<RUN_ID> \
  --jq '.head_sha, .conclusion'
gh api repos/TrossenRobotics/trossen_arm-source/actions/runs/<RUN_ID>/artifacts \
  --jq '.artifacts[] | "\(.name) expired=\(.expired)"'
gh run download <RUN_ID> -R TrossenRobotics/trossen_arm-source \
  -n libtrossen_arm-linux-arm64
```

Always confirm the architecture before shipping it anywhere — the wrong one does
not fail on install, it fails much later as baffling undefined references:

```bash
mkdir x && cd x && ar x ../libtrossen_arm.a "$(ar t ../libtrossen_arm.a | head -1)"
file ./*.o          # expect: ELF 64-bit LSB relocatable, ARM aarch64
```

A complete install needs pieces from **three** places, because the artifact
alone is not enough:

| Installed path | Comes from |
| --- | --- |
| `/usr/local/lib/libtrossen_arm.a` | the CI arm64 artifact (pinocchio is bundled in) |
| `/usr/local/include/libtrossen_arm/trossen_arm.hpp`, `trossen_arm_type.hpp` | the source tree at that same commit |
| `/usr/local/include/libtrossen_arm/trossen_arm_config.hpp` | generated at build time; CI ships it beside the archive |
| `/usr/local/lib/cmake/libtrossen_arm/libtrossen_arm{Config,ConfigVersion}.cmake` | **hand-maintained** |

That last row is load-bearing and easy to lose: upstream's install target ships
**no** CMake package config, so `find_package(libtrossen_arm)` cannot work
without it. It also hardcodes absolute `/usr/local` paths, so that prefix is not
optional. Never delete it while "cleaning up" an install.

**Which commit.** `setup.sh` pins
`TROSSEN_ARM_SOURCE_REF=6cc9cc6c3a30c3ada159c3f1904db4bed763f3bf` (tip of
`actuate-demo`) and explains why: the released tarball omits
`TrossenArmDriver::get_input_report()`, without which Glide handle joysticks and
buttons cannot be read at all. It also carries a 50 ms UDP timeout, up from 1 ms
which dropped teleop packets under ordinary jitter. Verify what you installed
actually has it:

```bash
grep -c InputReport /usr/local/include/libtrossen_arm/trossen_arm_type.hpp   # expect 1
```

`setup.sh` keys its own idempotency off that same grep, so a correct install
makes `bash setup.sh` skip the rebuild rather than repeat it.

---

## Is a 20.04-built archive safe on a 22.04 rig?

**Yes**, and the reasoning generalises: CI builds the artifact on `ubuntu:20.04`
while a JetPack 6 rig is 22.04.

**Why it works.** A static archive is a bag of relocatable `.o` files whose
undefined symbols are **unversioned** — version binding happens at final link
time against whatever libc/libstdc++ is on the machine doing the linking. So the
archive imposes no minimum glibc:

```bash
nm --undefined-only libtrossen_arm.a | grep -cE '@GLIBC|@GLIBCXX|@CXXABI'   # expect 0
nm libtrossen_arm.a | grep -c __cxx11    # non-zero: the modern std::string ABI
```

The `__cxx11` count matters most. That is the `_GLIBCXX_USE_CXX11_ABI=1`
`std::string`/`std::list` ABI, and mixing it with the old one is the classic way
cross-distro C++ breaks. GCC 9 (20.04) and GCC 11 (22.04) both default to it.

libstdc++ is backward compatible, so **older objects on a newer runtime** is the
supported direction. The reverse — building on 22.04 and deploying to 20.04 —
is what fails.

**Prove it rather than trusting it.** This is a 30-second test on the rig, and
static linking is all-or-nothing, so there is no scenario where it half-works and
bites you later:

```bash
cat > /tmp/abi.cpp <<'EOF'
#include "libtrossen_arm/trossen_arm.hpp"
int main() { trossen_arm::TrossenArmDriver d; d.get_input_report(); }
EOF
g++ -std=c++17 -I/usr/local/include /tmp/abi.cpp /usr/local/lib/libtrossen_arm.a \
    -lpthread -o /tmp/abi && echo LINK OK
objdump -T /tmp/abi | grep -oE 'GLIBCXX_[0-9.]+|GLIBC_[0-9.]+' | sort -Vu | tail -4
```

Deliberately call `get_input_report()`: it links only if the archive carries the
Glide API. Observed on the Orin: the binary needed at most `GLIBCXX_3.4.29` /
`GLIBC_2.34`, and the rig provides `3.4.30` / `2.35`.

`libtrossen_arm.a` also needs **no Boost** at link time — Boost appears in
`setup.sh` only to *build* pinocchio from source, which this path skips.

---

## trossen_base: no CI, and the rig cannot clone it

**Symptom.** `find_package(trossen_base)` fails, and cloning it on the rig hangs
or is denied.

**Cause.** Two separate things. `trossen_base` has **zero** GitHub Actions
workflows, so unlike the arm driver there is no artifact to download. And it is
private, while a rig has no GitHub credentials.

**Fix.** Clone on a machine that *does* have credentials, push the source over,
and build on the rig. Nothing secret ends up on the rig.

```bash
# on the laptop
git clone --depth 1 --branch dev git@github.com:TrossenRobotics/trossen_base.git /tmp/tb
rsync -a --exclude .git /tmp/tb/ workbench-00:~/trossen_base/

# on the rig — DEMO OFF avoids a hard SDL2 dependency
cmake -S ~/trossen_base -B ~/trossen_base/build \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_CONTROLLER_DEMO=OFF
cmake --build ~/trossen_base/build -j"$(nproc)"
sudo cmake --install ~/trossen_base/build
```

It is small: no fetched dependencies, ~3 seconds on an AGX Orin. It installs its
own working CMake package config (unlike the arm driver), so
`find_package(trossen_base)` then resolves and the FetchContent clone is skipped.

Preview an install without root using `DESTDIR=~/stage cmake --install <build>`.

---

## Configure fails: `No package 'protobuf' found`

**Symptom.**

```
-- Checking for module 'protobuf'
--   No package 'protobuf' found
CMake Error at .../FindPkgConfig.cmake:603 (message): A required package was not found
```

**Cause.** A fresh Jetson has protobuf's **runtime** (`libprotobuf23`) but not
its dev package, so there is no `protobuf.pc` and no `protoc`.

**Fix.** Check every `REQUIRED` dependency at once rather than discovering them
one failed configure at a time:

```bash
grep -nE 'find_package\([A-Za-z0-9_]+ .*REQUIRED|pkg_check_modules\([A-Za-z0-9_]+ REQUIRED' CMakeLists.txt
```

On a stock JetPack 6 rig, four are typically missing. Arrow and Parquet are
**not** in jammy's archive and need Apache's own repo:

```bash
sudo apt-get install -y --no-install-recommends \
    libprotobuf-dev protobuf-compiler libbz2-dev

wget -qO /tmp/aa.deb \
  https://packages.apache.org/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-jammy.deb
sudo apt-get install -y --no-install-recommends /tmp/aa.deb && sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends libarrow-dev libparquet-dev && rm -f /tmp/aa.deb
```

Usually already present and not worth reinstalling: `zlib1g-dev`, OpenCV
(JetPack ships 4.8.0 with a working `opencv4.pc`), Boost headers, ZED, CUDA.
`setup.sh` installs the full documented set if you would rather not hand-pick —
`bash setup.sh --no-realsense --no-webapp` — but it also builds the SDK *without*
ZED, so `run-native.sh` then redoes that work.

If apt proposes upgrading or removing protobuf to satisfy Arrow, stop and look:
jammy ships protobuf 3.12.4 and a current Arrow may want newer.

### Known defect: the protobuf fallback is unreachable

`CMakeLists.txt` (~line 208) reads:

```cmake
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(Protobuf REQUIRED protobuf)   # hard-errors here
else()
  find_package(Protobuf REQUIRED)                 # only if pkg-config is absent
```

The comment calls the second branch a fallback, but it only runs when
*pkg-config itself* is missing — not when protobuf's `.pc` file is. A machine
with protobuf installed via CMake config and no `.pc` fails configure even
though `find_package(Protobuf)` would have found it. Installing
`libprotobuf-dev` sidesteps it, so it stays latent.

---

## `no frontend/dist — API only`

**Symptom.** The backend serves the API but `GET /` returns 404.

**Cause.** `frontend/dist/` is gitignored, so **`git pull` never updates the
UI**. A rig with no Node toolchain has no way to produce it locally.

**Fix.** The bundle is plain JS/CSS and architecture-independent: build it
anywhere, copy the directory over.

Building on the host may fail — vite 8 needs Node ≥ 20 and the host may have 18,
with an empty `node_modules` because dependencies live in an anonymous Docker
volume. Build inside the frontend container instead; `webapp/frontend` is
bind-mounted, so the output lands on the host:

```bash
docker exec trossen_webapp_frontend sh -c \
  'cd /app/webapp/frontend && ./node_modules/.bin/vite build'

rsync -a --delete webapp/frontend/dist/ workbench-00:~/trossen_sdk/webapp/frontend/dist/
```

Use `--delete` so a stale hashed bundle is not left behind. The tree is ~46 MB,
almost entirely the Rerun WASM viewer.

**Then restart `run-native.sh`.** The static mount is decided at startup, so a
`dist/` that appears afterwards is ignored and `GET /` keeps returning 404.
Confirm the served bundle is the one you just built:

```bash
grep -o 'index-[A-Za-z0-9_-]*\.js' ~/trossen_sdk/webapp/frontend/dist/index.html
```

A pull that brings a backend fix but leaves an old bundle reads as "the fix
didn't work" rather than "the fix isn't deployed". After any frontend change,
repeat this.

---

## `Unsupported hardware type: 'zed_camera'`

**Symptom.** A ZED camera fails to open even though the config is correct.

**Cause.** The SDK extension was built without ZED support. The standard Docker
backend image has no ZED, so this is expected there — it needs the
`docker-compose.zed.yml` overlay.

**Fix.** Build native with `--zed`, then confirm registration. The registry
prints its types on import:

```bash
cd ~/trossen_sdk/webapp/backend
./.venv-native/bin/python -c 'import trossen_sdk' 2>&1 | grep -i registered
# expect: Registered push producer type: zed_camera
```

Do **not** probe with `HardwareRegistry.create(...)` — that instantiates real
hardware and blocks (a `trossen_base` probe will try to bring up CAN).

Newly connected cameras may not be detected until the rig is rebooted. ZED X
units are GMSL rather than USB, so `lsusb` will not show them; check that
`ZEDX_Daemon` is running instead.

---

## The rig keeps disappearing from mDNS

**Symptom.** `workbench-00.local` stops resolving mid-session, repeatedly.

**Cause.** Check the obvious thing first — `uptime`. On this bring-up the
apparent mDNS flapping was the rig **rebooting**, not a name-resolution fault.
`avahi-daemon` was `enabled` and `active` throughout.

**Fix.** If avahi really is down, `sudo systemctl enable --now avahi-daemon`
makes it survive reboots. Enrolling the rig in the tailnet removes the
dependency on mDNS altogether for shells and the web UI.
