/**
 * @file mock_joint_producer.cpp
 * @brief Implementation of synthetic joint state producer.
 */

#include <cmath>
#include <utility>

#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/data/timestamp.hpp"

namespace trossen::hw::arm {

TeleopMockJointStateProducer::TeleopMockJointStateProducer(Config cfg)
  : cfg_(std::move(cfg)) {}

void TeleopMockJointStateProducer::poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  if (!started_) {
    last_tick_ = now;
    started_ = true;
  }
  auto dt_ms = std::chrono::duration<double, std::milli>(now - last_tick_).count();
  last_tick_ = now;

  if (stats_.produced > 0) {
    diag_.avg_period_ms += (dt_ms - diag_.avg_period_ms) / static_cast<double>(stats_.produced);
    if (dt_ms > diag_.max_period_ms) diag_.max_period_ms = dt_ms;
    double expected_ms = 1000.0 / cfg_.rate_hz;
    if (dt_ms > expected_ms * 1.5) diag_.overruns++;
  }

  // Generate synthetic joint state
  std::vector<float>  act(cfg_.num_joints), obs(cfg_.num_joints);
  double t = (cfg_.rate_hz > 0.0) ? (static_cast<double>(stats_.produced) / cfg_.rate_hz) : static_cast<double>(stats_.produced);
  for (size_t i = 0; i < cfg_.num_joints; ++i) {
    double phase = t * 0.5 + static_cast<double>(i) * 0.1;
    act[i] = static_cast<float>(cfg_.amplitude * std::sin(phase));
    obs[i] = static_cast<float>(cfg_.amplitude * 0.5 * std::cos(phase));
  }
  uint64_t seq = stats_.produced; // sequential
  if (seq != (seq_)) { // seq_ tracks last emitted internally
    if (stats_.produced > 0 && seq != seq_ + 1) {
      diag_.gaps++;
    }
  }
  seq_ = seq;

  auto rec = std::make_shared<data::TeleopJointStateRecord>(
    data::make_timestamp_now(),
    seq,
    cfg_.id,
    act, obs);
  emit(rec);

  stats_.produced++;
}

} // namespace trossen::hw::arm
