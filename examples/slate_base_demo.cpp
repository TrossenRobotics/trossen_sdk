// Copyright 2025 Trossen Robotics
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/**
 * @file slate_base_demo.cpp
 * @brief Demo showcasing SLATE mobile base hardware component usage
 *
 * This demo demonstrates how to:
 * 1. Create and configure a SLATE base hardware component
 * 2. Register it with the ActiveHardwareRegistry
 * 3. Control the base using velocity commands
 * 4. Monitor base state (odometry, battery, etc.)
 */

#include <chrono>
#include <iostream>
#include <thread>

#include "trossen_sdk/hw/base/slate_base_component.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"

int main() {
  try {
    std::cout << "=== SLATE Base Hardware Component Demo ===" << std::endl;

    // Create a SLATE base hardware component
    auto slate_base = std::make_shared<trossen::hw::base::SlateBaseComponent>("slate_base_1");

    // Configure the base
    nlohmann::json config = {
      {"reset_odometry", false},
      {"enable_torque", true},
      {"enable_charging", false}
    };

    std::cout << "\nConfiguring SLATE base..." << std::endl;
    slate_base->configure(config);

    // Register with the active hardware registry
    trossen::hw::ActiveHardwareRegistry::register_active("slate_base_1", slate_base);
    std::cout << "\nSLATE base registered with ActiveHardwareRegistry" << std::endl;

    // Display component info
    std::cout << "\nComponent Information:" << std::endl;
    std::cout << slate_base->get_info().dump(2) << std::endl;

    // Get the underlying driver for direct control
    auto driver = slate_base->get_driver();

    // Main control loop
    std::cout << "\n=== Starting Control Loop ===" << std::endl;
    std::cout << "The base will rotate slowly while displaying telemetry" << std::endl;
    std::cout << "Press Ctrl+C to exit\n" << std::endl;

    int loop_count = 0;
    const int max_loops = 25;  // Run for 25 iterations (~2.5 seconds)

    while (loop_count < max_loops) {
      // Set velocity command: rotate in place
      base_driver::ChassisData cmd_data = {};
      cmd_data.cmd_vel_x = 0.0f;
      cmd_data.cmd_vel_y = 0.0f;
      cmd_data.cmd_vel_z = -0.2f;  // Slow rotation
      cmd_data.light_state = static_cast<uint32_t>(LightState::WHITE);

      // Write command to base
      driver->write(cmd_data);

      // Read current state
      base_driver::ChassisData read_data;
      driver->read(read_data);

      // Display telemetry
      std::cout << "\r";  // Carriage return for in-place update
      std::cout << "Charge: " << read_data.charge << "% | "
                << "Linear: " << read_data.vel_x << " m/s | "
                << "Angular: " << read_data.vel_z << " rad/s | "
                << "X: " << read_data.odom_x << " m | "
                << "Y: " << read_data.odom_y << " m | "
                << "Theta: " << read_data.odom_z << " rad" << std::flush;

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      loop_count++;
    }

    std::cout << std::endl;

    // Stop the base
    std::cout << "\nStopping the base..." << std::endl;
    base_driver::ChassisData stop_data = {};
    stop_data.cmd_vel_x = 0.0f;
    stop_data.cmd_vel_y = 0.0f;
    stop_data.cmd_vel_z = 0.0f;
    stop_data.light_state = static_cast<uint32_t>(LightState::GREEN);
    driver->write(stop_data);

    // Disable motor torque
    std::cout << "Disabling motor torque..." << std::endl;
    std::string torque_result;
    if (!driver->enable_motor_torque(false, torque_result)) {
      std::cerr << "Warning: Failed to disable motor torque: " << torque_result << std::endl;
    } else {
      std::cout << "Motor torque disabled: " << torque_result << std::endl;
    }

    // Display final info
    std::cout << "\nFinal Component Information:" << std::endl;
    std::cout << slate_base->get_info().dump(2) << std::endl;

    std::cout << "\n=== Demo Complete ===" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
