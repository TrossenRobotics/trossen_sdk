/**
 * @file cli_parser.cpp
 * @brief CLI argument parser implementation
 */

#include "trossen_sdk/configuration/cli_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace trossen::configuration {

CliParser::CliParser(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);

    // Check for flag (starts with --)
    if (arg.starts_with("--")) {
      std::string flag_name = arg.substr(2);

      // Check for --flag=value format
      size_t eq_pos = flag_name.find('=');
      if (eq_pos != std::string::npos) {
        std::string key = flag_name.substr(0, eq_pos);
        std::string value = flag_name.substr(eq_pos + 1);
        flags_[key] = value;

        // Special handling for --set key=value
        if (key == "set") {
          size_t set_eq_pos = value.find('=');
          if (set_eq_pos != std::string::npos) {
            set_overrides_.emplace_back(
              value.substr(0, set_eq_pos),
              value.substr(set_eq_pos + 1));
          }
        }
      } else {
        // Check if next arg is a value
        if (i + 1 < argc && argv[i + 1][0] != '-') {
          std::string value(argv[i + 1]);
          flags_[flag_name] = value;
          ++i;  // Skip next arg

          // Special handling for --set key=value
          if (flag_name == "set") {
            size_t set_eq_pos = value.find('=');
            if (set_eq_pos != std::string::npos) {
              set_overrides_.emplace_back(
                value.substr(0, set_eq_pos),
                value.substr(set_eq_pos + 1));
            }
          }
        } else {
          // Boolean flag
          flags_[flag_name] = "true";
        }
      }
    } else {
      // Positional argument
      positional_.push_back(arg);
    }
  }
}

bool CliParser::has_flag(const std::string& flag) const {
  return flags_.find(flag) != flags_.end();
}

std::string CliParser::get_string(const std::string& flag, const std::string& default_val) const {
  auto it = flags_.find(flag);
  return (it != flags_.end()) ? it->second : default_val;
}

int CliParser::get_int(const std::string& flag, int default_val) const {
  auto it = flags_.find(flag);
  if (it == flags_.end()) {
    return default_val;
  }
  try {
    return std::stoi(it->second);
  } catch (const std::exception&) {
    return default_val;
  }
}

float CliParser::get_float(const std::string& flag, float default_val) const {
  auto it = flags_.find(flag);
  if (it == flags_.end()) {
    return default_val;
  }
  try {
    return std::stof(it->second);
  } catch (const std::exception&) {
    return default_val;
  }
}

bool CliParser::get_bool(const std::string& flag, bool default_val) const {
  auto it = flags_.find(flag);
  if (it == flags_.end()) {
    return default_val;
  }
  std::string val = it->second;
  std::transform(val.begin(), val.end(), val.begin(), ::tolower);
  return val == "true" || val == "1" || val == "yes" || val == "on";
}

std::unordered_map<std::string, std::string> CliParser::get_set_overrides() const {
  std::unordered_map<std::string, std::string> result;
  for (const auto& [key, value] : set_overrides_) {
    result[key] = value;
  }
  return result;
}

namespace {

/// @brief The child of `node` named by `segment`, creating an object member when missing.
///        In an array, `segment` must be the index of an existing element.
/// @throws std::invalid_argument when `segment` does not name a child `--set` can reach.
nlohmann::json& child_for_set(nlohmann::json& node, const std::string& segment,
                              const std::string& key) {
  if (node.is_array()) {
    const bool numeric = !segment.empty() &&
      std::all_of(segment.begin(), segment.end(), [](unsigned char c) { return std::isdigit(c); });
    if (!numeric || std::stoull(segment) >= node.size()) {
      throw std::invalid_argument("--set " + key + ": '" + segment +
                                  "' is not an index of an existing array element");
    }
    return node[std::stoull(segment)];
  }
  if (node.is_null()) node = nlohmann::json::object();
  if (!node.is_object()) {
    throw std::invalid_argument("--set " + key + ": '" + segment +
                                "' is inside a value, not an object");
  }
  return node[segment];
}

}  // namespace

nlohmann::json merge_overrides(
  const nlohmann::json& base,
  const std::unordered_map<std::string, std::string>& overrides)
{
  nlohmann::json result = base;

  for (const auto& [key, value] : overrides) {
    // Split key by dots for nested access
    std::vector<std::string> path;
    std::stringstream ss(key);
    std::string segment;
    while (std::getline(ss, segment, '.')) {
      path.push_back(segment);
    }

    if (path.empty()) {
      continue;
    }

    // Navigate to the nested location. A numeric segment indexes an existing array element.
    nlohmann::json* current = &result;
    for (size_t i = 0; i < path.size() - 1; ++i) {
      current = &child_for_set(*current, path[i], key);
    }

    // Set the value (try to parse as number or bool, otherwise string)
    nlohmann::json& target = child_for_set(*current, path.back(), key);

    // A key that already holds a string stays a string, so a numeric serial number is kept.
    if (target.is_string()) {
      target = value;
    } else if (value == "true" || value == "false") {
      target = (value == "true");
    } else if (!value.empty() && (std::isdigit(value[0]) ||
               value[0] == '-' || value[0] == '.')) {
      // Try to parse as number - only when the entire string is consumed.
      // This prevents partial parses like "10.0.0.1" -> 10.0 (IP addresses, etc.).
      bool parsed = false;
      try {
        if (value.find('.') != std::string::npos) {
          std::size_t pos = 0;
          const double dval = std::stod(value, &pos);
          if (pos == value.size()) {
            target = dval;
            parsed = true;
          }
        } else {
          std::size_t pos = 0;
          const int64_t ival = std::stoll(value, &pos);
          if (pos == value.size()) {
            target = ival;
            parsed = true;
          }
        }
      } catch (const std::exception&) {}
      if (!parsed) {
        target = value;
      }
    } else {
      // Default to string
      target = value;
    }
  }

  return result;
}

void dump_config(const nlohmann::json& config, const std::string& title) {
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "  " << title << "\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << config.dump(2) << "\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << std::endl;
}

}  // namespace trossen::configuration
