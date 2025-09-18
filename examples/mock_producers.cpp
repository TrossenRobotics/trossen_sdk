#include <random>
#include <chrono>
#include <thread>
#include <iostream>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/sink.hpp"
#include "trossen_sdk/io/backends/lerobot_v2.hpp"

using namespace std::chrono;

static uint64_t now_monotonic_ns() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}
static uint64_t now_realtime_ns() {
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

struct MockJointStateGenerator {
  size_t joint_count{6};
  uint64_t seq{0};
  std::mt19937 rng{std::random_device{}()};
  std::normal_distribution<float> noise{0.f, 0.01f};

  trossen::data::JointStateRecord makeSample(const std::string& id) {
    trossen::data::JointStateRecord rec;
    rec.ts.monotonic_ns = now_monotonic_ns();
    rec.ts.realtime_ns = now_realtime_ns();
    rec.seq = seq++;
    rec.id = id;
    rec.positions.resize(joint_count);
    rec.velocities.resize(joint_count);
    rec.efforts.resize(joint_count);
    for (size_t i=0;i<joint_count;++i) {
      float base = static_cast<float>(i) * 0.1f;
      rec.positions[i] = base + noise(rng);
      rec.velocities[i] = noise(rng);
      rec.efforts[i] = 0.5f + noise(rng);
    }
    return rec;
  }
};

struct MockImageGenerator {
  uint32_t width{320};
  uint32_t height{240};
  uint32_t channels{3};
  uint64_t seq{0};

  trossen::data::ImageRecord makeSample(const std::string& id) {
    trossen::data::ImageRecord rec;
    rec.ts.monotonic_ns = now_monotonic_ns();
    rec.ts.realtime_ns = now_realtime_ns();
    rec.seq = seq++;
    rec.id = id;
    rec.width = width;
    rec.height = height;
    rec.channels = channels;
    rec.encoding = "rgb8";
    auto buf = std::make_shared<std::vector<uint8_t>>();
    buf->resize(width * height * channels);
    // simple gradient pattern with seq influence
    for (uint32_t y=0; y<height; ++y) {
      for (uint32_t x=0; x<width; ++x) {
        size_t idx = (y*width + x)*channels;
        (*buf)[idx+0] = static_cast<uint8_t>((x + seq) % 256);
        (*buf)[idx+1] = static_cast<uint8_t>((y + seq) % 256);
        (*buf)[idx+2] = static_cast<uint8_t>(((x+y) + seq) % 256);
      }
    }
    rec.data = std::move(buf);
    return rec;
  }
};

/**
 * @brief Run a mock data session producing joint states and images at specified rates.
 *
 * @param output_dir Directory to write data (will be created if needed)
 * @param joint_hz Joint state frequency in Hz
 * @param image_hz Image frequency in Hz
 * @param runtime_ms Total runtime in milliseconds
 */
void run_mock_session(
  const std::string& output_dir,
  int joint_hz = 100,
  int image_hz = 10,
  int runtime_ms = 500)
{
  trossen::io::BackendPtr backend = std::make_unique<trossen::io::backends::LeRobotV2Backend>(output_dir);
  trossen::io::Sink sink(std::move(backend));
  sink.start();

  MockJointStateGenerator joint_gen;
  MockImageGenerator image_gen;

  size_t expected_records = (joint_hz * runtime_ms / 1000) + (image_hz * runtime_ms / 1000);
  std::cout << "Expecting ~"
            << expected_records
            << " total records ("
            << (joint_hz * runtime_ms / 1000)
            << " joint states, "
            << (image_hz * runtime_ms / 1000)
            << " images)"
            << std::endl;

  auto start = steady_clock::now();
  auto next_joint = start;
  auto next_image = start;
  auto joint_period = milliseconds(1000 / joint_hz);
  auto image_period = milliseconds(1000 / image_hz);

  while (duration_cast<milliseconds>(steady_clock::now() - start).count() < runtime_ms) {
    auto now = steady_clock::now();
    if (now >= next_joint) {
      sink.emplace<trossen::data::JointStateRecord>(joint_gen.makeSample("follower/joint_states"));
      next_joint += joint_period;
    }
    if (now >= next_image) {
      sink.emplace<trossen::data::ImageRecord>(image_gen.makeSample("cam_high"));
      next_image += image_period;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  sink.stop();
  std::cout << "Mock session complete. Output at: " << output_dir << std::endl;
  std::cout << "Saved "
            << sink.processed_count()
            << " records ("
            << (100.0f * sink.processed_count() / expected_records)
            << "% of expected)"
            << std::endl;
}

int main(int argc, char** argv) {
  std::string output_dir = "output";
  // Optional output directory and runtime_ms arguments
  int runtime_ms = 10000;
  if (argc > 1) {
    output_dir = argv[1];
  }
  if (argc > 2) {
    runtime_ms = std::stoi(argv[2]);
  }
  run_mock_session(output_dir, 100, 10, runtime_ms);
  return 0;
}
