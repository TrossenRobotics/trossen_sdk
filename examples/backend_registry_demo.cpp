/**
 * @file backend_registry_demo.cpp
 * @brief Simple demo of the BackendRegistry system
 *
 * Shows how to create backends at runtime without hardcoded type checking.
 */

#include <iostream>
#include <memory>

#include "trossen_sdk/trossen_sdk.hpp"

int main() {
  // List registered backends
  std::cout << "Registered backends:\n";
  for (const auto& type : {"null", "mcap", "lerobot", "trossen"}) {
    if (trossen::io::BackendRegistry::is_registered(type)) {
      std::cout << "  Is Registered: " << type << "\n";
    }
  }
  std::cout << "\n";

  // Create and use a backend through the registry
  std::cout << "Creating null backend...\n";
  trossen::io::backends::NullBackend::Config cfg;
  cfg.type = "null";

  auto backend = trossen::io::BackendRegistry::create("null", cfg);
  backend->open();

  // Write some data
  trossen::data::JointStateRecord record;
  record.ts = trossen::data::make_timestamp_now();
  record.seq = 0;
  record.id = "demo/joints";
  record.positions = {0.1f, 0.2f, 0.3f};

  backend->write(record);
  backend->flush();
  backend->close();

  std::cout << "Successfully wrote data through registry-created backend!\n\n";

  return 0;
}
