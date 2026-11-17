/**
 * @file test_app_utils.cpp
 * @brief Unit tests for the shared application utilities
 */

#include <csignal>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/app_utils.hpp"

// SIGTERM, as sent by kill or a service stop, requests the same clean stop as Ctrl+C.
TEST(SignalHandler, SigtermRequestsStop) {
  trossen::utils::g_stop_requested.store(false);
  trossen::utils::install_signal_handler();

  ASSERT_EQ(std::raise(SIGTERM), 0);
  EXPECT_TRUE(trossen::utils::g_stop_requested.load());

  std::signal(SIGTERM, SIG_DFL);
  std::signal(SIGINT, SIG_DFL);
  trossen::utils::g_stop_requested.store(false);
}
