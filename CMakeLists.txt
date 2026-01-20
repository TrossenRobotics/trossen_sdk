cmake_minimum_required(VERSION 3.16)
project(trossen_sdk
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TROSSEN_ENABLE_REALSENSE "Enable Intel RealSense support" OFF)
option(TROSSEN_ENABLE_ZED "Enable ZED camera support" ON)

add_library(trossen_sdk SHARED
  src/backends/backend_registry.cpp
  src/backends/trossen/trossen_backend.cpp
  src/backends/lerobot/lerobot_backend.cpp
  src/backends/mcap/mcap_backend.cpp
  src/backends/null/null_backend.cpp
  src/configuration/global_config.cpp
  src/configuration/loaders/json_loader.cpp
  src/hw/hardware_registry.cpp
  src/hw/active_hardware_registry.cpp
  src/hw/arm/trossen_arm_component.cpp
  src/hw/arm/arm_producer.cpp
  src/hw/arm/teleop_arm_producer.cpp
  src/hw/arm/mock_joint_producer.cpp
  src/hw/arm/teleop_mock_joint_producer.cpp
  src/hw/arm/so101_arm_driver.cpp
  src/hw/arm/feetech_bus.cpp
  src/hw/arm/so101_teleop_arm_producer.cpp
  src/hw/camera/opencv_camera_component.cpp
  src/hw/camera/opencv_producer.cpp
  src/hw/camera/mock_producer.cpp
  src/hw/camera/mock_synced_producer.cpp
  src/runtime/session_manager.cpp
)

if(TROSSEN_ENABLE_REALSENSE)
  message(STATUS "Intel RealSense support enabled")
  target_sources(trossen_sdk PRIVATE
    src/hw/camera/realsense_producer.cpp
    src/hw/camera/realsense_depth_producer.cpp
    src/hw/camera/realsense_camera_component.cpp
  )
  target_compile_definitions(trossen_sdk PUBLIC TROSSEN_ENABLE_REALSENSE)

  # Find RealSense dependencies first
  find_package(fastcdr REQUIRED)
  find_package(fastrtps REQUIRED)
  find_package(realsense2 REQUIRED)
endif()

if(TROSSEN_ENABLE_ZED)
  message(STATUS "ZED camera support enabled")
  target_sources(trossen_sdk PRIVATE
    src/hw/camera/zed_producer.cpp
    src/hw/camera/zed_depth_producer.cpp
    src/hw/camera/zed_camera_component.cpp
  )
  target_compile_definitions(trossen_sdk PUBLIC TROSSEN_ENABLE_ZED)

  # Find ZED SDK
  find_package(ZED 4 REQUIRED)
  find_package(CUDA REQUIRED)
endif()

# Add architecture-specific CMAKE_PREFIX_PATH
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} /usr/lib/x86_64-linux-gnu/cmake)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} /usr/lib/aarch64-linux-gnu/cmake)
endif()

# Try to find Protobuf
find_package(PkgConfig)
# First try pkg-config
if(PKG_CONFIG_FOUND)
  pkg_check_modules(Protobuf REQUIRED protobuf)

  message(STATUS "Found Protobuf via pkg-config: ${Protobuf_VERSION}")
else()
  # Fallback to CMake find_package
  find_package(Protobuf REQUIRED)
  message(STATUS "Found Protobuf: ${Protobuf_VERSION}")
endif()

# Help CMake find ZLIB and BZip2 (needed by Arrow)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set(ZLIB_ROOT /usr/lib/x86_64-linux-gnu)
  set(BZIP2_LIBRARIES /usr/lib/x86_64-linux-gnu/libbz2.so)
  set(BZIP2_INCLUDE_DIRS /usr/include)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(ZLIB_ROOT /usr/lib/aarch64-linux-gnu)
  set(BZIP2_LIBRARIES /usr/lib/aarch64-linux-gnu/libbz2.so)
  set(BZIP2_INCLUDE_DIRS /usr/include)
endif()
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)

find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(libtrossen_arm REQUIRED)

include(FetchContent)

set(FOXGLOVE_VERSION "0.16.1")
# Detect platform for Foxglove SDK download
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set(FOXGLOVE_ARCH "x86_64-unknown-linux-gnu")
  set(FOXGLOVE_HASH "d8afbb8f99b75d7ad0f1b2885d35cacead28e6f2e36bd4d1a30e17413ebab896")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(FOXGLOVE_ARCH "aarch64-unknown-linux-gnu")
  set(FOXGLOVE_HASH "20dd412fa8be481e6ed84d88879f368e1a55a146e9a98f936a05931742e1dea5")
else()
  message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

FetchContent_Declare(
  foxglove
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL "https://github.com/foxglove/foxglove-sdk/releases/download/sdk%2Fv${FOXGLOVE_VERSION}/foxglove-v${FOXGLOVE_VERSION}-cpp-${FOXGLOVE_ARCH}.zip"
  URL_HASH SHA256=${FOXGLOVE_HASH}
)
message(STATUS "Fetching Foxglove SDK v${FOXGLOVE_VERSION} for ${FOXGLOVE_ARCH}...")
FetchContent_MakeAvailable(foxglove)

