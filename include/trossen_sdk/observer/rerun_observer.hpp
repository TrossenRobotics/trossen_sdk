/**
 * @file rerun_observer.hpp
 * @brief Concrete Observer that streams records to a ReRun viewer.
 */

#ifndef TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_
#define TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"
#include "opencv2/core.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/observer/observer_base.hpp"

// Forward-declare so the rerun headers stay out of the public include surface.
namespace rerun {
class RecordingStream;
}  // namespace rerun

namespace trossen::observer {

namespace detail {

/**
 * @brief Convert an 8-bit color image to a contiguous RGB byte buffer.
 *
 * Supported encodings: ``"rgb8"``, ``"bgr8"``, ``"mono8"``. Unknown encodings return an
 * empty vector.
 */
std::vector<uint8_t> mat_to_rgb_bytes(const cv::Mat& image, const std::string& encoding);

}  // namespace detail

/**
 * @brief Observer implementation that forwards records to a ReRun viewer.
 *
 * Owns one ``rerun::RecordingStream`` connected over gRPC. The worker dispatches each
 * record by concrete type:
 *
 *  - ``data::ImageRecord``       -> ``rerun::Image`` (and ``rerun::DepthImage`` if present)
 *  - ``data::JointStateRecord``  -> ``rerun::Scalars`` for positions / velocities / efforts
 *  - other record types are counted as skipped.
 *
 * A failed gRPC connect returns ``false`` from ``on_start()`` so the SessionManager can
 * mark the observer dead and continue recording.
 */
class RerunObserver : public ObserverBase {
public:
  /**
   * @brief Construct from a JSON configuration object.
   *
   * Expected fields:
   *  - ``type`` (string, required) - registry key ("rerun")
   *  - ``id`` (string, optional) - logging name; defaults to ``type``
   *  - ``rerun_url`` (string, optional) - gRPC URL of the ReRun viewer. Defaults to
   *    ``"rerun+http://127.0.0.1:9876/proxy"`` (matches rerun-cpp).
   *  - ``app_id`` (string, optional) - ReRun application id; defaults to ``"trossen_sdk"``.
   *  - ``subscriptions`` (array, required) - each entry must have ``record_id`` (string)
   *    and ``throttle_hz`` (positive number).
   *
   * @param cfg JSON object (the raw JSON parsed by ``ObserverConfig``).
   * @throws std::runtime_error on missing/invalid required fields.
   */
  explicit RerunObserver(const nlohmann::json& cfg);
  ~RerunObserver() override;

  /// ReRun application id passed to the recording stream constructor.
  const std::string& app_id() const noexcept { return app_id_; }

  /// gRPC URL of the connected viewer.
  const std::string& rerun_url() const noexcept { return rerun_url_; }

  /// Records the worker reached but did not log (unsupported encoding, record type,
  /// or depth scale). Lets operators detect a silently-empty viewer.
  uint64_t skipped_frames() const noexcept {
    return skipped_frames_.load(std::memory_order_relaxed);
  }

protected:
  /// Open the ReRun gRPC connection. Returns ``false`` on transport failure.
  bool on_start() override;

  /// Drop the ReRun recording stream.
  void on_stop() override;

private:
  /// Worker-thread dispatch entry point. Captured into each subscription's handler.
  void dispatch_(const std::string& record_id,
                 const std::shared_ptr<data::RecordBase>& rec);

  std::string rerun_url_;
  std::string app_id_;

  // Writes (on_start/on_stop) and reads (dispatch_ on the worker) never overlap in time
  // because ObserverBase::start joins-before-on_start and joins-before-on_stop; no
  // synchronization required.
  std::unique_ptr<rerun::RecordingStream> rec_;

  // Per-instance set of skip reasons already logged. The worker thread is the only writer
  // (dispatch_ runs on this observer's single worker), so no synchronisation is required.
  // Kept per-instance so two RerunObserver instances do not share a static and race.
  std::unordered_set<std::string> logged_skip_reasons_;

  std::atomic<uint64_t> skipped_frames_{0};
};

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__RERUN_OBSERVER_HPP_
