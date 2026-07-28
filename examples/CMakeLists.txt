find_package(libtrossen_arm REQUIRED)

# Trossen Solo AI — 1 leader + 1 follower arm + 2 cameras
add_executable(trossen_solo_ai trossen_solo_ai/trossen_solo_ai.cpp)
target_link_libraries(trossen_solo_ai trossen_sdk libtrossen_arm)

# Trossen Stationary AI — bimanual (2 leader + 2 follower) + 4 cameras
add_executable(trossen_stationary_ai trossen_stationary_ai/trossen_stationary_ai.cpp)
target_link_libraries(trossen_stationary_ai trossen_sdk libtrossen_arm)

# Trossen Mobile AI — bimanual + SLATE mobile base + 3 cameras
add_executable(trossen_mobile_ai trossen_mobile_ai/trossen_mobile_ai.cpp)
target_link_libraries(trossen_mobile_ai trossen_sdk libtrossen_arm)

# Trossen Stationary AI Policy — 2 followers + 4 cameras, policy-driven leaders.
# Requires the PolicyClient gate (FetchContent of IXWebSocket + msgpack-cxx).
#
# One transport-agnostic source builds into two binaries; they differ only in
# the default config (which selects the transport) and a display label, both
# injected via compile definitions. openpi_ws vs lerobot_grpc is a config
# concern, not a code concern.
if(TROSSEN_SDK_ENABLE_POLICY_CLIENT)
  set(_policy_example_src
    trossen_stationary_ai_policy/trossen_stationary_ai_policy.cpp)

  # openpi WebSocket variant.
  add_executable(trossen_stationary_ai_openpi ${_policy_example_src})
  target_link_libraries(trossen_stationary_ai_openpi trossen_sdk libtrossen_arm)
  target_compile_definitions(trossen_stationary_ai_openpi PRIVATE
    TROSSEN_AI_EXAMPLE_CONFIG="examples/trossen_stationary_ai_policy/configs/openpi.json"
    TROSSEN_AI_EXAMPLE_LABEL="openpi")

  # LeRobot async_inference gRPC variant (same code, different config).
  add_executable(trossen_stationary_ai_lerobot ${_policy_example_src})
  target_link_libraries(trossen_stationary_ai_lerobot trossen_sdk libtrossen_arm)
  target_compile_definitions(trossen_stationary_ai_lerobot PRIVATE
    TROSSEN_AI_EXAMPLE_CONFIG="examples/trossen_stationary_ai_policy/configs/lerobot.json"
    TROSSEN_AI_EXAMPLE_LABEL="LeRobot")
endif()

# Registry demo executable
add_executable(backend_registry_demo backend_registry_demo.cpp)
target_link_libraries(backend_registry_demo trossen_sdk)

add_executable(hardware_registry_demo hardware_registry_demo.cpp)
target_link_libraries(hardware_registry_demo trossen_sdk)

add_executable(producer_registry_demo producer_registry_demo.cpp)
target_link_libraries(producer_registry_demo trossen_sdk)

# Lifecycle callback demo (no hardware required)
add_executable(callback_demo callback_demo/callback_demo.cpp)
target_link_libraries(callback_demo trossen_sdk)
