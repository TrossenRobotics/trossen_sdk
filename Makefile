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
