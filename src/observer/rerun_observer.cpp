
/**
 * @file rerun_observer.cpp
 * @brief Implementation of RerunObserver.
 */

#include "trossen_sdk/observer/rerun_observer.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "opencv2/imgproc.hpp"
#include "rerun.hpp"

#include "trossen_sdk/observer/observer_registry.hpp"

namespace trossen::observer {

namespace {

constexpr const char* kDefaultRerunUrl = "rerun+http://127.0.0.1:9876/proxy";
constexpr const char* kDefaultAppId = "trossen_sdk";

}  // namespace

namespace detail {

ColorEncoding resolve_encoding(const std::string& encoding) {
  if (encoding == "rgb8") {
    return ColorEncoding::kRgb8;
  }
  if (encoding == "bgr8") {
    return ColorEncoding::kBgr8;
  }
  if (encoding == "mono8") {
    return ColorEncoding::kMono8;
  }
  return ColorEncoding::kUnsupported;
}

cv::Mat mat_to_rgb(const cv::Mat& image, ColorEncoding encoding) {
  cv::Mat rgb;
  // Exhaustive switch (no default) so adding a new ColorEncoding value triggers a
  // -Wswitch warning here rather than silently falling through to "unsupported".
  switch (encoding) {
    case ColorEncoding::kRgb8:
      // rgb8 requires a 3-channel 8-bit source; a single-channel Mat would otherwise be
      // shipped as garbled RGB pixels with the wrong byte count.
      if (image.type() != CV_8UC3) {
        return {};
      }
      // For kRgb8 we return the source Mat unchanged (a shallow header copy that shares
      // the same refcounted pixel storage). The caller borrows ``.data`` into rerun's
      // Image ctor, so a non-contiguous ROI input would produce a row-stepped view that
      // mis-samples bytes. Caller surfaces this as ``non_contiguous_rgb`` rather than
      // forcing a hidden cv::Mat::clone() copy here.
      if (!image.isContinuous()) {
        return {};
      }
      rgb = image;
      break;
    case ColorEncoding::kBgr8:
      if (image.type() != CV_8UC3) {
        return {};
      }
      // cv::cvtColor allocates a fresh dense ``rgb`` (always contiguous, CV_8UC3).
      cv::cvtColor(image, rgb, cv::COLOR_BGR2RGB);
      break;
    case ColorEncoding::kMono8:
      if (image.type() != CV_8UC1) {
        return {};
      }
      cv::cvtColor(image, rgb, cv::COLOR_GRAY2RGB);
      break;
    case ColorEncoding::kUnresolved:
    case ColorEncoding::kUnsupported:
      return {};
  }
  return rgb;
}

}  // namespace detail

RerunObserver::RerunObserver(const nlohmann::json& cfg)
  : ObserverBase(cfg.value("id", cfg.value("type", std::string("rerun")))) {
  rerun_url_ = cfg.value("rerun_url", std::string{kDefaultRerunUrl});
  app_id_ = cfg.value("app_id", std::string{kDefaultAppId});

  if (!cfg.contains("subscriptions") || !cfg.at("subscriptions").is_array()) {
    throw std::runtime_error(
      "RerunObserver: 'subscriptions' (array) is required");
  }
  const auto& subs = cfg.at("subscriptions");
  if (subs.empty()) {
    throw std::runtime_error("RerunObserver: 'subscriptions' must be non-empty");
  }
  for (const auto& sub_j : subs) {
    if (!sub_j.is_object() ||
        !sub_j.contains("record_id") ||
        !sub_j.at("record_id").is_string() ||
        !sub_j.contains("throttle_hz") ||
        !sub_j.at("throttle_hz").is_number()) {
      throw std::runtime_error(
        "RerunObserver: each subscription must have 'record_id' (string) and "
        "'throttle_hz' (number)");
    }
    const auto record_id = sub_j.at("record_id").get<std::string>();
    const auto throttle_hz = sub_j.at("throttle_hz").get<double>();
    add_subscription(record_id, throttle_hz,
                     [this, record_id](const std::shared_ptr<data::RecordBase>& rec) {
                       dispatch_(record_id, rec);
                     });
  }
}

RerunObserver::~RerunObserver() {
  // Call stop() in the derived dtor so the worker is joined while this object is still
  // a RerunObserver; the base dtor would not dispatch to our on_stop().
  try {
    stop();
  } catch (...) {
  }
}

bool RerunObserver::on_start() {
  try {
    rec_ = std::make_unique<rerun::RecordingStream>(app_id_);
    const auto err = rec_->connect_grpc(rerun_url_);
    if (err.is_err()) {
      std::cerr << "RerunObserver '" << name()
                << "' connect_grpc failed: " << err.description << std::endl;
      rec_.reset();
      return false;
    }
  } catch (const std::exception& e) {
    std::cerr << "RerunObserver '" << name() << "' on_start threw: "
              << e.what() << std::endl;
    rec_.reset();
    return false;
  } catch (...) {
    std::cerr << "RerunObserver '" << name() << "' on_start threw (unknown)"
              << std::endl;
    rec_.reset();
    return false;
  }
  return true;
}

void RerunObserver::on_stop() {
  rec_.reset();
}

void RerunObserver::dispatch_(const std::string& record_id,
                              const std::shared_ptr<data::RecordBase>& rec) {
  if (!rec_ || !rec) {
    return;
  }

  // Count every skip; log once per reason so a config mismatch surfaces a single line
  // instead of a flood. ``logged_skip_reasons_`` is a per-instance member; the worker
  // thread is the sole writer for this observer, so no synchronisation is required.
  const auto skip = [this, &record_id](const char* reason) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    if (logged_skip_reasons_.insert(reason).second) {
      try {
        std::cerr << "[observer:" << name() << "] skipping record '" << record_id
                  << "' (reason=" << reason << "); further skips counted in "
                  << "skipped_frames()" << std::endl;
      } catch (...) {
      }
    }
  };

