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
if(TROSSEN_SDK_ENABLE_POLICY_CLIENT)
  add_executable(trossen_stationary_ai_policy
    trossen_stationary_ai_policy/trossen_stationary_ai_policy.cpp)
  target_link_libraries(trossen_stationary_ai_policy trossen_sdk libtrossen_arm)

  # LeRobot sibling: same bimanual setup driven by a LeRobot async_inference
  # gRPC server (transport lerobot_grpc) instead of openpi WebSocket.
  add_executable(trossen_stationary_ai_lerobot
    trossen_stationary_ai_lerobot/trossen_stationary_ai_lerobot.cpp)
  target_link_libraries(trossen_stationary_ai_lerobot trossen_sdk libtrossen_arm)
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
