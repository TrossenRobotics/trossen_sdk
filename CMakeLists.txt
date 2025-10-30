cmake_minimum_required(VERSION 3.16)
project(trossen_sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_library(trossen_sdk
  src/backends/trossen/trossen_backend.cpp
  src/backends/mcap/mcap_backend.cpp
  src/hw/arm/arm_producer.cpp
  src/hw/arm/mock_joint_producer.cpp
  src/hw/camera/opencv_producer.cpp
  src/hw/camera/mock_producer.cpp
  src/hw/camera/mock_synced_producer.cpp
  src/runtime/episode_manager.cpp
)

include(FetchContent)
FetchContent_Declare(
  mcap_builder
  GIT_REPOSITORY https://github.com/olympus-robotics/mcap_builder.git
  GIT_TAG d6f3662b7204341797eaeca5ff97b8f659d02bda
)
message(STATUS "Fetching MCAP (mcap_builder)...")
FetchContent_MakeAvailable(mcap_builder)
# mcap_builder provides an imported target named "mcap"
target_link_libraries(trossen_sdk PUBLIC mcap)
target_compile_definitions(trossen_sdk PUBLIC TROSSEN_WITH_MCAP=1)

# Required OpenCV dependency for camera producer
find_package(OpenCV REQUIRED)
target_include_directories(trossen_sdk PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(trossen_sdk PUBLIC ${OpenCV_LIBS})

# Protobuf schemas for MCAP backend
find_package(Protobuf REQUIRED)
set(PROTOBUF_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)

# List proto files
set(TROSSEN_PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include/trossen_sdk/io/backends/mcap/proto)
set(TROSSEN_PROTO_FILES
  ${TROSSEN_PROTO_PATH}/timestamp.proto
  ${TROSSEN_PROTO_PATH}/joint_state.proto
  ${TROSSEN_PROTO_PATH}/RawImage.proto
  ${TROSSEN_PROTO_PATH}/session_metadata.proto
  ${TROSSEN_PROTO_PATH}/robot_description.proto
)

set(PROTOBUF_GENERATE_CPP_APPEND_PATH ON)
protobuf_generate_cpp(TROSSEN_PROTO_SRCS TROSSEN_PROTO_HDRS ${TROSSEN_PROTO_FILES})
target_sources(trossen_sdk PRIVATE ${TROSSEN_PROTO_SRCS} ${TROSSEN_PROTO_HDRS})
target_link_libraries(trossen_sdk PUBLIC ${Protobuf_LIBRARIES})
target_include_directories(trossen_sdk PUBLIC ${Protobuf_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(trossen_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Examples
add_subdirectory(examples)

# Benchmarks
add_subdirectory(benchmarks)
