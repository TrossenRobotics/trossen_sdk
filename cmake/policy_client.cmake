# PolicyClient dependencies and source wiring (gated by TROSSEN_SDK_ENABLE_POLICY_CLIENT).
#
# Brings in IXWebSocket and msgpack-cxx via FetchContent, then appends the PolicyClient
# source files to the existing trossen_sdk target. The gate is checked here so callers can
# unconditionally include() this file from the top-level CMakeLists.txt.

if(NOT TROSSEN_SDK_ENABLE_POLICY_CLIENT)
  return()
endif()

include(FetchContent)

message(STATUS "PolicyClient enabled - fetching IXWebSocket + msgpack-cxx")

set(_TROSSEN_POLICY_PREV_PIC ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# IXWebSocket: TLS disabled (openpi server is plain ws://); keep dependency surface minimal.
set(USE_TLS OFF CACHE BOOL "Disable TLS in IXWebSocket build" FORCE)
set(USE_ZLIB OFF CACHE BOOL "Disable zlib in IXWebSocket build" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  # Tag v11.4.5.
  GIT_TAG c5a02f1066fb0fde48f80f51178429a27f689a39
)
FetchContent_MakeAvailable(ixwebsocket)

# msgpack-cxx: header-only API; disable its own boost/examples knobs.
set(MSGPACK_USE_BOOST OFF CACHE BOOL "Disable Boost in msgpack-cxx" FORCE)
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "Skip msgpack-cxx tests" FORCE)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "Skip msgpack-cxx examples" FORCE)
FetchContent_Declare(
  msgpack-cxx
  GIT_REPOSITORY https://github.com/msgpack/msgpack-c.git
  # Tag cpp-6.1.1.
  GIT_TAG 44c0f705c9a60217d7e07de844fb13ce4c1c1e6e
)
FetchContent_MakeAvailable(msgpack-cxx)

set(CMAKE_POSITION_INDEPENDENT_CODE ${_TROSSEN_POLICY_PREV_PIC})

# --- LeRobot async_inference gRPC wiring -------------------------------------
# The proto is vendored byte-identical from the pinned LeRobot version the
# codec targets — v0.6.0, commit 30da8e68, src/lerobot/transport/services.proto.
# The local filename does not affect wire identity: gRPC method paths come from
# the proto package "transport", not the filename. C++ sources are
# generated at build time so they always match the system protobuf/grpc
# versions; committing generated code would pin the wrong thing.
pkg_check_modules(GRPCPP REQUIRED grpc++)
find_program(TROSSEN_PROTOC protoc REQUIRED)
find_program(TROSSEN_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(_lerobot_proto_dir ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/policy/proto)
set(_lerobot_proto ${_lerobot_proto_dir}/lerobot_transport_services.proto)
set(_lerobot_proto_out ${CMAKE_CURRENT_BINARY_DIR}/lerobot_proto)
file(MAKE_DIRECTORY ${_lerobot_proto_out})
set(_lerobot_proto_generated
  ${_lerobot_proto_out}/lerobot_transport_services.pb.cc
  ${_lerobot_proto_out}/lerobot_transport_services.pb.h
  ${_lerobot_proto_out}/lerobot_transport_services.grpc.pb.cc
  ${_lerobot_proto_out}/lerobot_transport_services.grpc.pb.h
  # generate_mock_code=true also emits MockAsyncInferenceStub here, so the
  # transport can be unit-tested against a gmock stub with no real channel.
  ${_lerobot_proto_out}/lerobot_transport_services_mock.grpc.pb.h
)
add_custom_command(
  OUTPUT ${_lerobot_proto_generated}
  COMMAND ${TROSSEN_PROTOC}
    -I ${_lerobot_proto_dir}
    --cpp_out=${_lerobot_proto_out}
    --grpc_out=generate_mock_code=true:${_lerobot_proto_out}
    --plugin=protoc-gen-grpc=${TROSSEN_GRPC_CPP_PLUGIN}
    ${_lerobot_proto}
  DEPENDS ${_lerobot_proto}
  COMMENT "Generating C++ from lerobot_transport_services.proto"
  VERBATIM
)

target_sources(trossen_sdk PRIVATE
  src/hw/policy/lerobot_codec.cpp
  src/hw/policy/lerobot_grpc_transport.cpp
  src/hw/policy/msgpack_ndarray.cpp
  src/hw/policy/openpi_websocket_transport.cpp
  src/hw/policy/transport_registry.cpp
  ${_lerobot_proto_out}/lerobot_transport_services.pb.cc
  ${_lerobot_proto_out}/lerobot_transport_services.grpc.pb.cc
)
target_compile_definitions(trossen_sdk PRIVATE TROSSEN_SDK_ENABLE_POLICY_CLIENT)
# The public LerobotGrpcTransport header includes the generated proto stub, so
# the generated dir and gRPC headers are PUBLIC. Build tree only: the SDK's
# headers are consumed in-tree (examples/tests/bindings), never installed.
target_include_directories(trossen_sdk PUBLIC $<BUILD_INTERFACE:${_lerobot_proto_out}>)
target_include_directories(trossen_sdk SYSTEM PUBLIC ${GRPCPP_INCLUDE_DIRS})

# msgpack-cxx is consumed only inside the .cpp files; keep it PRIVATE so the
# public headers and downstream targets are not forced to pull msgpack headers.
target_link_libraries(trossen_sdk PRIVATE
  ixwebsocket::ixwebsocket msgpack-cxx ${GRPCPP_LIBRARIES})
target_link_directories(trossen_sdk PRIVATE ${GRPCPP_LIBRARY_DIRS})
