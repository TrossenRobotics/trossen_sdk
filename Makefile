# Variables
BUILD_DIR := build
JOBS := $(shell nproc)
ARGS :=

# Default target
all: configure build

# Configure CMake
configure:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake ..

# Build project
build:
	@cd $(BUILD_DIR) && make -j$(JOBS)

# Install (optional)
install:
	@cd $(BUILD_DIR) && sudo make install

# Clean build directory
clean:
	@rm -rf $(BUILD_DIR)

# Run an executable (e.g., ./build/scripts/record)

run-%: 
	@chmod +x $(BUILD_DIR)/$*
	@$(BUILD_DIR)/$* $(ARGS)

.PHONY: all configure build install clean run-%
