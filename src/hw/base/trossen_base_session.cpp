/**
 * @file trossen_base_session.cpp
 * @brief Implementation of TrossenBaseSession
 */

#include "trossen_sdk/hw/base/trossen_base_session.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

#include <optional>
#include <string>

namespace trossen::hw::trossen_base {


TrossenBaseSession& TrossenBaseSession::instance() {}


void TrossenBaseSession::ensure_started(std::string ip) {}


void TrossenBaseSession::release(){}


}  // namespace trossen::hw::trossen_base
