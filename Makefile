NPROC ?= 4

build:
	mkdir -p build
	cd build && cmake .. && make -j$(NPROC)
.PHONY: build

build/all:
	mkdir -p build
	cd build && cmake .. -DBUILD_APPS=ON && make -j$(NPROC) all
.PHONY: build/all

install: build
	cd build && make install
.PHONY: install

run: build/all
	./build/apps/soma/soma
.PHONY: run

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
	cd build && cmake -DTROSSEN_ENABLE_REALSENSE=ON .. && make -j4
.PHONY: realsense

zed:
	mkdir -p build
	cd build && cmake -DTROSSEN_ENABLE_ZED=ON .. && make -j$(NPROC)
.PHONY: zed
