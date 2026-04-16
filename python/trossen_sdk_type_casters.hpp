#pragma once

#include <chrono>

#include <pybind11/pybind11.h>
#include <nlohmann/json.hpp>

// ─── Helper functions (outside pybind11::detail) ─────────────────────────────

namespace trossen_pybind {

namespace py = pybind11;

inline py::object json_to_python(const nlohmann::json& j) {
  switch (j.type()) {
    case nlohmann::json::value_t::null:
      return py::none();
    case nlohmann::json::value_t::boolean:
      return py::bool_(j.get<bool>());
    case nlohmann::json::value_t::number_integer:
      return py::int_(j.get<int64_t>());
    case nlohmann::json::value_t::number_unsigned:
      return py::int_(j.get<uint64_t>());
    case nlohmann::json::value_t::number_float:
      return py::float_(j.get<double>());
    case nlohmann::json::value_t::string:
      return py::str(j.get<std::string>());
    case nlohmann::json::value_t::array: {
      py::list lst;
      for (const auto& el : j) {
        lst.append(json_to_python(el));
      }
      return std::move(lst);
    }
    case nlohmann::json::value_t::object: {
      py::dict d;
      for (auto it = j.begin(); it != j.end(); ++it) {
        d[py::str(it.key())] = json_to_python(it.value());
      }
      return std::move(d);
    }
    default:
      return py::none();
  }
}

inline nlohmann::json python_to_json(const pybind11::handle& obj) {
  if (obj.is_none()) {
    return nullptr;
  }
  if (py::isinstance<py::bool_>(obj)) {
    return obj.cast<bool>();
  }
  if (py::isinstance<py::int_>(obj)) {
    return obj.cast<int64_t>();
  }
  if (py::isinstance<py::float_>(obj)) {
    return obj.cast<double>();
  }
  if (py::isinstance<py::str>(obj)) {
    return obj.cast<std::string>();
  }
  if (py::isinstance<py::dict>(obj)) {
    nlohmann::json j = nlohmann::json::object();
    for (auto item : obj.cast<py::dict>()) {
      j[item.first.cast<std::string>()] = python_to_json(item.second);
    }
    return j;
  }
  if (py::isinstance<py::list>(obj) || py::isinstance<py::tuple>(obj)) {
    nlohmann::json j = nlohmann::json::array();
    for (auto item : obj) {
      j.push_back(python_to_json(item));
    }
    return j;
  }
  throw py::type_error("Cannot convert Python object to JSON");
}

}  // namespace trossen_pybind

// ─── pybind11 type casters ───────────────────────────────────────────────────

namespace pybind11 {
namespace detail {

template <>
struct type_caster<nlohmann::json> {
  PYBIND11_TYPE_CASTER(nlohmann::json, const_name("dict | list | str | int | float | bool | None"));

  bool load(handle src, bool) {
    try {
      value = ::trossen_pybind::python_to_json(src);
      return true;
    } catch (...) {
      return false;
    }
  }

  static handle cast(const nlohmann::json& src, return_value_policy, handle) {
    return ::trossen_pybind::json_to_python(src).release();
  }
};

template <>
struct type_caster<nlohmann::ordered_json> {
  PYBIND11_TYPE_CASTER(
    nlohmann::ordered_json,
    const_name("dict | list | str | int | float | bool | None"));

  bool load(handle src, bool) {
    try {
      auto j = ::trossen_pybind::python_to_json(src);
      value = nlohmann::ordered_json(j);
      return true;
    } catch (...) {
      return false;
    }
  }

  static handle cast(const nlohmann::ordered_json& src, return_value_policy, handle) {
    nlohmann::json j = src;
    return ::trossen_pybind::json_to_python(j).release();
  }
};

// ─── std::chrono::duration<double> <-> Python float (seconds) ────────────────

template <>
struct type_caster<std::chrono::duration<double>> {
  PYBIND11_TYPE_CASTER(std::chrono::duration<double>, const_name("float"));

  bool load(handle src, bool) {
    if (!src) return false;
    try {
      value = std::chrono::duration<double>(src.cast<double>());
      return true;
    } catch (...) {
      return false;
    }
  }

  static handle cast(std::chrono::duration<double> src, return_value_policy, handle) {
    return pybind11::float_(src.count()).release();
  }
};

// ─── std::chrono::milliseconds <-> Python int (ms) ──────────────────────────

template <>
struct type_caster<std::chrono::milliseconds> {
  PYBIND11_TYPE_CASTER(std::chrono::milliseconds, const_name("int"));

  bool load(handle src, bool) {
    if (!src) return false;
    try {
      value = std::chrono::milliseconds(src.cast<int64_t>());
      return true;
    } catch (...) {
      return false;
    }
  }

  static handle cast(std::chrono::milliseconds src, return_value_policy, handle) {
    return pybind11::int_(src.count()).release();
  }
};

}  // namespace detail
}  // namespace pybind11
