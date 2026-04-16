/**
 * @file trossen_sdk.cpp
 * @brief Python bindings for the Trossen SDK (pybind11).
 *
 * Binding sections are added incrementally across the PR stack:
 *   1. Version constants  (this PR)
 *   2. Data types          (PR B)
 *   3. Hardware + producers (PR B)
 *   4. Config types         (PR B)
 *   5. Teleop + session     (PR C)
 *   6. Utilities            (PR C)
 */

#include <pybind11/pybind11.h>

// Version — must be included before trossen_slate headers which
// #define VERSION_MAJOR/MINOR/PATCH.
#include "trossen_sdk/version.hpp"

// Save version values before trossen_slate macros clobber the tokens.
namespace {
  constexpr uint32_t kSdkVersionMajor = trossen::core::VERSION_MAJOR;
  constexpr uint32_t kSdkVersionMinor = trossen::core::VERSION_MINOR;
  constexpr uint32_t kSdkVersionPatch = trossen::core::VERSION_PATCH;
}

namespace py = pybind11;

PYBIND11_MODULE(trossen_sdk, m) {
  m.doc() = "Trossen SDK Python bindings";

  // ── 1. Version ──────────────────────────────────────────────────────────
  m.def("version", &trossen::core::version, "Get SDK version string");
  m.attr("VERSION_MAJOR") = kSdkVersionMajor;
  m.attr("VERSION_MINOR") = kSdkVersionMinor;
  m.attr("VERSION_PATCH") = kSdkVersionPatch;
}
