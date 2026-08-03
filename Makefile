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
# and in defaulting the Rivet/ZED flags on.
#
# Prerequisites (see setup.sh, which installs all of these):
#   - libtrossen_arm installed with the input-report API (Glide handles)
#   - trossen_base installed, or reachable for FetchContent
#   - libboost-filesystem-dev + libboost-serialization-dev (pinocchio needs them)
#   - ZED SDK for Jetson under $(ZED_DIR), when building with ZED=1
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
REALSENSE ?= 1
ORIN_BUILD_DIR ?= build

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
