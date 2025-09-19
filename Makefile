build:
	mkdir -p build
	cd build && cmake .. .. && make -j4
.PHONY: build

install: build
	cd build && make install
.PHONY: install

clean:
	rm -rf build
.PHONY: clean

run_example: build
	./build/examples/trossen_ai_solo
.PHONY: run_example
