NPROC ?= 4
DOCS_DIR ?= docs
DOCS_BUILD_DIR ?= $(DOCS_DIR)/_build
DOCS_PORT ?= 8000
PYTHON ?= python3
PIP ?= $(PYTHON) -m pip

build:
	mkdir -p build
	cd build && cmake .. && make -j$(NPROC)
.PHONY: build

install: build
	cd build && make install
.PHONY: install

test:
	mkdir -p build
	cd build && cmake -DBUILD_TESTING=ON .. && ctest --output-on-failure
.PHONY: test

test-verbose:
	mkdir -p build
	cd build && cmake -DBUILD_TESTING=ON .. && make -j$(NPROC)
.PHONY: test-verbose

docker-build:
	docker build -t trossen-sdk:latest .
.PHONY: docker-build

clean:
	rm -rf build output
.PHONY: clean

realsense:
	mkdir -p build
	cd build && cmake -DTROSSEN_ENABLE_REALSENSE=ON .. && make -j$(NPROC)
.PHONY: realsense

# --- NVIDIA Jetson Orin (aarch64) ------------------------------------------
#
# The dependency graph is arm64-clean: libtrossen_arm and trossen_base build
# from source, trossen_slate ships an aarch64 prebuilt, the Foxglove SDK has an
# aarch64 release asset, and both the Apache Arrow and RealSense apt repos
# publish arm64. So these targets differ from a desktop build only in job count
# and in their flag defaults: Rivet and ZED on, RealSense off.
#
# RealSense defaults OFF here while the SDK-wide default is ON, which is a
# deliberate divergence. Every Orin we flash drives a Glide rig, and rivet and
# workbench declare ZED cameras exclusively -- so the SDK default would make each
# of these targets fail on a REQUIRED find_package for a camera backend the robot
# never opens. Opt back in with `make orin REALSENSE=1` on a rig that does have
# RealSense cameras.
#
# Prerequisites (see setup.sh, which installs all of these). `orin-check`
# verifies every one of them, at the severity CMake actually assigns it:
#   - libtrossen_arm installed                     REQUIRED  -> hard error
#   - ...with the input-report API (Glide handles)  optional  -> warning
#   - trossen_base installed, when RIVET=1          optional  -> warning
#   - libboost-filesystem-dev + libboost-serialization-dev    -> hard error
#     (pinocchio, vendored by libtrossen_arm, needs them)
#   - ZED SDK for Jetson under $(ZED_DIR), when ZED=1         -> hard error
#   - realsense2 + fastcdr + fastrtps, only if REALSENSE=1    -> hard error
#     (not checked by default, since REALSENSE defaults 0 here)
#
# The two warnings are warnings on purpose: both configurations build and are
# legitimate. Missing input-report costs Glide handle input at RUNTIME only, and
# a missing trossen_base falls back to a FetchContent clone. Neither shows up as
# a build failure, which is exactly why they are surfaced here instead.
#
# RealSense, when asked for, is a hard error instead: its find_package calls are
# REQUIRED, so there is no degraded mode to warn about -- configure simply stops.
#
# Job count is derived, not $(NPROC): a Jetson has 6-12 cores but as little as
# 8 GB shared with the GPU, and Eigen/pinocchio translation units are measured
# in GB. One job per GB of RAM, capped at the core count, is the same rule
# setup.sh uses and is what keeps the compile from being OOM-killed — which
# surfaces as an opaque "compilation failed" several minutes in.
ORIN_MEM_GB := $(shell awk '/MemTotal/{printf "%d", $$2/1024/1024}' /proc/meminfo 2>/dev/null || echo 4)
ORIN_CORES  := $(shell nproc 2>/dev/null || echo 4)
ORIN_JOBS   ?= $(shell echo $$(( $(ORIN_MEM_GB) < $(ORIN_CORES) ? $(ORIN_MEM_GB) : $(ORIN_CORES) )) )

ZED_DIR ?= /usr/local/zed
ZED ?= 1
RIVET ?= 1
# OFF unlike the SDK-wide default; see the RealSense note in the header above.
REALSENSE ?= 0
ORIN_BUILD_DIR ?= build

# Where an installed CMake package config can land. find_package searches
# <prefix>/lib/cmake/<name>, <prefix>/lib/<arch>/cmake/<name> and
# <prefix>/share/cmake/<name>, so cover those three under CMAKE_PREFIX_PATH and
# the two default prefixes rather than hardcoding the one path that happens to
# work here. This is a pre-flight heuristic, not CMake's own resolution: an
# install under an exotic prefix that is only reachable via -DCMAKE_PREFIX_PATH
# on the cmake line can still be missed, which is why a miss on trossen_base
# warns rather than fails.
ORIN_PKG_PREFIXES = $(subst :, ,$(CMAKE_PREFIX_PATH)) /usr/local /usr