  // Use the producer-side monotonic capture time so the timeline does not jump backwards
  // on system-clock adjustments.
  const double t_secs = static_cast<double>(rec->ts.monotonic.to_ns()) * 1e-9;
  rec_->set_time_duration_secs("monotonic", t_secs);

  if (auto* img = dynamic_cast<data::ImageRecord*>(rec.get())) {
    if (img->image.empty() || img->width == 0 || img->height == 0) {
      skip("empty_image");
      return;
    }
    // Guard against producers that fill width/height inconsistently with the cv::Mat
    // payload: rerun is told (width, height) explicitly and would otherwise mis-render
    // (or read off the end of the byte buffer).
    if (static_cast<uint32_t>(img->image.cols) != img->width ||
        static_cast<uint32_t>(img->image.rows) != img->height) {
      skip("dimension_mismatch");
      return;
    }
    // Lazy per-subscription state: encoding is resolved exactly once from the first
    // ImageRecord's ``encoding`` string. ``operator[]`` default-constructs the entry on
    // first sight (encoding=kUnresolved). After resolution we never re-parse the string
    // on subsequent frames, and a kUnsupported result sticks so further string churn is
    // impossible for that subscription.
    auto& state = subscription_state_[record_id];
    if (state.encoding == detail::ColorEncoding::kUnresolved) {
      state.encoding = detail::resolve_encoding(img->encoding);
    }
    const cv::Mat rgb = detail::mat_to_rgb(img->image, state.encoding);
    if (rgb.empty()) {
      // mat_to_rgb returns empty for two distinct failure modes: a known encoding whose
      // source cv::Mat is non-contiguous (only reachable for kRgb8 since the kBgr8 /
      // kMono8 paths run cv::cvtColor which always produces a dense output), or an
      // unsupported / type-mismatched encoding. Report them separately so a stride
      // mismatch does not silently masquerade as a config mismatch.
      if (state.encoding == detail::ColorEncoding::kRgb8 &&
          img->image.type() == CV_8UC3 && !img->image.isContinuous()) {
        skip("non_contiguous_rgb");
      } else {
        skip("unsupported_encoding");
      }
      return;
    }
    // SAFETY: rerun::archetypes::Image(const T*, WidthHeight, ColorModel) wraps the raw
    // pointer in a rerun::Collection<uint8_t>::borrow and immediately constructs an
    // arrow array via ComponentBatch::from_loggable -> rerun::components::ImageBuffer.
    // Traced against rerun-cpp 0.32 source (archetypes/image.hpp lines 263-270 ->
    // with_buffer/with_format) the Arrow copy is synchronous: by the time the ctor
    // returns, rerun no longer references rgb.data. We therefore only need rgb to
    // outlive the Image-ctor full-expression below, which it does (rgb lives on this
    // stack frame and is destroyed at the end of dispatch_). If a future rerun version
    // lazily references the borrowed buffer, the lifetime stress test in
    // tests/test_rerun_observer.cpp will catch it under ASan.
    rec_->log(
      record_id,
      rerun::archetypes::Image(
        rgb.data,
        rerun::WidthHeight{img->width, img->height},
        rerun::datatypes::ColorModel::RGB));

    if (img->has_depth() && img->depth_image.has_value() &&
        !img->depth_image->empty() && img->depth_scale.has_value()) {
      const float depth_scale = img->depth_scale.value();
      if (!(depth_scale > 0.0f) || !std::isfinite(depth_scale)) {
        skip("bad_depth_scale");
        return;
      }
      const cv::Mat& depth = img->depth_image.value();
      // reinterpret_cast<const uint16_t*> requires depth to be CV_16UC1 (the documented
      // payload format) and to be contiguous in memory - an ROI sub-image would be a row-
      // stepped view that aliasing as uint16_t* would silently mis-sample.
      if (depth.type() != CV_16UC1 || !depth.isContinuous()) {
        skip("unsupported_depth_format");
        return;
      }
      const auto width = static_cast<uint32_t>(depth.cols);
      const auto height = static_cast<uint32_t>(depth.rows);
      // rerun-cpp's with_meter expects depth units per meter; depth_scale is the
      // reciprocal (meters per unit).
      const float meter = 1.0f / depth_scale;
      if (!state.image_paths.has_value()) {
        state.image_paths = PerSubscriptionState::ImagePaths{record_id + "/depth"};
      }
      rec_->log(
        state.image_paths->depth,
        rerun::DepthImage(
          reinterpret_cast<const uint16_t*>(depth.data),
          {width, height})
          .with_meter(meter));
    }
    return;
  }

