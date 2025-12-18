/**
 * @file trossen_sdk.hpp
 * @brief Main include file for Trossen SDK
 */

#ifndef TROSSEN_SDK__TROSSEN_SDK_HPP
#define TROSSEN_SDK__TROSSEN_SDK_HPP

#include "trossen_sdk/version.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/hw/arm/arm_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"

#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/trossen/trossen_backend.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_schemas.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"
#include "trossen_sdk/io/queue_adapter.hpp"
#include "trossen_sdk/io/sink.hpp"

#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/runtime/scheduler.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"
#include "trossen_sdk/configuration/types/backends/mcap_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/null_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/trossen_backend_config.hpp"

#endif  // TROSSEN_SDK__TROSSEN_SDK_HPP