# Fail early and legibly rather than part-way through a long compile.
orin-check:
	@arch=$$(uname -m); \
	if [ "$$arch" != "aarch64" ] && [ "$$arch" != "arm64" ]; then \
		echo "ERROR: 'orin' targets are for aarch64; this machine is $$arch."; \
		echo "       Use 'make build' (or 'make realsense') on a desktop."; \
		exit 1; \
	fi
	@if [ "$(ZED)" = "1" ] && [ ! -d "$(ZED_DIR)" ]; then \
		echo "ERROR: ZED=1 but no ZED SDK at $(ZED_DIR)."; \
		echo "       Install the Jetson build of the ZED SDK, point ZED_DIR at it,"; \
		echo "       or build without it: make orin ZED=0"; \
		exit 1; \
	fi
	@for pkg in libboost-filesystem-dev libboost-serialization-dev; do \
		dpkg -s $$pkg >/dev/null 2>&1 || { \
			echo "ERROR: $$pkg is missing; pinocchio (vendored by libtrossen_arm)"; \
			echo "       fails configure without it. sudo apt-get install -y $$pkg"; \
			exit 1; }; \
	done
	@arm_prefix=""; \
	for p in $(ORIN_PKG_PREFIXES); do \
		for sub in lib/cmake lib/$$(uname -m)-linux-gnu/cmake share/cmake; do \
			if [ -d "$$p/$$sub/libtrossen_arm" ]; then arm_prefix="$$p"; break 2; fi; \
		done; \
	done; \
	if [ -z "$$arm_prefix" ]; then \
		echo "ERROR: libtrossen_arm is not installed (no CMake package config found)."; \
		echo "       The SDK does find_package(libtrossen_arm REQUIRED), so configure"; \
		echo "       fails immediately without it. Install the driver (see setup.sh),"; \
		echo "       then re-run. Note the driver and the arm firmware must agree on"; \
		echo "       major.minor, so match the version already flashed on the arms."; \
		echo "       Searched under: $(ORIN_PKG_PREFIXES)"; \
		exit 1; \
	fi; \
	echo "orin-check: libtrossen_arm found under $$arm_prefix"; \
	hdr="$$arm_prefix/include/libtrossen_arm/trossen_arm.hpp"; \
	if [ ! -f "$$hdr" ]; then \
		echo "WARNING: $$hdr is missing, so the input-report API could not be checked."; \
		echo "         An install with a CMake config but no headers is broken; expect"; \
		echo "         configure to fail on the first #include."; \
	elif ! grep -q "get_input_report" "$$hdr"; then \
		echo "WARNING: this libtrossen_arm has no input-report API."; \
		echo "         The build SUCCEEDS -- CMake only logs a status line -- but Glide"; \
		echo "         handle joystick and button input is unavailable AT RUNTIME, so on"; \
		echo "         a Rivet or Workbench the handles read as permanently idle and a"; \
		echo "         session never starts. Nothing about the build output says this,"; \
		echo "         which is why it is checked here."; \
		echo "         Fix: install a driver providing TrossenArmDriver::get_input_report()."; \
	else \
		echo "orin-check: libtrossen_arm has the input-report API (Glide handles OK)"; \
	fi
	@if [ "$(RIVET)" = "1" ]; then \
		base_prefix=""; \
		for p in $(ORIN_PKG_PREFIXES); do \
			for sub in lib/cmake lib/$$(uname -m)-linux-gnu/cmake share/cmake; do \
				if [ -d "$$p/$$sub/trossen_base" ]; then base_prefix="$$p"; break 2; fi; \
			done; \
		done; \
		if [ -z "$$base_prefix" ]; then \
			echo "WARNING: RIVET=1 but trossen_base is not installed."; \
			echo "         CMake falls back to cloning it, and that repo is PRIVATE: the"; \
			echo "         fetch needs git credentials on this machine, adds a long build"; \
			echo "         to every fresh build dir, and does not work under sudo or CI."; \
			echo "         Install trossen_base, or drop the base: make orin RIVET=0"; \
		else \
			echo "orin-check: trossen_base found under $$base_prefix"; \
		fi; \
	fi
	@if [ "$(REALSENSE)" = "1" ]; then \
		for pkgcfg in realsense2 fastcdr fastrtps; do \
			found=""; \
			for p in $(ORIN_PKG_PREFIXES); do \
				for sub in lib/cmake lib/$$(uname -m)-linux-gnu/cmake share/cmake; do \
					if [ -d "$$p/$$sub/$$pkgcfg" ]; then found="$$p"; break 2; fi; \
				done; \
			done; \
			if [ -z "$$found" ]; then \
				echo "ERROR: REALSENSE=1 but $$pkgcfg is not installed."; \
				echo "       The RealSense block does find_package(realsense2/fastcdr/fastrtps"; \
				echo "       REQUIRED), so configure fails before compiling anything."; \
				echo "       Install librealsense2-dev (and the FastDDS packages) from the"; \
				echo "       RealSense apt repo, which does publish arm64."; \
				echo "       Or, if this rig has no RealSense cameras -- a Rivet and a"; \
				echo "       Workbench are all ZED -- just drop it: make orin REALSENSE=0"; \
				exit 1; \
			fi; \
		done; \
		echo "orin-check: realsense2 + fastcdr + fastrtps found"; \
	fi
	@echo "orin-check OK: aarch64, $(ORIN_CORES) cores, $(ORIN_MEM_GB) GB RAM -> -j$(ORIN_JOBS)"