  if (auto* js = dynamic_cast<data::JointStateRecord*>(rec.get())) {
    auto& state = subscription_state_[record_id];
    if (!state.joint_paths.has_value()) {
      state.joint_paths = PerSubscriptionState::JointPaths{
        record_id + "/positions",
        record_id + "/velocities",
        record_id + "/efforts"};
    }
    const auto& jp = *state.joint_paths;
    if (!js->positions.empty()) {
      std::vector<double> pos(js->positions.begin(), js->positions.end());
      rec_->log(jp.positions, rerun::Scalars(std::move(pos)));
    }
    if (!js->velocities.empty()) {
      std::vector<double> vel(js->velocities.begin(), js->velocities.end());
      rec_->log(jp.velocities, rerun::Scalars(std::move(vel)));
    }
    if (!js->efforts.empty()) {
      std::vector<double> eff(js->efforts.begin(), js->efforts.end());
      rec_->log(jp.efforts, rerun::Scalars(std::move(eff)));
    }
    return;
  }

  if (auto* odom = dynamic_cast<data::Odometry2DRecord*>(rec.get())) {
    auto& state = subscription_state_[record_id];
    if (!state.odom_paths.has_value()) {
      state.odom_paths = PerSubscriptionState::OdomPaths{
        record_id + "/pose/x",
        record_id + "/pose/y",
        record_id + "/pose/theta",
        record_id + "/twist/linear_x",
        record_id + "/twist/linear_y",
        record_id + "/twist/angular_z"};
    }
    const auto& op = *state.odom_paths;
    rec_->log(op.pose_x, rerun::Scalars(static_cast<double>(odom->pose.x)));
    rec_->log(op.pose_y, rerun::Scalars(static_cast<double>(odom->pose.y)));
    rec_->log(op.pose_theta, rerun::Scalars(static_cast<double>(odom->pose.theta)));
    rec_->log(op.twist_lx, rerun::Scalars(static_cast<double>(odom->twist.linear_x)));
    rec_->log(op.twist_ly, rerun::Scalars(static_cast<double>(odom->twist.linear_y)));
    rec_->log(op.twist_wz, rerun::Scalars(static_cast<double>(odom->twist.angular_z)));
    return;
  }

  skip("unsupported_record_type");
}

REGISTER_OBSERVER(RerunObserver, "rerun");

}  // namespace trossen::observer
