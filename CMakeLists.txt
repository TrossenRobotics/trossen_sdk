cmake_minimum_required(VERSION 3.16)
project(trossen_sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Glob all *.cpp files in src/
add_library(trossen_sdk
  src/backends/lerobot_v2.cpp
  src/hw/arm/arm_producer.cpp
  src/hw/camera/opencv_producer.cpp
)

# Required OpenCV dependency for camera producer
find_package(OpenCV REQUIRED)
target_include_directories(trossen_sdk PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(trossen_sdk PUBLIC ${OpenCV_LIBS})

target_include_directories(trossen_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Examples
add_subdirectory(examples)

# Benchmarks
add_subdirectory(benchmarks)