.PHONY: orin-check

orin: orin-check
	mkdir -p $(ORIN_BUILD_DIR)
	cd $(ORIN_BUILD_DIR) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DTROSSEN_ENABLE_RIVET=$(if $(filter 1,$(RIVET)),ON,OFF) \
		-DTROSSEN_ENABLE_ZED=$(if $(filter 1,$(ZED)),ON,OFF) \
		-DTROSSEN_ENABLE_REALSENSE=$(if $(filter 1,$(REALSENSE)),ON,OFF) \
		$(if $(filter 1,$(ZED)),-DZED_DIR=$(ZED_DIR),) \
		&& cmake --build . -j$(ORIN_JOBS)
.PHONY: orin

orin-install: orin
	cd $(ORIN_BUILD_DIR) && $(SUDO) cmake --install .
	@echo "Installed. If this is the first install, run: sudo ldconfig"
.PHONY: orin-install

# Python extension for the webapp backend, same flags as `orin`.
orin-python: orin-check
	mkdir -p $(ORIN_BUILD_DIR)
	cd $(ORIN_BUILD_DIR) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_PYTHON_BINDINGS=ON \
		-DTROSSEN_ENABLE_RIVET=$(if $(filter 1,$(RIVET)),ON,OFF) \
		-DTROSSEN_ENABLE_ZED=$(if $(filter 1,$(ZED)),ON,OFF) \
		-DTROSSEN_ENABLE_REALSENSE=$(if $(filter 1,$(REALSENSE)),ON,OFF) \
		$(if $(filter 1,$(ZED)),-DZED_DIR=$(ZED_DIR),) \
		&& cmake --build . -j$(ORIN_JOBS)
.PHONY: orin-python

docs-deps:
	@if [ -z "$$VIRTUAL_ENV" ] && [ -z "$$CONDA_PREFIX" ]; then \
		echo "ERROR: activate a venv or conda env first (no VIRTUAL_ENV or CONDA_PREFIX set)."; \
		echo "       Example: python -m venv .venv && source .venv/bin/activate"; \
		exit 1; \
	fi
	$(PIP) install -r $(DOCS_DIR)/requirements.txt
.PHONY: docs-deps

docs:
	$(PYTHON) -m sphinx -b html $(DOCS_DIR) $(DOCS_BUILD_DIR)/html
	@echo "Docs built: $(DOCS_BUILD_DIR)/html/index.html"
.PHONY: docs

docs-strict:
	$(PYTHON) -m sphinx -W --keep-going -b html $(DOCS_DIR) $(DOCS_BUILD_DIR)/html
.PHONY: docs-strict

docs-clean:
	rm -rf $(DOCS_BUILD_DIR)
.PHONY: docs-clean

docs-serve: docs
	@echo "Serving docs at http://localhost:$(DOCS_PORT)"
	$(PYTHON) -m http.server $(DOCS_PORT) --directory $(DOCS_BUILD_DIR)/html
.PHONY: docs-serve

python-build:
	mkdir -p build
	cd build && cmake -DBUILD_PYTHON_BINDINGS=ON .. && make -j$(NPROC)
.PHONY: python-build

python-install:
	$(PIP) install --no-build-isolation -e .
.PHONY: python-install

python-wheel:
	$(PIP) wheel --no-build-isolation -w dist .
.PHONY: python-wheel

python-test:
	@if [ -d python/tests ]; then \
		$(PYTHON) -m pytest python/tests/ -v; \
	else \
		echo "python-test: python/tests/ not present; nothing to run"; \
	fi
.PHONY: python-test

# --- webapp (optional Python+TS app under webapp/) -------------------------

webapp-backend-install:
	cd webapp/backend && uv sync
.PHONY: webapp-backend-install

webapp-backend:
	cd webapp/backend && uv run uvicorn app.main:app --reload
.PHONY: webapp-backend

webapp-frontend-install:
	cd webapp/frontend && npm install
.PHONY: webapp-frontend-install

webapp-frontend:
	cd webapp/frontend && npm run dev
.PHONY: webapp-frontend

webapp-frontend-build:
	cd webapp/frontend && npm run build
.PHONY: webapp-frontend-build

webapp-install: webapp-backend-install webapp-frontend-install
.PHONY: webapp-install
