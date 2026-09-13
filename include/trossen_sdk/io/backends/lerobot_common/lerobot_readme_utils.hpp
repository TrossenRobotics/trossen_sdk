/**
 * @file lerobot_readme_utils.hpp
 * @brief README.md dataset-card generation shared across output versions.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_README_UTILS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_README_UTILS_HPP_

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace trossen::io::backends {

/**
 * @brief Generate a README.md dataset card for HuggingFace Hub compatibility
 *
 * Creates a README.md with YAML frontmatter (license, task_categories, tags, configs)
 * and a markdown body embedding the full info.json content.
 *
 * @param dataset_root Path to the dataset root directory (containing meta/, data/, etc.)
 * @param info_json_path Location of the info.json to embed, relative to `dataset_root`;
 *        an absolute path is also accepted. A path that does not exist embeds an empty
 *        object.
 * @param tool_name Name of the converter binary that produced this dataset, credited in
 *        the README body (e.g. "trossen_mcap_to_lerobot_v2").
 * @param license SPDX license identifier for the dataset (default: "apache-2.0")
 * @return true on success, false on failure
 */
inline bool generate_dataset_readme(
    const std::filesystem::path& dataset_root,
    const std::filesystem::path& info_json_path,
    const std::string& tool_name,
    const std::string& license = "apache-2.0") {
  namespace fs = std::filesystem;

  fs::path readme_path = dataset_root / "README.md";
  fs::path info_path = dataset_root / info_json_path;

  // Read info.json content for embedding in the README
  std::string info_json_str = "{}";
  if (fs::exists(info_path)) {
    std::ifstream info_file(info_path);
    if (info_file.is_open()) {
      try {
        nlohmann::ordered_json info_json;
        info_file >> info_json;
        info_json_str = info_json.dump(4);
      } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to parse " << info_path << ": " << e.what() << "\n";
      }
      info_file.close();
    }
  }

  std::ofstream readme_file(readme_path);
  if (!readme_file.is_open()) {
    std::cerr << "Error: Failed to create " << readme_path << " for writing\n";
    return false;
  }

  readme_file
    << "---\n"
    << "license: " << license << "\n"
    << "task_categories:\n"
    << "- robotics\n"
    << "tags:\n"
    << "- LeRobot\n"
    << "configs:\n"
    << "- config_name: default\n"
    << "  data_files: data/*/*.parquet\n"
    << "---\n"
    << "\n"
    << "This dataset was created using [LeRobot](https://github.com/huggingface/lerobot).\n"
    << "\n"
    << "## Dataset Description\n"
    << "\n"
    << "Converted from TrossenMCAP format using the Trossen SDK `" << tool_name << "` tool.\n"
    << "\n"
    << "- **Homepage:** [More Information Needed]\n"
    << "- **Paper:** [More Information Needed]\n"
    << "- **License:** " << license << "\n"
    << "\n"
    << "## Dataset Structure\n"
    << "\n"
    << "[meta/info.json](meta/info.json):\n"
    << "\n"
    << "```json\n"
    << info_json_str << "\n"
    << "```\n"
    << "\n"
    << "\n"
    << "## Citation\n"
    << "\n"
    << "**BibTeX:**\n"
    << "\n"
    << "```bibtex\n"
    << "[More Information Needed]\n"
    << "```\n";

  readme_file.close();
  return true;
}

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_README_UTILS_HPP_
