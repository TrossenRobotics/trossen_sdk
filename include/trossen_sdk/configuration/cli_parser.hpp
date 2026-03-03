/**
 * @file cli_parser.hpp
 * @brief Simple CLI argument parser for configuration overrides
 */

#ifndef TROSSEN_SDK__CONFIGURATION__CLI_PARSER_HPP_
#define TROSSEN_SDK__CONFIGURATION__CLI_PARSER_HPP_

#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Simple command-line argument parser
 *
 * Supports:
 * - Named flags: --flag value or --flag=value
 * - Boolean flags: --flag (sets to true)
 * - Positional arguments
 */
class CliParser {
public:
  CliParser(int argc, char** argv);

  /// Check if a flag is present
  bool has_flag(const std::string& flag) const;

  /// Get string value for a flag (returns default if not found)
  std::string get_string(const std::string& flag, const std::string& default_val = "") const;

  /// Get integer value for a flag (returns default if not found)
  int get_int(const std::string& flag, int default_val = 0) const;

  /// Get float value for a flag (returns default if not found)
  float get_float(const std::string& flag, float default_val = 0.0f) const;

  /// Get boolean value for a flag (returns default if not found)
  bool get_bool(const std::string& flag, bool default_val = false) const;

  /// Get all key=value pairs from --set flags
  std::unordered_map<std::string, std::string> get_set_overrides() const;

  /// Get positional arguments (non-flag arguments)
  const std::vector<std::string>& get_positional() const { return positional_; }

private:
  std::unordered_map<std::string, std::string> flags_;
  std::vector<std::string> positional_;
  std::vector<std::pair<std::string, std::string>> set_overrides_;
};

/**
 * @brief Merge CLI overrides into a JSON object
 *
 * Supports dot notation for nested keys: "camera.width=640"
 *
 * @param base Base JSON configuration
 * @param overrides Key-value pairs from CLI (e.g., from --set)
 * @return Merged JSON configuration
 */
nlohmann::json merge_overrides(
  const nlohmann::json& base,
  const std::unordered_map<std::string, std::string>& overrides);

/**
 * @brief Dump configuration to stdout in human-readable format
 *
 * @param config JSON configuration to dump
 * @param title Optional title to display
 */
void dump_config(const nlohmann::json& config, const std::string& title = "Configuration");

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__CLI_PARSER_HPP_
