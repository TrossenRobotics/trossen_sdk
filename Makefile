NPROC ?= 4

build:
	mkdir -p build
	cd build && cmake .. && make -j$(NPROC)
.PHONY: build

install: build
	cd build && make install
.PHONY: install

run-example: build
	./build/examples/widowxai
.PHONY: run-example

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
