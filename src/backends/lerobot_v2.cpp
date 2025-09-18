#include "trossen_sdk/io/backends/lerobot_v2.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>

#include "trossen_sdk/data/record.hpp"

// For PNG writing we stub out with raw dump placeholder (future: integrate stb_image_write or libpng)
namespace trossen::io::backends {

namespace fs = std::filesystem;

bool LeRobotV2Backend::open(const std::string& uri) {
  root_ = fs::path(uri);
  images_root_ = root_ / "observations" / "images";
  try {
    fs::create_directories(images_root_);
  } catch (const std::exception& e) {
    std::cerr << "Failed to create directories: " << e.what() << "\n";
    return false;
  }
  joint_csv_.open((root_ / "joint_states.csv").string(), std::ios::out | std::ios::trunc);
  if (!joint_csv_) {
    std::cerr << "Failed to open joint_states.csv\n";
    return false;
  }
  header_written_ = false;
  return true;
}

void LeRobotV2Backend::write(const data::RecordBase& record) {
  // Decide type by RTTI (simple approach for now)
  if (auto js = dynamic_cast<const data::JointStateRecord*>(&record)) {
    writeJointState(*js);
  } else if (auto img = dynamic_cast<const data::ImageRecord*>(&record)) {
    writeImage(*img);
  } else {
    // Unknown type ignored for now
  }
}

void LeRobotV2Backend::writeBatch(std::span<const data::RecordBase* const> records) {
  for (auto* r : records) {
    if (r) write(*r);
  }
}

void LeRobotV2Backend::flush() {
  if (joint_csv_) joint_csv_.flush();
}

void LeRobotV2Backend::close() {
  if (joint_csv_.is_open()) joint_csv_.close();
}

void LeRobotV2Backend::writeJointState(const data::RecordBase& base) {
  auto& js = static_cast<const data::JointStateRecord&>(base);
  if (!header_written_) {
    joint_csv_ << "monotonic_ns,realtime_ns,id,positions,velocities,efforts\n";
    header_written_ = true;
  }
  auto vecToStr = [](const std::vector<float>& v){
    std::ostringstream oss; for (size_t i=0;i<v.size();++i){ if(i) oss<<";"; oss<<std::setprecision(6)<<v[i]; } return oss.str(); };
  joint_csv_ << js.ts.monotonic_ns << ','
             << js.ts.realtime_ns << ','
             << js.id << ','
             << vecToStr(js.positions) << ','
             << vecToStr(js.velocities) << ','
             << vecToStr(js.efforts) << '\n';
}

void LeRobotV2Backend::writeImage(const data::RecordBase& base) {
  auto& img = static_cast<const data::ImageRecord&>(base);
  // Directory per camera id
  fs::path camera_dir = images_root_ / img.id;
  std::error_code ec;
  fs::create_directories(camera_dir, ec);
  // Filename uses monotonic ns
  fs::path file_path = camera_dir / (std::to_string(img.ts.monotonic_ns) + ".png");
  // Placeholder: simply dump raw bytes (not valid PNG). Future: real PNG encoder.
  std::ofstream ofs(file_path.string(), std::ios::binary | std::ios::out | std::ios::trunc);
  if (!ofs) return;
  // Minimal pseudo header to signal placeholder
  ofs << "FAKEPNG";
  if (img.data && !img.data->empty()) {
    ofs.write(reinterpret_cast<const char*>(img.data->data()), static_cast<std::streamsize>(img.data->size()));
  }
}

} // namespace trossen::io::backends
