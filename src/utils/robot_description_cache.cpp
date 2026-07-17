/**
 * @file robot_description_cache.cpp
 */

#include "trossen_sdk/utils/robot_description_cache.hpp"

#include <curl/curl.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_map>

#include "trossen_sdk/io/backend_utils.hpp"

namespace trossen::utils {

namespace {

static constexpr char kRepoBase[] =
  "https://raw.githubusercontent.com/TrossenRobotics/trossen_arm_description/";

// Maps the robot_name field from backend config to the repo-relative URDF path.
// Override with urdf_variant in config for variants not listed here (e.g. leader arm).
static const std::unordered_map<std::string, std::string> kUrdfLookup = {
  {"trossen_solo_ai",       "urdf/generated/wxai/wxai_follower.urdf"},
  {"trossen_stationary_ai", "urdf/generated/stationary_ai.urdf"},
  {"trossen_mobile_ai",     "urdf/generated/mobile_ai.urdf"},
};

// Download

static size_t curl_write_cb(char* data, size_t size, size_t nmemb, void* userp) {
  static_cast<std::string*>(userp)->append(data, size * nmemb);
  return size * nmemb;
}

std::string download_url(const std::string& url) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    std::cerr << "[robot_description] curl_easy_init failed\n";
    return "";
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;  // NOLINT(runtime/int) — libcurl requires long* for CURLINFO_RESPONSE_CODE
  if (res == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  }
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    std::cerr << "[robot_description] Download failed (" << url << "): "
              << curl_easy_strerror(res) << "\n";
    return "";
  }
  if (http_code != 200) {
    std::cerr << "[robot_description] HTTP " << http_code << " for " << url << "\n";
    return "";
  }
  return response;
}

// Cache

std::filesystem::path cache_root() {
  return trossen::io::backends::get_default_root_path() / "robot_description";
}

// Returns file content from the local cache. On a cache miss, downloads the file
// from GitHub, saves it, and returns the content.
std::string load_or_download(
  const std::string& rel_path,
  const std::string& git_ref,
  const std::filesystem::path& ref_cache_dir)
{
  auto cached = ref_cache_dir / rel_path;

  if (std::filesystem::exists(cached)) {
    std::ifstream f(cached, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  }

  std::string url = std::string(kRepoBase) + git_ref + "/" + rel_path;
  std::cout << "[robot_description] Downloading " << url << "\n";
  std::string data = download_url(url);
  if (data.empty()) return "";

  // Cache writes are best-effort: on failure, still return the downloaded
  // content and remove any partial file so it is not later read as valid.
  std::error_code ec;
  std::filesystem::create_directories(cached.parent_path(), ec);
  if (ec) {
    std::cerr << "[robot_description] Failed to create cache dir "
              << cached.parent_path() << ": " << ec.message() << "\n";
    return data;
  }
  std::ofstream out(cached, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!out) {
    std::cerr << "[robot_description] Failed to write cache file " << cached
              << "; skipping cache\n";
    out.close();
    std::filesystem::remove(cached, ec);
  }
  return data;
}

// Base64

static constexpr char kBase64Chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& input) {
  std::string result;
  result.reserve(((input.size() + 2) / 3) * 4);

  size_t i = 0;
  const size_t n = input.size();
  while (i + 2 < n) {
    uint32_t b = (static_cast<uint8_t>(input[i]) << 16) |
                 (static_cast<uint8_t>(input[i + 1]) << 8) |
                  static_cast<uint8_t>(input[i + 2]);
    result += kBase64Chars[(b >> 18) & 0x3f];
    result += kBase64Chars[(b >> 12) & 0x3f];
    result += kBase64Chars[(b >>  6) & 0x3f];
    result += kBase64Chars[(b) & 0x3f];
    i += 3;
  }
  if (i < n) {
    uint32_t b = static_cast<uint8_t>(input[i]) << 16;
    if (i + 1 < n) b |= static_cast<uint8_t>(input[i + 1]) << 8;
    result += kBase64Chars[(b >> 18) & 0x3f];
    result += kBase64Chars[(b >> 12) & 0x3f];
    result += (i + 1 < n) ? kBase64Chars[(b >> 6) & 0x3f] : '=';
    result += '=';
  }
  return result;
}

// URDF path patching

static bool is_mesh_or_texture(const std::string& rel_path) {
  return rel_path.ends_with(".stl") || rel_path.ends_with(".STL") ||
         rel_path.ends_with(".dae") || rel_path.ends_with(".DAE") ||
         rel_path.ends_with(".png") || rel_path.ends_with(".PNG");
}

