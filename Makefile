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
