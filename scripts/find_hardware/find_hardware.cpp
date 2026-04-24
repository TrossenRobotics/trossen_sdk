/**
 * @file find_hardware.cpp
 * @brief Discover connected hardware of a given type and save preview images.
 *
 * Thin CLI over @c DiscoveryRegistry — dispatches the type argument to whichever
 * component self-registered under that key. Adding a new discoverable hardware
 * type requires only a @c REGISTER_HARDWARE_DISCOVERY in the component's .cpp;
 * this CLI does not need to change.
 *
 * Usage:
 *   ./find_hardware <type> [--output DIR]
 *
 * Preview images are written to <output>/<type>_<id>.jpg so you can identify
 * which physical device maps to which serial number or device index.
 */

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include "trossen_sdk/hw/discovery_registry.hpp"

namespace fs = std::filesystem;
using trossen::hw::DiscoveryRegistry;

namespace {

std::string supported_types_csv() {
  auto types = DiscoveryRegistry::supported_types();
  std::string out;
  for (size_t i = 0; i < types.size(); ++i) {
    if (i) out += ", ";
    out += types[i];
  }
  return out.empty() ? "(none registered)" : out;
}

void print_help(const char* argv0, const fs::path& default_out) {
  std::cout << "Usage: " << argv0 << " <type> [--output DIR]\n\n"
            << "  type            One of: " << supported_types_csv() << "\n"
            << "  --output DIR    Preview output directory (default: "
            << default_out.string() << ")\n"
            << "  --help          Show this help\n\n"
            << "Outputs <type>_<identifier>.jpg for each detected device so you\n"
            << "can match physical devices to their config IDs.\n";
}

}  // namespace

int main(int argc, char** argv) {
  fs::path    output_dir = "./scripts/find_hardware/discovery";
  std::string type;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_help(argv[0], output_dir);
      return 0;
    }
    if (arg == "--output" && i + 1 < argc) {
      output_dir = argv[++i];
      continue;
    }
    type = arg;
  }

  if (type.empty()) {
    std::cerr << "Error: hardware type required. See --help.\n";
    return 1;
  }

  if (fs::exists(output_dir)) fs::remove_all(output_dir);
  fs::create_directories(output_dir);

  auto result = DiscoveryRegistry::find(type, output_dir);
  if (!result) {
    std::cerr << "Error: no discovery support for type '" << type << "'. "
              << "Supported: " << supported_types_csv() << "\n";
    return 1;
  }

  const auto& devices = *result;
  if (devices.empty()) {
    std::cout << "No " << type << " devices found.\n";
    return 0;
  }

  std::cout << "\n"
            << std::left
            << std::setw(22) << "Identifier"
            << std::setw(12) << "Resolution"
            << std::setw(6)  << "FPS"
            << "Preview\n"
            << std::string(70, '-') << "\n";

  for (const auto& dev : devices) {
    int width        = dev.details.value("width",  0);
    int height       = dev.details.value("height", 0);
    int fps          = dev.details.value("fps",    0);
    std::string prev = dev.details.value("preview_path", std::string{});
    std::string res  = std::to_string(width) + "x" + std::to_string(height);
    std::string fps_str = (fps > 0) ? std::to_string(fps) : "?";
    std::cout << std::left
              << std::setw(22) << dev.identifier
              << std::setw(12) << res
              << std::setw(6)  << fps_str
              << prev
              << (dev.ok ? "" : "  [preview failed]")
              << "\n";
  }
  std::cout << "\n";

  return 0;
}
