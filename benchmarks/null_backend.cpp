/**
 * @file benchmark.cpp
 * @brief Benchmark the performance of the sinks.
 *
 * This benchmark uses the NullBackend to measure the throughput of the sink system.
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>

#include "trossen_sdk/io/sink.hpp"
#include "trossen_sdk/io/backends/null_backend.hpp"
#include "trossen_sdk/data/record.hpp"

using namespace std::chrono;

struct Args {
  // benchmark duration in ms
  int duration_ms = 1000;

  // records per second
  int joint_rate = 200;

  // disabled by default
  int image_rate = 0;

  // joints per joint state message
  int joints = 14;
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i){
    // Check for help flags
    if(!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      std::cout << "Usage: benchmark [--duration ms] [--joint-rate hz] [--image-rate hz] [--joints n]" << std::endl;
      std::cout << "  --duration ms    Duration of the benchmark in milliseconds (default=1000)" << std::endl;
      std::cout << "  --joint-rate hz  Target joint state message rate (default=200)" << std::endl;
      std::cout << "  --image-rate hz  Target image message rate (default=0=disabled)" << std::endl;
      std::cout << "  --joints n       Number of joints per joint state message (default=14)" << std::endl;
      std::exit(0);
    }

    // Parse known args
    if(!std::strcmp(argv[i], "--duration") && i + 1 < argc) {
      a.duration_ms = std::atoi(argv[++i]);
    } else if(!std::strcmp(argv[i], "--joint-rate") && i + 1 <argc) {
      a.joint_rate = std::atoi(argv[++i]);
    } else if(!std::strcmp(argv[i], "--image-rate") && i + 1 <argc) {
      a.image_rate = std::atoi(argv[++i]);
    } else if(!std::strcmp(argv[i], "--joints") && i + 1 <argc) {
      a.joints = std::atoi(argv[++i]);
    }
  }

  // Print off configuration
  std::cout << "Benchmark configuration:" << std::endl;
  std::cout << " Duration:   " << a.duration_ms << " ms" << std::endl;
  std::cout << " Joint rate: " << a.joint_rate << " Hz" << std::endl;
  std::cout << " Image rate: " << a.image_rate << " Hz" << std::endl;
  std::cout << " Joints:     " << a.joints << std::endl;

  return a;
}

int main(int argc, char** argv) {
  auto args = parse(argc, argv);
  trossen::io::BackendPtr backend = std::make_unique<trossen::io::backends::NullBackend>("null://");
  trossen::io::Sink sink(std::move(backend));
  sink.start();

  auto start = steady_clock::now();
  auto end_time = start + milliseconds(args.duration_ms);
  auto next_joint = start;
  auto next_image = start;
  auto joint_period = (args.joint_rate>0)? nanoseconds(1'000'000'000LL / args.joint_rate) : nanoseconds::max();
  auto image_period = (args.image_rate>0)? nanoseconds(1'000'000'000LL / args.image_rate) : nanoseconds::max();

  uint64_t produced_joint = 0;
  uint64_t produced_image = 0;

  std::vector<float> zeros(args.joints, 0.f);

  while (steady_clock::now() < end_time) {
    auto now = steady_clock::now();
    if (now >= next_joint) {
      trossen::data::JointStateRecord rec;
      rec.ts = trossen::data::make_timestamp_now();
      rec.seq = produced_joint;
      rec.id = "bench/joint_states";
      rec.positions = zeros;
      rec.velocities = zeros;
      rec.efforts = zeros;
      sink.emplace<trossen::data::JointStateRecord>(rec);
      ++produced_joint;
      next_joint += joint_period;
    }
    if (now >= next_image) {
      trossen::data::ImageRecord img;
      img.ts = trossen::data::make_timestamp_now();
      img.seq = produced_image;
      img.id = "bench/image";

      // 1080p RGB
      img.width = 1920;
      img.height = 1080;
      img.channels = 3;
      img.encoding = "rgb8";

      auto buf = std::make_shared<std::vector<uint8_t>>();
      buf->resize(64*64*3);
      img.data = buf;

      sink.emplace<trossen::data::ImageRecord>(img);
      ++produced_image;
      next_image += image_period;
    }
    // Tight loop; small sleep to avoid maxing a single core if rates are low
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  sink.stop();

  double seconds = args.duration_ms / 1000.0;
  double joint_throughput = produced_joint / seconds;
  double image_throughput = (args.image_rate > 0) ? produced_image / seconds : 0.0;

  // Print off results
  std::cout << std::endl
            << "Benchmark results:" << std::endl
            << "----------------------" << std::endl
            << " Duration: " << args.duration_ms << " ms" << std::endl
            << " Target joint rate: " << args.joint_rate << " Hz" << std::endl
            << " Produced joint states: " << produced_joint << std::endl
            << " Joint throughput: " << joint_throughput << std::endl
            << " Target image rate: " << args.image_rate << " Hz" << std::endl
            << " Produced images: " << produced_image << std::endl
            << " Image throughput: " << image_throughput << std::endl
            << std::endl;

  return 0;
}
