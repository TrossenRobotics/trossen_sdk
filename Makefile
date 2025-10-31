build:
	mkdir -p build
	cd build && cmake .. && make -j4
.PHONY: build

install: build
	cd build && make install
.PHONY: install

run_example: build
	./build/examples/trossen_ai_solo_mcap
.PHONY: run_example

test:
	mkdir -p build
	cd build && cmake -DBUILD_TESTING=ON .. && ctest --output-on-failure
.PHONY: test

test-verbose:
	mkdir -p build
	cd build && cmake -DBUILD_TESTING=ON .. && make -j4
.PHONY: test-verbose

clean:
	rm -rf build output
.PHONY: clean
