#include "backend/session_actions.hpp"
#include "backend/config_manager.hpp"
#include "backend/hardware_helpers.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/arm/so101_arm_component.hpp"
#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/so101_teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/arm_producer.hpp"
#include <iostream>
#include <algorithm>

extern trossen::config::ConfigManager config_manager;

namespace trossen::backend {

SessionAction string_to_action(const std::string& action_str) {
    if (action_str == "teleop_so101") return SessionAction::TELEOP_SO101;
    if (action_str == "teleop_widowx") return SessionAction::TELEOP_WIDOWX;
    if (action_str == "record_cameras") return SessionAction::RECORD_CAMERAS_ONLY;

    throw std::runtime_error("Unknown session action: " + action_str);
}

std::string action_to_string(SessionAction action) {
    switch (action) {
        case SessionAction::TELEOP_SO101: return "teleop_so101";
        case SessionAction::TELEOP_WIDOWX: return "teleop_widowx";
        case SessionAction::RECORD_CAMERAS_ONLY: return "record_cameras";
        default: return "unknown";
    }
}

bool validate_hardware_for_action(
    SessionAction action,
    const std::string& system_id,
    std::string& error)
{
    // Get system configuration
    auto configs = config_manager.get_configurations();
    auto system_it = std::find_if(configs.systems.begin(), configs.systems.end(),
        [&system_id](const trossen::config::HardwareSystem& s) {
            return s.id == system_id;
        });

    if (system_it == configs.systems.end()) {
        error = "Hardware system not found: " + system_id;
        return false;
    }

    auto& system = *system_it;

    // Get all producers in the system
    // Note: system.producers contains producer IDs, and ProducerConfig.id contains the ID
    std::vector<trossen::config::ProducerConfig> system_producers;
    std::cout << "\n  Matching producers:" << std::endl;
    for (const auto& producer_id : system.producers) {
        std::cout << "    Looking for producer ID: '" << producer_id << "'" << std::endl;
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                // Match by stream_id which contains the producer ID from JSON
                return p.id == producer_id;
            });
        if (prod_it != configs.producers.end()) {
            system_producers.push_back(*prod_it);
            std::cout << "      ✓ FOUND: id='" << prod_it->id
                      << "' type='" << prod_it->type << "'" << std::endl;
        } else {
            std::cout << "      ✗ NOT FOUND in configs.producers" << std::endl;
        }
    }

    std::cout << "  Total system_producers matched: " << system_producers.size() << std::endl;
    std::cout << "=======================\n" << std::endl;

    switch (action) {
        case SessionAction::TELEOP_SO101: {
            // Needs 1 SO101 teleop producer with both leader and follower configured
            int so101_count = 0;
            bool has_valid_config = false;

            for (const auto& prod : system_producers) {
                if (prod.type == "teleop_so101_arm") {
                    so101_count++;

                    // Check that both leader and follower are configured
                    if (prod.leader_name.empty() || prod.follower_name.empty()) {
                        error = "SO101 teleop producer must have both "
                                "leader_name and follower_name configured";
                        return false;
                    }

                    // Verify the arms are connected
                    if (g_arm_status.find(prod.leader_name) == g_arm_status.end() ||
                        !g_arm_status[prod.leader_name].is_connected) {
                        error = "Leader arm not connected: " + prod.leader_name;
                        return false;
                    }
                    if (g_arm_status.find(prod.follower_name) == g_arm_status.end() ||
                        !g_arm_status[prod.follower_name].is_connected) {
                        error = "Follower arm not connected: " + prod.follower_name;
                        return false;
                    }

                    has_valid_config = true;
                }
            }

            if (so101_count != 1) {
                error = "SO101 teleoperation requires exactly 1 SO101 "
                        "teleop producer. Found: " + std::to_string(so101_count);
                return false;
            }

            if (!has_valid_config) {
                error = "SO101 teleop producer configuration is invalid";
                return false;
            }
            break;
        }

        case SessionAction::TELEOP_WIDOWX: {
            // Needs 1 WidowX teleop producer with both leader and follower configured
            int widowx_count = 0;
            bool has_valid_config = false;

            for (const auto& prod : system_producers) {
                if (prod.type == "teleop_arm") {
                    widowx_count++;

                    // Check that both leader and follower are configured
                    if (prod.leader_name.empty() || prod.follower_name.empty()) {
                        error = "WidowX teleop producer must have both "
                                "leader_name and follower_name configured";
                        return false;
                    }

                    // Verify the arms are connected
                    if (g_arm_status.find(prod.leader_name) == g_arm_status.end() ||
                        !g_arm_status[prod.leader_name].is_connected) {
                        error = "Leader arm not connected: " + prod.leader_name;
                        return false;
                    }
                    if (g_arm_status.find(prod.follower_name) == g_arm_status.end() ||
                        !g_arm_status[prod.follower_name].is_connected) {
                        error = "Follower arm not connected: " + prod.follower_name;
                        return false;
                    }

                    has_valid_config = true;
                }
            }

            if (widowx_count != 1) {
                error = "WidowX teleoperation requires exactly 1 WidowX "
                        "teleop producer. Found: " + std::to_string(widowx_count);
                return false;
            }

            if (!has_valid_config) {
                error = "WidowX teleop producer configuration is invalid";
                return false;
            }
            break;
        }

        case SessionAction::RECORD_CAMERAS_ONLY: {
            // Need at least one camera producer
            int camera_count = std::count_if(system_producers.begin(), system_producers.end(),
                [](const trossen::config::ProducerConfig& p) {
                    return p.type == "opencv_camera" ||
                           p.type == "realsense_color" ||
                           p.type == "realsense_depth";
                });

            if (camera_count == 0) {
                error = "Camera recording requires at least one camera producer";
                return false;
            }
            break;
        }

        default:
            error = "Unknown session action";
            return false;
    }

    return true;
}

