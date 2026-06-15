# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Unit test executables
add_executable(test_timestamp test_timestamp.cpp)
target_link_libraries(test_timestamp PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_record test_record.cpp)
target_link_libraries(test_record PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_null_backend test_null_backend.cpp)
target_link_libraries(test_null_backend PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_backend_registry test_backend_registry.cpp)
target_link_libraries(test_backend_registry PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_trossen_mcap_backend test_trossen_mcap_backend.cpp)
target_link_libraries(test_trossen_mcap_backend PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_active_hardware_registry test_active_hardware_registry.cpp)
target_link_libraries(test_active_hardware_registry PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_push_producer_registry test_push_producer_registry.cpp)
target_link_libraries(test_push_producer_registry PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_push_producer_base test_push_producer_base.cpp)
target_link_libraries(test_push_producer_base PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_session_manager_push_producer test_session_manager_push_producer.cpp)
target_link_libraries(test_session_manager_push_producer PRIVATE trossen_sdk GTest::gtest_main)

if(TROSSEN_ENABLE_REALSENSE)
  add_executable(test_realsense_push_producer_metadata test_realsense_push_producer_metadata.cpp)
  target_link_libraries(test_realsense_push_producer_metadata PRIVATE trossen_sdk GTest::gtest_main)
  target_compile_definitions(test_realsense_push_producer_metadata PRIVATE TROSSEN_ENABLE_REALSENSE)
endif()

add_executable(test_lerobot_depth_utils test_lerobot_depth_utils.cpp)
target_link_libraries(test_lerobot_depth_utils PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_record_types test_record_types.cpp)
target_link_libraries(test_record_types PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_lerobot_utils test_lerobot_utils.cpp)
target_link_libraries(test_lerobot_utils PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_queue_adapter test_queue_adapter.cpp)
target_link_libraries(test_queue_adapter PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_producer_registry test_producer_registry.cpp)
target_link_libraries(test_producer_registry PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_sink test_sink.cpp)
target_link_libraries(test_sink PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_session_manager test_session_manager.cpp)
target_link_libraries(test_session_manager PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_session_manager_discard test_session_manager_discard.cpp)
target_link_libraries(test_session_manager_discard PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_keyboard_input_utils test_keyboard_input_utils.cpp)
target_link_libraries(test_keyboard_input_utils PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_announce test_announce.cpp)
target_link_libraries(test_announce PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_teleop_space_helpers test_teleop_space_helpers.cpp)
target_link_libraries(test_teleop_space_helpers PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_teleop_controller test_teleop_controller.cpp)
target_link_libraries(test_teleop_controller PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_teleop_config test_teleop_config.cpp)
target_link_libraries(test_teleop_config PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_observer_base test_observer_base.cpp)
target_link_libraries(test_observer_base PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_observer_registry test_observer_registry.cpp)
target_link_libraries(test_observer_registry PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_session_manager_observers test_session_manager_observers.cpp)
target_link_libraries(test_session_manager_observers PRIVATE trossen_sdk GTest::gtest_main)

if(TROSSEN_ENABLE_RERUN_OBSERVER)
  add_executable(test_rerun_observer test_rerun_observer.cpp)
  # rerun_sdk is PRIVATE to trossen_sdk so its include dirs aren't propagated; the
  # lifetime-stress test constructs ``rerun::archetypes::Image`` directly, so link the
  # SDK here too to pull in the public headers.
  target_link_libraries(test_rerun_observer PRIVATE trossen_sdk rerun_sdk GTest::gtest_main)
endif()

add_executable(test_policy_client_config test_policy_client_config.cpp)
target_link_libraries(test_policy_client_config PRIVATE trossen_sdk GTest::gtest_main)

