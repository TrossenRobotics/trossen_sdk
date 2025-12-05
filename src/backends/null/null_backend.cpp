/**
 * @file null_backend.cpp
 * @brief Implementation of NullBackend.
 */

#include "trossen_sdk/io/backends/null/null_backend.hpp"
#include "trossen_sdk/io/backend_registry.hpp"

namespace trossen::io::backends {

REGISTER_BACKEND(NullBackend, "null")

NullBackend::NullBackend(
  const ProducerMetadataList&)
  : Backend() {
  // Load configuration
  cfg_ = GlobalConfig::instance().get_as<NullBackendConfig>("null_backend");
  if (!cfg_) {
        std::cerr << "Backend config not found!" << std::endl;
        return;
  }
  // Print the stored values
  std::cout << "================= Null Backend Config =================" << std::endl;
  std::cout << "URI: " << cfg_->uri << std::endl;
  std::cout << "======================================================" << std::endl;

  }

bool NullBackend::open() {
  opened_ = true;
  return true;
}

void NullBackend::write(const data::RecordBase& record) {
  (void)record;
  count_.fetch_add(1);
}

void NullBackend::write_batch(std::span<const data::RecordBase* const> records) {
  count_.fetch_add(records.size());
}

void NullBackend::flush() {
  // No-op
}

void NullBackend::close() {
  opened_ = false;
}

uint64_t NullBackend::count() const {
  return count_.load();
}

}  // namespace trossen::io::backends