bool setup_so101_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error)
{
    std::cout << "Setting up SO101 teleoperation session..." << std::endl;

    // Get system configuration
    auto configs = config_manager.get_configurations();
    auto system_it = std::find_if(configs.systems.begin(), configs.systems.end(),
        [&system_id](const trossen::config::HardwareSystem& s) {
            return s.id == system_id;
        });

    if (system_it == configs.systems.end()) {
        error = "Hardware system not found";
        return false;
    }

    // Find SO101 teleop producer
    for (const auto& producer_id : system_it->producers) {
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                return p.id == producer_id;
            });

        if (prod_it != configs.producers.end() && prod_it->type == "teleop_so101_arm") {
            const auto& prod = *prod_it;

            std::string leader_name = prod.leader_name;
            std::string follower_name = prod.follower_name;
            std::string id = prod.id;
            bool use_device_time = prod.use_device_time;

            // Get drivers from ActiveHardwareRegistry
            auto leader_comp = trossen::hw::ActiveHardwareRegistry::get(leader_name);
            auto follower_comp = trossen::hw::ActiveHardwareRegistry::get(follower_name);

            if (!leader_comp || !follower_comp) {
                error = "Failed to get SO101 components from registry";
                return false;
            }

            auto leader_so101 =
                std::dynamic_pointer_cast<trossen::hw::arm::SO101ArmComponent>(
                    leader_comp);
            auto follower_so101 =
                std::dynamic_pointer_cast<trossen::hw::arm::SO101ArmComponent>(
                    follower_comp);

            if (!leader_so101 || !follower_so101) {
                error = "Failed to cast to SO101ArmComponent";
                return false;
            }

            auto leader_driver = leader_so101->get_driver();
            auto follower_driver = follower_so101->get_driver();

            if (!leader_driver || !follower_driver) {
                error = "Failed to get SO101 drivers";
                return false;
            }

            // Create SO101 teleop producer
            trossen::hw::arm::TeleopSO101ArmProducer::Config teleop_cfg;
            teleop_cfg.stream_id = id;
            teleop_cfg.use_device_time = use_device_time;

            auto teleop_producer = std::make_shared<
                trossen::hw::arm::TeleopSO101ArmProducer>(
                    leader_driver, follower_driver, teleop_cfg);

            // Register with 30Hz polling rate
            auto teleop_period = std::chrono::milliseconds(33);  // ~30Hz
            active_session->manager->add_producer(teleop_producer, teleop_period);

            // Store drivers to keep them alive
            active_session->arm_drivers.push_back(leader_driver);
            active_session->arm_drivers.push_back(follower_driver);

            std::cout << "  ✓ Registered SO101 teleop producer: " << id << std::endl;

            // Give servos time to initialize after connection
            std::cout << "  Waiting for servos to initialize..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto test_leader = leader_driver->get_joint_positions(false);
            auto test_follower = follower_driver->get_joint_positions(false);

            // false = raw values
            auto leader_positions = leader_driver->get_joint_positions(false);
            std::cout << "  Leader RAW positions: [";
            for (size_t i = 0; i < leader_positions.size(); ++i) {
                std::cout << leader_positions[i];
                if (i < leader_positions.size() - 1) std::cout << ", ";
            }
            std::cout << "]" << std::endl;

            follower_driver->set_joint_positions(leader_positions, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "  ✓ Synced follower to leader position" << std::endl;

            // Start teleoperation thread (100Hz control loop)
            // This runs continuously across all episodes until manually
            // stopped
            active_session->teleop_active = true;
            active_session->teleop_thread = std::thread(
                [leader_driver, follower_driver, active_session]() {
                std::cout << "  ✓ Teleoperation loop started" << std::endl;

                int loop_count = 0;
                while (active_session->teleop_active) {
                    auto leader_positions = leader_driver->get_joint_positions(false);

                    follower_driver->set_joint_positions(leader_positions, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    loop_count++;
                }

                std::cout << "  ✓ Teleoperation loop stopped" << std::endl;
            });

            return true;
        }
    }

    error = "No SO101 teleop producer found in system";
    return false;
}

