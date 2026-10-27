/**
 * @file lerobot_worker_pool.cpp
 * @brief Implementation of the converter worker count default.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_worker_pool.hpp"

#include <algorithm>

namespace trossen::io::backends {

int default_jobs(unsigned hardware_threads) {
  return static_cast<int>(std::min(std::max(1u, hardware_threads), kMaxDefaultJobs));
}

}  // namespace trossen::io::backends
