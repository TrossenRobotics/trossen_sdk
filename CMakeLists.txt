cmake_minimum_required(VERSION 3.16)
project(trossen_sdk
  VERSION 0.1.0
  LANGUAGES CXX
)

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
  src/runtime/session_manager.cpp
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
target_link_libraries(trossen_sdk PUBLIC ${OpenCV_LIBS})

# Protobuf schemas for MCAP backend
# Try modern CMake config first, then fall back to FindProtobuf module
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  # Fall back to the Find module (older method)
  find_package(Protobuf QUIET)
  if(NOT Protobuf_FOUND)
    # Last resort: use pkg-config
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(Protobuf REQUIRED protobuf)
      # Set standard variables for compatibility
      set(Protobuf_LIBRARIES ${Protobuf_LINK_LIBRARIES})
      set(Protobuf_INCLUDE_DIRS ${Protobuf_INCLUDE_DIRS})
      # Find protoc compiler
      find_program(Protobuf_PROTOC_EXECUTABLE protoc REQUIRED)
    else()
      message(FATAL_ERROR "Protobuf not found. Please install libprotobuf-dev and protobuf-compiler")
    endif()
  endif()
endif()
message(STATUS "Found Protobuf: ${Protobuf_VERSION}")

# List proto files
set(TROSSEN_PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include/trossen_sdk/io/backends/mcap/proto)
set(PROTOBUF_IMPORT_DIRS ${TROSSEN_PROTO_PATH})
set(TROSSEN_PROTO_FILES
  ${TROSSEN_PROTO_PATH}/JointState.proto
  ${TROSSEN_PROTO_PATH}/RawImage.proto
  ${TROSSEN_PROTO_PATH}/RobotDescription.proto
  ${TROSSEN_PROTO_PATH}/SessionMetadata.proto
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