bool setup_widowx_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error)
{
    std::cout << "Setting up WidowX teleoperation session..." << std::endl;

    // Get system configuration
    auto configs = config_manager.get_configurations();
    auto system_it = std::find_if(configs.systems.begin(), configs.systems.end(),
        [&system_id](const trossen::config::HardwareSystem& s) {
            return s.id == system_id;
        });

    if (system_it == configs.systems.end()) {
        error = "Hardware system not found";
        return false;
    }

    // Find teleop_arm producer in configuration
    std::string teleop_stream_id;
    std::string leader_arm_id, follower_arm_id;
    bool use_device_time = true;

    for (const auto& producer_id : system_it->producers) {
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                return p.id == producer_id;
            });
        if (prod_it != configs.producers.end() && prod_it->type == "teleop_arm") {
            teleop_stream_id = prod_it->id;
            leader_arm_id = prod_it->leader_name;
            follower_arm_id = prod_it->follower_name;
            use_device_time = prod_it->use_device_time;
            break;
        }
    }

    if (teleop_stream_id.empty()) {
        error = "No teleop_arm producer found in system configuration";
        return false;
    }

    // Get hardware components from registry
    auto leader_comp = trossen::hw::ActiveHardwareRegistry::get(leader_arm_id);
    auto follower_comp = trossen::hw::ActiveHardwareRegistry::get(follower_arm_id);

    if (!leader_comp || !follower_comp) {
        error = "WidowX arms not found in hardware registry";
        return false;
    }

    // Cast to TrossenArmComponent and extract drivers
    auto leader_trossen =
        std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
            leader_comp);
    auto follower_trossen =
        std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(
            follower_comp);

    if (!leader_trossen || !follower_trossen) {
        error = "Failed to cast components to TrossenArmComponent";
        return false;
    }

    auto leader_driver = leader_trossen->get_hardware();
    auto follower_driver = follower_trossen->get_hardware();

    if (!leader_driver || !follower_driver) {
        error = "Failed to get arm drivers from components";
        return false;
    }

    // Store drivers in active session
    active_session->widowx_drivers.push_back(leader_driver);
    active_session->widowx_drivers.push_back(follower_driver);

    // Create and register the teleop producer
    trossen::hw::arm::TeleopTrossenArmProducer::Config teleop_cfg;
    teleop_cfg.stream_id = teleop_stream_id;
    teleop_cfg.use_device_time = use_device_time;

    auto teleop_producer = std::make_shared<
        trossen::hw::arm::TeleopTrossenArmProducer>(
            leader_driver, follower_driver, teleop_cfg);

    active_session->manager->add_producer(
        teleop_producer, std::chrono::milliseconds(1));
    std::cout << "  ✓ Registered WidowX teleop producer: " << teleop_stream_id << std::endl;

    // Start teleoperation thread with freeze logic
    active_session->teleop_active = true;
    active_session->teleop_thread = std::thread([active_session]() {
        std::cout << "Starting WidowX teleoperation thread..." << std::endl;

        auto leader_driver = active_session->widowx_drivers[0];
        auto follower_driver = active_session->widowx_drivers[1];

        while (active_session->teleop_active) {
            // Check if we should freeze (waiting for next episode)
            if (active_session->waiting_for_next.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            try {
                // Mirror leader arm positions to follower
                auto positions = leader_driver->get_all_positions();
                follower_driver->set_all_positions(positions, 0.0f, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } catch (const std::exception& e) {
                std::cerr << "Error in WidowX teleop thread: " << e.what() << std::endl;
                break;
            }
        }

        std::cout << "WidowX teleoperation thread stopped" << std::endl;
    });

    return true;
}

bool setup_camera_recording(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error)
{
    std::cout << "Setting up camera recording session..." << std::endl;

    // Get system configuration
    auto configs = config_manager.get_configurations();
    auto system_it = std::find_if(configs.systems.begin(), configs.systems.end(),
        [&system_id](const trossen::config::HardwareSystem& s) {
            return s.id == system_id;
        });

    if (system_it == configs.systems.end()) {
        error = "Hardware system not found";
        return false;
    }

    int producers_added = 0;

    // Instantiate all camera producers in the system
    for (const auto& producer_id : system_it->producers) {
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                return p.id == producer_id;
            });

        if (prod_it == configs.producers.end()) continue;

        const auto& prod = *prod_it;

        if (prod.type == "opencv_camera") {
            // TODO(trossen): Implement OpenCV camera producer instantiation
            // Need to get camera component from ActiveHardwareRegistry
            // Create OpenCvCameraProducer and register with SessionManager
            std::cout << "OpenCV camera producer not yet implemented: "
                      << prod.id << std::endl;
        } else if (prod.type == "realsense_color" ||
                   prod.type == "realsense_depth") {
            std::cout << "RealSense camera producer not yet implemented: "
                      << prod.id << std::endl;
        }
    }

    if (producers_added == 0) {
        error = "No camera producers were instantiated";
        return false;
    }

    std::cout << "  ✓ Registered " << producers_added << " camera producers" << std::endl;
    return true;
}



}  // namespace trossen::backend
