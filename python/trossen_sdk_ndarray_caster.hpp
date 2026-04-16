#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <opencv2/core.hpp>

namespace pybind11 {
namespace detail {

template <>
struct type_caster<cv::Mat> {
  PYBIND11_TYPE_CASTER(cv::Mat, const_name("numpy.ndarray"));

  // Python -> C++
  bool load(handle src, bool) {
    if (!pybind11::isinstance<pybind11::array>(src)) return false;

    auto arr = pybind11::array::ensure(src);
    if (!arr) return false;

    auto ndim = arr.ndim();
    if (ndim != 2 && ndim != 3) return false;

    int rows = static_cast<int>(arr.shape(0));
    int cols = static_cast<int>(arr.shape(1));

    auto dtype = arr.dtype();
    int cv_type;

    if (ndim == 2) {
      if (dtype.is(pybind11::dtype::of<uint8_t>())) {
        cv_type = CV_8UC1;
      } else if (dtype.is(pybind11::dtype::of<uint16_t>())) {
        cv_type = CV_16UC1;
      } else if (dtype.is(pybind11::dtype::of<float>())) {
        cv_type = CV_32FC1;
      } else {
        return false;
      }
    } else {
      int channels = static_cast<int>(arr.shape(2));
      if (dtype.is(pybind11::dtype::of<uint8_t>())) {
        cv_type = CV_MAKETYPE(CV_8U, channels);
      } else {
        return false;
      }
    }

    // Create cv::Mat header pointing at numpy data, then clone to own data
    auto cont = pybind11::array::ensure(src, pybind11::array::c_style);
    if (!cont) return false;

    value = cv::Mat(rows, cols, cv_type, const_cast<void*>(cont.data())).clone();
    return true;
  }

  // C++ -> Python
  static handle cast(const cv::Mat& src, return_value_policy, handle) {
    if (src.empty()) return pybind11::none().release();

    cv::Mat contiguous;
    if (src.isContinuous()) {
      contiguous = src;
    } else {
      contiguous = src.clone();
    }

    std::string format;
    switch (contiguous.depth()) {
      case CV_8U:  format = pybind11::format_descriptor<uint8_t>::format(); break;
      case CV_16U: format = pybind11::format_descriptor<uint16_t>::format(); break;
      case CV_32F: format = pybind11::format_descriptor<float>::format(); break;
      default:
        throw pybind11::type_error("Unsupported cv::Mat depth for numpy conversion");
    }

    std::vector<ssize_t> shape;
    std::vector<ssize_t> strides;

    if (contiguous.channels() == 1) {
      shape = {contiguous.rows, contiguous.cols};
      strides = {
        static_cast<ssize_t>(contiguous.step[0]),
        static_cast<ssize_t>(contiguous.elemSize())
      };
    } else {
      shape = {contiguous.rows, contiguous.cols, contiguous.channels()};
      strides = {
        static_cast<ssize_t>(contiguous.step[0]),
        static_cast<ssize_t>(contiguous.elemSize()),
        static_cast<ssize_t>(contiguous.elemSize1())
      };
    }

    // Use a capsule to prevent the Mat data from being freed
    auto* mat_ptr = new cv::Mat(contiguous);
    auto capsule = pybind11::capsule(mat_ptr, [](void* p) {
      delete static_cast<cv::Mat*>(p);
    });

    return pybind11::array(pybind11::dtype(format), shape, strides,
                           mat_ptr->data, capsule).release();
  }
};

}  // namespace detail
}  // namespace pybind11
