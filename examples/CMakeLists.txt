find_package(libtrossen_arm REQUIRED)

# Trossen Solo AI — 1 leader + 1 follower arm + 2 cameras
add_executable(trossen_solo_ai trossen_solo_ai/trossen_solo_ai.cpp)
target_link_libraries(trossen_solo_ai trossen_sdk libtrossen_arm)

# Trossen Stationary AI — bimanual (2 leader + 2 follower) + 4 cameras
add_executable(trossen_stationary_ai trossen_stationary_ai/trossen_stationary_ai.cpp)
target_link_libraries(trossen_stationary_ai trossen_sdk libtrossen_arm)
# Expose the Adamo gate so the example can guard its AdamoObserver-specific
# teardown path (the SDK target defines this PRIVATE, so it does not propagate).
if(TROSSEN_ENABLE_ADAMO)
  target_compile_definitions(trossen_stationary_ai PRIVATE TROSSEN_ENABLE_ADAMO)
endif()

# Trossen Mobile AI — bimanual + SLATE mobile base + 3 cameras
add_executable(trossen_mobile_ai trossen_mobile_ai/trossen_mobile_ai.cpp)
target_link_libraries(trossen_mobile_ai trossen_sdk libtrossen_arm)

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

# Adamo smoke test — only built when the Adamo gate is on. Pure SDK probe:
# opens adamo::Session against the configured backend and exits. Useful to
# validate ADAMO_API_KEY / network reachability before running anything that
# pulls in the full SessionManager + arm hardware.
if(TROSSEN_ENABLE_ADAMO)
  add_executable(adamo_smoke_test trossen_adamo_remote/adamo_smoke_test.cpp)
  target_link_libraries(adamo_smoke_test PRIVATE trossen_adamo_common)

  # Remote-teleop leader/publisher: drives the local leader arm(s) into gravity-comp
  # and streams joint state to Adamo directly (adamo::Session + trossen_adamo::wire,
  # no SessionManager/observer). Links trossen_adamo_common for those headers plus
  # trossen_sdk (TrossenArmComponent) and libtrossen_arm (the driver).
  add_executable(trossen_adamo_remote_leader
    trossen_adamo_remote/trossen_adamo_remote_leader.cpp)
  target_link_libraries(trossen_adamo_remote_leader
    trossen_sdk trossen_adamo_common libtrossen_arm)

  # Remote-teleop follower: drives a local follower arm from a RemoteAdamoLeaderArm
  # fed by the leader host's AdamoObserver. Gated on the Adamo flag because the
  # 'remote_adamo_leader' hardware type is only compiled into trossen_sdk here.
  add_executable(trossen_adamo_remote_follower
    trossen_adamo_remote/trossen_adamo_remote_follower.cpp)
  # trossen_adamo_common: the follower opens its own adamo::Session for the
  # lifecycle control channel (adamo_lifecycle.hpp) in addition to the observer path.
  target_link_libraries(trossen_adamo_remote_follower
    trossen_sdk trossen_adamo_common libtrossen_arm)
  # Expose the gate so the binary can guard its AdamoObserver camera-pipeline
  # teardown path (the SDK defines TROSSEN_ENABLE_ADAMO PRIVATE, so it does not propagate).
  target_compile_definitions(trossen_adamo_remote_follower PRIVATE TROSSEN_ENABLE_ADAMO)
endif()
