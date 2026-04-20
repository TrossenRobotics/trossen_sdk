NPROC ?= 4
DOCS_DIR ?= docs
DOCS_BUILD_DIR ?= $(DOCS_DIR)/_build
DOCS_PORT ?= 8000
PYTHON ?= python
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
