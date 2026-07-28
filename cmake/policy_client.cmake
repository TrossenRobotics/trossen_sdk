# PolicyClient dependencies and source wiring (gated by TROSSEN_SDK_ENABLE_POLICY_CLIENT).
#
# Brings in the codec/transport dependencies via FetchContent, then appends the
# PolicyClient source files to the existing trossen_sdk target. The gate is checked
# here so callers can unconditionally include() this file from the top-level
# CMakeLists.txt. Sources are added by the layer that introduces them; this layer
# wires the wire-format codecs (openpi msgpack-numpy + LeRobot pickle).

if(NOT TROSSEN_SDK_ENABLE_POLICY_CLIENT)
  return()
endif()

include(FetchContent)

message(STATUS "PolicyClient enabled - fetching msgpack-cxx")

set(_TROSSEN_POLICY_PREV_PIC ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# Wire-format codecs: openpi msgpack-numpy ndarray + LeRobot pickle/torch.
target_sources(trossen_sdk PRIVATE
  src/hw/policy/lerobot_codec.cpp
  src/hw/policy/msgpack_ndarray.cpp
)
target_compile_definitions(trossen_sdk PRIVATE TROSSEN_SDK_ENABLE_POLICY_CLIENT)

# msgpack-cxx is consumed only inside the .cpp files; keep it PRIVATE.
target_link_libraries(trossen_sdk PRIVATE msgpack-cxx)
