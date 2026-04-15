/**
 * @file trossen_sdk.hpp
 * @brief Convenience include - pulls in all public Trossen SDK headers.
 *
 * Including this single header gives access to every component:
 * hardware, producers, backends, session manager, and configuration.
 *
 * RealSense headers are included only when the library is built with
 * -DTROSSEN_ENABLE_REALSENSE=ON (the preprocessor symbol
 * TROSSEN_ENABLE_REALSENSE is defined by the build system).
 */

#ifndef TROSSEN_SDK__TROSSEN_SDK_HPP
#define TROSSEN_SDK__TROSSEN_SDK_HPP

// -- Version ------------------------------------------------------------------
#include "trossen_sdk/version.hpp"
#include "trossen_sdk/types.hpp"

// -- Data records and timestamps -----------------------------------------------
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"

// -- Hardware abstraction ------------------------------------------------------
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

// Arms
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_producer.hpp"
#include "trossen_sdk/hw/arm/so101_arm_component.hpp"
#include "trossen_sdk/hw/arm/mock_joint_producer.hpp"

// Cameras
#include "trossen_sdk/hw/camera/camera_types.hpp"
#include "trossen_sdk/hw/camera/camera_streams_config.hpp"
#include "trossen_sdk/hw/camera/opencv_camera_component.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"
#include "trossen_sdk/hw/camera/mock_synced_producer.hpp"

#ifdef TROSSEN_ENABLE_REALSENSE
#include "trossen_sdk/hw/camera/realsense_camera_component.hpp"
#include "trossen_sdk/hw/camera/realsense_push_producer.hpp"
#endif

// Mobile base
#include "trossen_sdk/hw/base/slate_base_component.hpp"
#include "trossen_sdk/hw/base/slate_base_producer.hpp"

// -- I/O and backends ----------------------------------------------------------
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/queue_adapter.hpp"
#include "trossen_sdk/io/sink.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_backend.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"

// -- Runtime -------------------------------------------------------------------
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/runtime/scheduler.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

// -- Configuration -------------------------------------------------------------
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

// Runtime config
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"

// Backend configs
#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/null_backend_config.hpp"

// Hardware configs
#include "trossen_sdk/configuration/types/hardware/arm_config.hpp"
#include "trossen_sdk/configuration/types/hardware/camera_config.hpp"
#include "trossen_sdk/configuration/types/hardware/mobile_base_config.hpp"

// Producer configs
#include "trossen_sdk/configuration/types/producers/producer_config.hpp"
#include "trossen_sdk/configuration/types/producers/arm_producer_config.hpp"
#include "trossen_sdk/configuration/types/producers/camera_producer_config.hpp"
#include "trossen_sdk/configuration/types/producers/mobile_base_producer_config.hpp"

// Teleop config
#include "trossen_sdk/configuration/types/teleop_config.hpp"

#endif  // TROSSEN_SDK__TROSSEN_SDK_HPP