if(TROSSEN_SDK_ENABLE_POLICY_CLIENT)
  add_executable(test_msgpack_ndarray test_msgpack_ndarray.cpp)
  target_link_libraries(test_msgpack_ndarray PRIVATE trossen_sdk GTest::gtest_main msgpack-cxx)

  add_executable(test_openpi_websocket_transport test_openpi_websocket_transport.cpp)
  target_link_libraries(test_openpi_websocket_transport PRIVATE
    trossen_sdk GTest::gtest_main msgpack-cxx ixwebsocket::ixwebsocket)

  add_executable(test_policy_client test_policy_client.cpp)
  target_link_libraries(test_policy_client PRIVATE trossen_sdk GTest::gtest_main)

  add_executable(test_policy_client_producer test_policy_client_producer.cpp)
  target_link_libraries(test_policy_client_producer PRIVATE trossen_sdk GTest::gtest_main)

  add_executable(test_lerobot_codec test_lerobot_codec.cpp)
  target_link_libraries(test_lerobot_codec PRIVATE trossen_sdk GTest::gtest_main)
  # Absolute fixture path baked in: the test must pass regardless of ctest's cwd.
  target_compile_definitions(test_lerobot_codec PRIVATE
    LEROBOT_CODEC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lerobot_codec")

  # Drives LerobotGrpcTransport against a gmock MockAsyncInferenceStub (no real
  # channel/server, so no gRPC teardown race). Needs the generated proto + mock
  # headers, gmock, and a direct gRPC/protobuf link (PRIVATE on trossen_sdk).
  add_executable(test_lerobot_grpc_transport test_lerobot_grpc_transport.cpp)
  target_link_libraries(test_lerobot_grpc_transport PRIVATE
    trossen_sdk GTest::gtest_main GTest::gmock ${GRPCPP_LIBRARIES} ${Protobuf_LIBRARIES})
  target_link_directories(test_lerobot_grpc_transport PRIVATE ${GRPCPP_LIBRARY_DIRS})
  target_include_directories(test_lerobot_grpc_transport PRIVATE
    # Generated mock-stub header lives in the build tree.
    ${CMAKE_BINARY_DIR}/lerobot_proto)
  # Replays a pinned pickled action chunk through the mock stub's GetActions.
  target_compile_definitions(test_lerobot_grpc_transport PRIVATE
    LEROBOT_CODEC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lerobot_codec")
endif()

add_executable(test_action_chunk test_action_chunk.cpp)
target_link_libraries(test_action_chunk PRIVATE trossen_sdk GTest::gtest_main)

add_executable(test_active_hardware_registry test_active_hardware_registry.cpp)
target_link_libraries(test_active_hardware_registry PRIVATE trossen_sdk GTest::gtest_main)

# Enable CTest integration
include(GoogleTest)
gtest_discover_tests(test_timestamp)
gtest_discover_tests(test_record)
gtest_discover_tests(test_null_backend)
gtest_discover_tests(test_backend_registry)
gtest_discover_tests(test_trossen_mcap_backend)
gtest_discover_tests(test_active_hardware_registry)
gtest_discover_tests(test_push_producer_registry)
gtest_discover_tests(test_push_producer_base)
gtest_discover_tests(test_session_manager_push_producer)
if(TROSSEN_ENABLE_REALSENSE)
  gtest_discover_tests(test_realsense_push_producer_metadata)
endif()
gtest_discover_tests(test_lerobot_depth_utils)
gtest_discover_tests(test_record_types)
gtest_discover_tests(test_config)
gtest_discover_tests(test_lerobot_utils)
gtest_discover_tests(test_queue_adapter)
gtest_discover_tests(test_producer_registry)
gtest_discover_tests(test_sink)
gtest_discover_tests(test_scheduler)
gtest_discover_tests(test_session_manager)
gtest_discover_tests(test_session_manager_discard)
gtest_discover_tests(test_keyboard_input_utils)
gtest_discover_tests(test_announce)
gtest_discover_tests(test_teleop_space_helpers)
gtest_discover_tests(test_teleop_controller)
gtest_discover_tests(test_teleop_config)
gtest_discover_tests(test_observer_base)
gtest_discover_tests(test_observer_registry)
gtest_discover_tests(test_session_manager_observers)
if(TROSSEN_ENABLE_RERUN_OBSERVER)
  gtest_discover_tests(test_rerun_observer)
endif()
gtest_discover_tests(test_policy_client_config)
gtest_discover_tests(test_action_chunk)
gtest_discover_tests(test_active_hardware_registry)
if(TROSSEN_SDK_ENABLE_POLICY_CLIENT)
  gtest_discover_tests(test_policy_client)
  gtest_discover_tests(test_policy_client_producer)
  gtest_discover_tests(test_lerobot_codec)
  gtest_discover_tests(test_lerobot_grpc_transport)
endif()
