/**
 * @file video_encoder_stub.cpp
 * @brief VideoEncoder definitions for builds without TROSSEN_ENABLE_VIDEO_ENCODE.
 *
 * TrossenMCAPBackend owns its encoders through std::unique_ptr<VideoEncoder> in every build, so
 * the destructor must exist even when FFmpeg is absent. No encoder is ever constructed in this
 * configuration (every call site is compiled out), so Impl is empty and only the destructor is
 * defined.
 */

#include "trossen_sdk/utils/video_encoder.hpp"

namespace trossen::utils {

struct VideoEncoder::Impl {};

VideoEncoder::~VideoEncoder() = default;

}  // namespace trossen::utils