# Find all Foxglove SDK source files
file(GLOB FOXGLOVE_SOURCES CONFIGURE_DEPENDS
  "${foxglove_SOURCE_DIR}/src/*.cpp"
  "${foxglove_SOURCE_DIR}/src/server/*.cpp"
)

# Add Foxglove SDK source files
target_sources(trossen_sdk PRIVATE ${FOXGLOVE_SOURCES})

# Add Foxglove SDK include directories
target_include_directories(trossen_sdk PUBLIC
  ${foxglove_SOURCE_DIR}/include
  ${foxglove_SOURCE_DIR}/include/foxglove
)

# Link against foxglove library
target_link_libraries(trossen_sdk PRIVATE ${foxglove_SOURCE_DIR}/lib/libfoxglove.a)

# Required OpenCV dependency for camera producer
# On Ubuntu/Debian, OpenCV 4.x is typically installed as opencv4
# Add common installation paths to help CMake find it
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} /usr/lib/x86_64-linux-gnu/cmake)
find_package(OpenCV QUIET NAMES opencv4 OpenCV)
if(NOT OpenCV_FOUND)
  # Fall back to pkg-config if CMake config isn't found
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(OpenCV REQUIRED opencv4)
  else()
    message(FATAL_ERROR "OpenCV not found. Please install libopencv-dev")
  endif()
endif()
message(STATUS "Found OpenCV: ${OpenCV_VERSION}")
target_include_directories(trossen_sdk PUBLIC ${OpenCV_INCLUDE_DIRS})

# # Feetech servo library for SO101 arms (SCServo library)
# find_library(SCSERVO_LIBRARY NAMES SCServo PATHS /usr/local/lib /usr/lib)
# find_path(SCSERVO_INCLUDE_DIR ftservo/ftservo.h PATHS /usr/local/include /usr/include)
FetchContent_Declare(
  SCServo
  SYSTEM
  GIT_REPOSITORY https://github.com/ftservo/FTServo_Linux.git
  GIT_TAG 064a6dbf814477b970959b5b01b2fc9bc594afa3
  CMAKE_ARGS
    -D CMAKE_POLICY_VERSION_MINIMUM=3.5
  PATCH_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR> bash -c "git apply --check ${CMAKE_SOURCE_DIR}/cmake/patches/SCServo.patch && git apply ${CMAKE_SOURCE_DIR}/cmake/patches/SCServo.patch || echo 'Patch already applied'"

)
FetchContent_MakeAvailable(SCServo)

# Add SCServo src subdirectory to build the library
add_subdirectory(${scservo_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/scservo-build)

# Use the FetchContent source directory for include paths
target_include_directories(trossen_sdk PUBLIC ${scservo_SOURCE_DIR}/src)

target_link_libraries(
  trossen_sdk PUBLIC
  ${OpenCV_LIBS}
  Arrow::arrow_shared
  Parquet::parquet_shared
  libtrossen_arm
  SCServo
)
if(TROSSEN_ENABLE_REALSENSE)
  target_link_libraries(trossen_sdk PUBLIC realsense2::realsense2)
endif()
if(TROSSEN_ENABLE_ZED)
  target_link_libraries(trossen_sdk PUBLIC ${ZED_LIBRARIES} ${CUDA_LIBRARIES})
  target_include_directories(trossen_sdk PUBLIC ${ZED_INCLUDE_DIRS} ${CUDA_INCLUDE_DIRS})
endif()

# List proto files for MCAP backend
set(TROSSEN_PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include/trossen_sdk/io/backends/mcap/proto)
set(PROTOBUF_IMPORT_DIRS ${TROSSEN_PROTO_PATH})
set(TROSSEN_PROTO_FILES
  ${TROSSEN_PROTO_PATH}/JointState.proto
  ${TROSSEN_PROTO_PATH}/RobotDescription.proto
  ${TROSSEN_PROTO_PATH}/Timestamp.proto
)

# Generate protobuf files with proper import paths
foreach(PROTO_FILE ${TROSSEN_PROTO_FILES})
  get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
  set(PROTO_SRC ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc)
  set(PROTO_HDR ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h)
  add_custom_command(
    OUTPUT ${PROTO_SRC} ${PROTO_HDR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
         --proto_path=${TROSSEN_PROTO_PATH}
         ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Running cpp protocol buffer compiler on ${PROTO_FILE}"
  )
  list(APPEND TROSSEN_PROTO_SRCS ${PROTO_SRC})
  list(APPEND TROSSEN_PROTO_HDRS ${PROTO_HDR})
endforeach()

target_sources(trossen_sdk PRIVATE ${TROSSEN_PROTO_SRCS} ${TROSSEN_PROTO_HDRS})
target_link_libraries(trossen_sdk PUBLIC ${Protobuf_LIBRARIES})
target_include_directories(trossen_sdk PUBLIC ${Protobuf_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(trossen_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Examples
add_subdirectory(examples)

# Testing
option(BUILD_TESTING "Build the testing tree" ON)
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

# Benchmarks
option(BUILD_BENCHMARKS "Build the benchmarking tree" OFF)
if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# Applications
option(BUILD_APPS "Build applications" OFF)
if(BUILD_APPS)
  add_subdirectory(apps/soma)
endif()
