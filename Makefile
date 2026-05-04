NPROC ?= 4

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

python-build:
	mkdir -p build
	cd build && cmake -DBUILD_PYTHON_BINDINGS=ON .. && make -j$(NPROC)
.PHONY: python-build

python-install:
	pip install --no-build-isolation -e .
.PHONY: python-install

python-wheel:
	pip wheel --no-build-isolation -w dist .
.PHONY: python-wheel

python-test:
	python -m pytest python/tests/ -v
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
