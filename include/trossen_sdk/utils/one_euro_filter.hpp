/**
 * @file one_euro_filter.hpp
 * @brief One-Euro adaptive low-pass filter for smoothing jittery signals.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace trossen::utils {

/**
 *  TODO: @schromya Check this implmentation and verify citation (this is claude generated)
 *  @brief Scalar one-Euro filter (Casiez, Godin & Vogel, "1â‚¬ Filter", 2012).
 *
 * An adaptive low-pass: it smooths harder (more filtering, more lag) when the
 * signal is nearly static, and relaxes (less filtering, less lag) as the
 * signal's rate of change grows. This makes it well suited for smoothing a
 * jittery position target — e.g. a teleop command arriving over a noisy
 * link — without adding perceptible lag during fast motion.
 */
class OneEuroFilter {
public:
  /**
   * @param min_cutoff Cutoff frequency (Hz) at zero speed; lower = smoother
   *   but laggier when the signal is nearly still.
   * @param beta Speed coefficient; higher = less smoothing (less lag) as the
   *   signal moves faster.
   * @param d_cutoff Cutoff frequency (Hz) for the derivative's own low-pass;
   *   rarely needs tuning.
   */
  explicit OneEuroFilter(double min_cutoff = 1.0, double beta = 0.0, double d_cutoff = 1.0)
  : min_cutoff_(min_cutoff), beta_(beta), d_cutoff_(d_cutoff) {}

  /// Drop all history; the next filter() call seeds the filter with its input
  /// instead of computing a derivative against stale state.
  void reset() {
    has_prev_ = false;
    dx_prev_ = 0.0;
  }

  /**
   * @brief Filter one sample.
   *
   * @param x Raw sample.
   * @param t_s Monotonic sample time (seconds). Only the difference between
   *   successive calls matters, so any monotonic clock epoch works.
   * @return Filtered value.
   */
  double filter(double x, double t_s) {
    if (!has_prev_) {
      x_prev_ = x;
      t_prev_ = t_s;
      has_prev_ = true;
      return x;
    }
    double dt = t_s - t_prev_;
    if (dt <= 0.0) {
      dt = 1e-3;
    }

    const double dx = (x - x_prev_) / dt;
    const double a_d = alpha(d_cutoff_, dt);
    const double dx_hat = a_d * dx + (1.0 - a_d) * dx_prev_;

    const double cutoff = min_cutoff_ + beta_ * std::fabs(dx_hat);
    const double a = alpha(cutoff, dt);
    const double x_hat = a * x + (1.0 - a) * x_prev_;

    x_prev_ = x_hat;
    dx_prev_ = dx_hat;
    t_prev_ = t_s;
    return x_hat;
  }

private:
  static double alpha(double cutoff, double dt) {
    const double tau = 1.0 / (2.0 * kPi * cutoff);
    return 1.0 / (1.0 + tau / dt);
  }

  static constexpr double kPi = 3.14159265358979323846;

  double min_cutoff_;
  double beta_;
  double d_cutoff_;

  bool has_prev_{false};
  double x_prev_{0.0};
  double dx_prev_{0.0};
  double t_prev_{0.0};
};

/**
 * @brief One-Euro filter applied independently to each element of a vector
 * (one scalar filter instance per element — elements never influence each
 * other's cutoff/derivative state).
 */
class VecOneEuroFilter {
public:
  VecOneEuroFilter() = default;

  explicit VecOneEuroFilter(
      size_t n, double min_cutoff = 1.0, double beta = 0.0, double d_cutoff = 1.0)
  : filters_(n, OneEuroFilter(min_cutoff, beta, d_cutoff)) {}

  void reset() {
    for (auto& f : filters_) {
      f.reset();
    }
  }

  /// Number of independent per-element filters.
  size_t size() const { return filters_.size(); }

  /// Filter `xs` in place. Only the first min(xs.size(), size()) elements are
  /// touched; a size mismatch is not treated as an error since callers may
  /// reuse a filter across command vectors that carry extra trailing fields.
  void filter(std::vector<double>& xs, double t_s) {
    const size_t n = std::min(xs.size(), filters_.size());
    for (size_t i = 0; i < n; ++i) {
      xs[i] = filters_[i].filter(xs[i], t_s);
    }
  }

private:
  std::vector<OneEuroFilter> filters_;
};

}  // namespace trossen::utils
