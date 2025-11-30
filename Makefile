build:
	mkdir -p build
	cd build && cmake .. && make -j4
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
	cd build && cmake -DBUILD_TESTING=ON .. && make -j4
.PHONY: test-verbose

docker-build:
	docker build -t trossen-sdk:latest .
.PHONY: docker-build

clean:
	rm -rf build output
.PHONY: clean