// Replaces every package://trossen_arm_description/ reference with the absolute path
// of the corresponding file in the local cache, downloading it first if needed.
// The returned URDF works directly in Rerun and other tools without any ROS environment.
std::string patch_paths_to_cache(
  const std::string& urdf,
  const std::string& git_ref,
  const std::filesystem::path& ref_cache_dir)
{
  const std::string prefix = "package://trossen_arm_description/";
  std::string result = urdf;
  size_t pos = 0;

  while ((pos = result.find(prefix, pos)) != std::string::npos) {
    size_t end = result.find('"', pos);
    if (end == std::string::npos) break;

    std::string rel_path = result.substr(pos + prefix.size(), end - pos - prefix.size());

    if (!is_mesh_or_texture(rel_path)) {
      pos = end;
      continue;
    }

    // Skip rewriting when the asset is unavailable; keep the original
    // package:// reference instead of pointing at a missing cache file.
    if (load_or_download(rel_path, git_ref, ref_cache_dir).empty()) {
      pos = end;
      continue;
    }
    std::string abs_path = (ref_cache_dir / rel_path).string();

    result = result.substr(0, pos) + abs_path + result.substr(end);
    pos += abs_path.size();
  }

  return result;
}

// Replaces every package://trossen_arm_description/ reference with a base64-encoded
// data URI, making the URDF fully self-contained. The closing quote of each filename
// attribute is used as the boundary, so extra XML attributes like scale="..." are
// preserved.
std::string patch_paths_to_data_uris(
  const std::string& urdf,
  const std::string& git_ref,
  const std::filesystem::path& ref_cache_dir)
{
  const std::string prefix = "package://trossen_arm_description/";
  std::string result = urdf;
  size_t pos = 0;

  while ((pos = result.find(prefix, pos)) != std::string::npos) {
    size_t end = result.find('"', pos);
    if (end == std::string::npos) break;

    std::string rel_path = result.substr(pos + prefix.size(), end - pos - prefix.size());

    std::string mime;
    if (rel_path.ends_with(".stl") || rel_path.ends_with(".STL")) {
      mime = "model/stl";
    } else if (rel_path.ends_with(".dae") || rel_path.ends_with(".DAE")) {
      mime = "model/vnd.collada+xml";
    } else if (rel_path.ends_with(".png") || rel_path.ends_with(".PNG")) {
      mime = "image/png";
    } else {
      pos = end;
      continue;
    }

    std::string asset_data = load_or_download(rel_path, git_ref, ref_cache_dir);
    if (asset_data.empty()) {
      pos = end;
      continue;
    }

    std::string data_uri = "data:" + mime + ";base64," + base64_encode(asset_data);
    result = result.substr(0, pos) + data_uri + result.substr(end);
    pos += data_uri.size();
  }

  return result;
}

}  // namespace

// Public API

std::string RobotDescriptionCache::resolve(
  const std::string& robot_name,
  const std::string& urdf_variant,
  const std::string& git_ref,
  bool include_meshes)
{
  std::string urdf_path;
  if (!urdf_variant.empty()) {
    urdf_path = urdf_variant;
  } else {
    auto it = kUrdfLookup.find(robot_name);
    if (it == kUrdfLookup.end()) {
      std::cerr << "[robot_description] No URDF known for robot_name='"
                << robot_name << "'. Set urdf_variant in config to override.\n";
      return "";
    }
    urdf_path = it->second;
  }

  auto ref_cache_dir = cache_root() / git_ref;
  std::error_code ec;
  std::filesystem::create_directories(ref_cache_dir, ec);
  if (ec) {
    std::cerr << "[robot_description] Failed to create cache dir "
              << ref_cache_dir << ": " << ec.message() << "\n";
    return "";
  }

  std::string urdf = load_or_download(urdf_path, git_ref, ref_cache_dir);
  if (urdf.empty()) return "";

  if (!include_meshes) {
    // Replace package:// paths with absolute local cache paths so the URDF
    // is immediately usable by Rerun and other tools on this machine.
    return patch_paths_to_cache(urdf, git_ref, ref_cache_dir);
  }

  // Embed all mesh files as base64 data URIs for a fully portable URDF.
  return patch_paths_to_data_uris(urdf, git_ref, ref_cache_dir);
}

}  // namespace trossen::utils
