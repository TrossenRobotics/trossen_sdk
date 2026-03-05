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
#include "trossen_sdk/hw/arm/trossen_arm_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include <iostream>
#include <algorithm>
#include <string>

extern trossen::config::ConfigManager config_manager;

namespace trossen::backend {

SessionAction string_to_action(const std::string& action_str) {
    if (action_str == "teleop_so101") return SessionAction::TELEOP_SO101;
    if (action_str == "teleop_widowx") return SessionAction::TELEOP_WIDOWX;
    if (action_str == "teleop_widowx_bimanual") return SessionAction::TELEOP_WIDOWX_BIMANUAL;

    throw std::runtime_error("Unknown session action: " + action_str);
}

std::string action_to_string(SessionAction action) {
    switch (action) {
        case SessionAction::TELEOP_SO101: return "teleop_so101";
        case SessionAction::TELEOP_WIDOWX: return "teleop_widowx";
        case SessionAction::TELEOP_WIDOWX_BIMANUAL: return "teleop_widowx_bimanual";
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
            std::cout << "      [ok] FOUND: id='" << prod_it->id
                      << "' type='" << prod_it->type << "'" << std::endl;
        } else {
            std::cout << "      [FAILED] NOT FOUND in configs.producers" << std::endl;
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
                    if (prod.leader_id.empty() || prod.follower_id.empty()) {
                        error = "SO101 teleop producer must have both "
                                "leader_id and follower_id configured";
                        return false;
                    }

                    // Verify the arms are connected by ID
                    if (g_arm_status.find(prod.leader_id) == g_arm_status.end() ||
                        !g_arm_status[prod.leader_id].is_connected) {
                        error = "Leader arm not connected: " + prod.leader_id;
                        return false;
                    }
                    if (g_arm_status.find(prod.follower_id) == g_arm_status.end() ||
                        !g_arm_status[prod.follower_id].is_connected) {
                        error = "Follower arm not connected: " + prod.follower_id;
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

            std::cout << "  Validating TELEOP_WIDOWX: checking "
                      << system_producers.size() << " producers" << std::endl;
            for (const auto& prod : system_producers) {
                std::cout << "    Producer type: '" << prod.type << "'" << std::endl;
                if (prod.type == "teleop_widowx_arm") {
                    widowx_count++;
                    std::cout << "    [ok] Found WidowX teleop producer!" << std::endl;

                    // Check that both leader and follower are configured
                    if (prod.leader_id.empty() || prod.follower_id.empty()) {
                        error = "WidowX teleop producer must have both "
                                "leader_id and follower_id configured";
                        return false;
                    }

                    // Verify the arms are connected by ID
                    if (g_arm_status.find(prod.leader_id) == g_arm_status.end() ||
                        !g_arm_status[prod.leader_id].is_connected) {
                        error = "Leader arm not connected: " + prod.leader_id;
                        return false;
                    }
                    if (g_arm_status.find(prod.follower_id) == g_arm_status.end() ||
                        !g_arm_status[prod.follower_id].is_connected) {
                        error = "Follower arm not connected: " + prod.follower_id;
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

        case SessionAction::TELEOP_WIDOWX_BIMANUAL: {
            // Needs 2 WidowX teleop producers (2 pairs of arms)
            int widowx_count = 0;
            std::vector<std::string> all_arms;

            for (const auto& prod : system_producers) {
                if (prod.type == "teleop_widowx_arm") {
                    widowx_count++;

                    // Check that both leader and follower are configured
                    if (prod.leader_id.empty() || prod.follower_id.empty()) {
                        error = "WidowX teleop producer must have both "
                                "leader_id and follower_id configured";
                        return false;
                    }

                    // Verify the arms are connected by ID
                    if (g_arm_status.find(prod.leader_id) == g_arm_status.end() ||
                        !g_arm_status[prod.leader_id].is_connected) {
                        error = "Leader arm not connected: " + prod.leader_id;
                        return false;
                    }
                    if (g_arm_status.find(prod.follower_id) == g_arm_status.end() ||
                        !g_arm_status[prod.follower_id].is_connected) {
                        error = "Follower arm not connected: " + prod.follower_id;
                        return false;
                    }

                    all_arms.push_back(prod.leader_id);
                    all_arms.push_back(prod.follower_id);
                }
            }

            if (widowx_count != 2) {
                error = "WidowX bimanual teleoperation requires exactly 2 WidowX "
                        "teleop producers. Found: " + std::to_string(widowx_count);
                return false;
            }

            // Verify no duplicate arms
            std::sort(all_arms.begin(), all_arms.end());
            if (std::adjacent_find(all_arms.begin(), all_arms.end()) != all_arms.end()) {
                error = "WidowX bimanual teleoperation: duplicate arm names detected";
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

    std::shared_ptr<SO101ArmDriver> leader_driver;
    std::shared_ptr<SO101ArmDriver> follower_driver;
    std::string teleop_stream_id;
    bool use_device_time = true;

    // Loop through ALL producers in the system
    std::cout << "Creating producers from system configuration..." << std::endl;
    int camera_count = 0;
    int arm_count = 0;

    for (const auto& producer_id : system_it->producers) {
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                return p.id == producer_id;
            });

        if (prod_it == configs.producers.end()) continue;

        const auto& prod = *prod_it;

        // Create camera producers
        if (prod.type == "opencv_camera") {
            // Look up camera configuration by ID
            auto cam_it = std::find_if(configs.cameras.begin(), configs.cameras.end(),
                [&prod](const trossen::config::CameraConfig& c) {
                    return c.id == prod.camera_id;
                });

            if (cam_it == configs.cameras.end()) {
                std::cerr << "Warning: Camera not found for producer: "
                          << prod.camera_id << std::endl;
                continue;
            }

            const auto& cam = *cam_it;
            trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
            cam_cfg.device_index = cam.device_index;
            cam_cfg.stream_id = "opencv_camera_" + std::to_string(cam.device_index);
            cam_cfg.width = cam.width;
            cam_cfg.height = cam.height;
            cam_cfg.height = cam.height;
            cam_cfg.fps = cam.fps;
            cam_cfg.use_device_time = prod.use_device_time;

            // TODO(user): Once OpenCvCameraProducer supports taking
            // OpenCvCameraComponent directly, use the already-connected camera from
            // ActiveHardwareRegistry (like arm producers do) to avoid device-busy
            // errors from attempting to open camera twice.
            auto camera_producer = std::make_shared<
                trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);

            // Replace the above with this once
            // OpenCvCameraProducer(shared_ptr<OpenCvCameraComponent>) is available:
            // auto hw_component = trossen::hw::ActiveHardwareRegistry::get(prod.camera_id);
            // auto camera_component = std::dynamic_pointer_cast<
            //     trossen::hw::camera::OpenCvCameraComponent>(hw_component);
            // if (!camera_component) {
            //     std::cerr << "Failed to get camera component from registry" << std::endl;
            //     continue;
            // }
            // auto camera_producer = std::make_shared<
            //     trossen::hw::camera::OpenCvCameraProducer>(camera_component);

            auto camera_period = std::chrono::milliseconds(
                static_cast<int>(1000.0f / cam.fps));
            active_session->manager->add_producer(
                camera_producer, camera_period);
            camera_count++;
            std::cout << "  [ok] Registered camera producer: "
                      << cam.name << std::endl;
        } else if (prod.type == "teleop_so101_arm") {
            // Handle SO101 teleop producer
            teleop_stream_id = prod.id;
            use_device_time = prod.use_device_time;

            // Get drivers from ActiveHardwareRegistry using IDs
            auto leader_comp = trossen::hw::ActiveHardwareRegistry::get(prod.leader_id);
            auto follower_comp = trossen::hw::ActiveHardwareRegistry::get(prod.follower_id);

            if (!leader_comp || !follower_comp) {
                error = "Failed to get SO101 components from registry";
                return false;
            }

            auto leader_so101 =
                std::dynamic_pointer_cast<trossen::hw::arm::SO101ArmComponent>(leader_comp);
            auto follower_so101 =
                std::dynamic_pointer_cast<trossen::hw::arm::SO101ArmComponent>(follower_comp);

            if (!leader_so101 || !follower_so101) {
                error = "Failed to cast to SO101ArmComponent";
                return false;
            }

            leader_driver = leader_so101->get_driver();
            follower_driver = follower_so101->get_driver();

            if (!leader_driver || !follower_driver) {
                error = "Failed to get SO101 drivers";
                return false;
            }

            // Store drivers to keep them alive
            active_session->arm_drivers.push_back(leader_driver);
            active_session->arm_drivers.push_back(follower_driver);

            // Create and register SO101 teleop producer
            trossen::hw::arm::TeleopSO101ArmProducer::Config teleop_cfg;
            teleop_cfg.stream_id = teleop_stream_id;
            teleop_cfg.use_device_time = use_device_time;

            auto teleop_producer = std::make_shared<
                trossen::hw::arm::TeleopSO101ArmProducer>(
                    leader_driver, follower_driver, teleop_cfg);

            auto teleop_period = std::chrono::milliseconds(33);  // ~30Hz
            active_session->manager->add_producer(teleop_producer, teleop_period);
            arm_count++;
            std::cout << "  [ok] Registered SO101 teleop producer: " << prod.id << std::endl;
        }
    }

    if (arm_count == 0) {
        error = "No SO101 teleop producer found in system";
        return false;
    }

    std::cout << "Producers created: " << arm_count << " arm(s), "
              << camera_count << " camera(s)" << std::endl;

    // Sync follower to leader's current position before starting
    std::cout << "Syncing follower to leader position..." << std::endl;
    auto leader_positions = leader_driver->get_joint_positions(false);
    follower_driver->set_joint_positions(leader_positions, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "  [ok] Follower synced to leader" << std::endl;

    // Start teleoperation thread (runs continuously, mirrors leader to follower)
    active_session->teleop_active = true;
    active_session->teleop_thread = std::thread([leader_driver, follower_driver, active_session]() {
        std::cout << "SO101 teleoperation thread started" << std::endl;

        while (active_session->teleop_active) {
            // Check if we should freeze (waiting for next episode)
            if (active_session->waiting_for_next.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            try {
                auto leader_pos = leader_driver->get_joint_positions(false);
                follower_driver->set_joint_positions(leader_pos, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 100Hz
            } catch (const std::exception& e) {
                std::cerr << "Error in SO101 teleop thread: " << e.what() << std::endl;
                break;
            }
        }

        std::cout << "SO101 teleoperation thread stopped" << std::endl;
    });

    std::cout << "  [ok] SO101 teleoperation ready" << std::endl;
    return true;
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

    std::shared_ptr<trossen_arm::TrossenArmDriver> leader_driver;
    std::shared_ptr<trossen_arm::TrossenArmDriver> follower_driver;
    std::string teleop_stream_id;
    bool use_device_time = true;

    // Loop through ALL producers in the system
    std::cout << "Creating producers from system configuration..." << std::endl;
    int camera_count = 0;
    int arm_count = 0;

    for (const auto& producer_id : system_it->producers) {
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                return p.id == producer_id;
            });

        if (prod_it == configs.producers.end()) continue;

        const auto& prod = *prod_it;

        // Create camera producers
        if (prod.type == "opencv_camera") {
            // Look up camera configuration by ID
            auto cam_it = std::find_if(configs.cameras.begin(), configs.cameras.end(),
                [&prod](const trossen::config::CameraConfig& c) {
                    return c.id == prod.camera_id;
                });

            if (cam_it == configs.cameras.end()) {
                std::cerr << "Warning: Camera not found for producer: "
                          << prod.camera_id << std::endl;
                continue;
            }

            const auto& cam = *cam_it;
            trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
            cam_cfg.device_index = cam.device_index;
            cam_cfg.stream_id = "opencv_camera_" + std::to_string(cam.device_index);
            cam_cfg.encoding = cam.encoding;
            cam_cfg.width = cam.width;
            cam_cfg.height = cam.height;
            cam_cfg.fps = cam.fps;
            cam_cfg.use_device_time = prod.use_device_time;

            // TODO(user): Once OpenCvCameraProducer supports taking
            // OpenCvCameraComponent directly, use the already-connected camera from
            // ActiveHardwareRegistry (like arm producers do) to avoid device-busy
            // errors from attempting to open camera twice.
            auto camera_producer = std::make_shared<
                trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);

            // Replace the above with this once
            // OpenCvCameraProducer(shared_ptr<OpenCvCameraComponent>) is available:
            // auto hw_component = trossen::hw::ActiveHardwareRegistry::get(prod.camera_id);
            // auto camera_component = std::dynamic_pointer_cast<
            //     trossen::hw::camera::OpenCvCameraComponent>(hw_component);
            // if (!camera_component) {
            //     std::cerr << "Failed to get camera component from registry" << std::endl;
            //     continue;
            // }
            // auto camera_producer = std::make_shared<
            //     trossen::hw::camera::OpenCvCameraProducer>(camera_component);

            auto camera_period = std::chrono::milliseconds(
                static_cast<int>(1000.0f / cam.fps));
            active_session->manager->add_producer(
                camera_producer, camera_period);
            camera_count++;
            std::cout << "  [ok] Registered camera producer: "
                      << cam.name << std::endl;
        } else if (prod.type == "teleop_widowx_arm") {
            // Handle WidowX teleop producer
            teleop_stream_id = prod.id;
            use_device_time = prod.use_device_time;

            // Get hardware components from registry using IDs
            auto leader_comp = trossen::hw::ActiveHardwareRegistry::get(prod.leader_id);
            auto follower_comp = trossen::hw::ActiveHardwareRegistry::get(prod.follower_id);

            if (!leader_comp || !follower_comp) {
                error = "WidowX arms not found in hardware registry";
                return false;
            }

            // Cast to TrossenArmComponent and extract drivers
            auto leader_trossen =
                std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(leader_comp);
            auto follower_trossen =
                std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(follower_comp);

            if (!leader_trossen || !follower_trossen) {
                error = "Failed to cast components to TrossenArmComponent";
                return false;
            }

            leader_driver = leader_trossen->get_hardware();
            follower_driver = follower_trossen->get_hardware();

            if (!leader_driver || !follower_driver) {
                error = "Failed to get arm drivers from components";
                return false;
            }

            // Store drivers to keep them alive
            active_session->widowx_drivers.push_back(leader_driver);
            active_session->widowx_drivers.push_back(follower_driver);

            // Set leader to external_effort mode (gravity compensation) for teleoperation
            std::cout << "  Setting leader to external_effort mode..." << std::endl;
            leader_driver->set_all_modes(trossen_arm::Mode::external_effort);
            leader_driver->set_all_external_efforts(
                std::vector<double>(leader_driver->get_num_joints(), 0.0),
                0.0,
                false);

            // Set follower to position mode
            follower_driver->set_all_modes(trossen_arm::Mode::position);

            // Create and register the teleop producer
            trossen::hw::arm::TeleopTrossenArmProducer::Config teleop_cfg;
            teleop_cfg.stream_id = teleop_stream_id;
            teleop_cfg.use_device_time = use_device_time;

            auto teleop_producer = std::make_shared<
                trossen::hw::arm::TeleopTrossenArmProducer>(
                    leader_driver, follower_driver, teleop_cfg);

            auto teleop_period = std::chrono::milliseconds(5);  // 200Hz
            active_session->manager->add_producer(teleop_producer, teleop_period);
            arm_count++;
            std::cout << "  [ok] Registered WidowX teleop producer: " << prod.id << std::endl;
        }
    }

    if (arm_count == 0) {
        error = "No WidowX teleop producer found in system";
        return false;
    }

    std::cout << "Producers created: " << arm_count << " arm(s), "
              << camera_count << " camera(s)" << std::endl;

    const float moving_time_s = 2.0f;
    const std::vector<double> STAGED_POSITIONS = {
        0.0, 1.04719755, 0.523598776, 0.628318531, 0.0, 0.0, 0.0
    };

    // Stage both arms to ready position
    std::cout << "Staging arms to ready position..." << std::endl;
    leader_driver->set_all_modes(trossen_arm::Mode::position);
    follower_driver->set_all_modes(trossen_arm::Mode::position);
    leader_driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    follower_driver->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "  [ok] Arms staged to ready position" << std::endl;

    // Move follower to mirror leader's current position
    std::cout << "Syncing follower to leader position..." << std::endl;
    auto leader_positions = leader_driver->get_all_positions();
    follower_driver->set_all_positions(leader_positions, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    // Set leader back to external_effort mode for teleoperation
    leader_driver->set_all_modes(trossen_arm::Mode::external_effort);
    leader_driver->set_all_external_efforts(
        std::vector<double>(leader_driver->get_num_joints(), 0.0),
        0.0,
        false);
    std::cout << "  [ok] Follower synced to leader" << std::endl;

    // Start teleoperation thread (runs continuously, mirrors leader to follower)
    active_session->teleop_active = true;
    active_session->teleop_thread = std::thread([active_session]() {
        std::cout << "WidowX teleoperation thread started" << std::endl;

        auto leader = active_session->widowx_drivers[0];
        auto follower = active_session->widowx_drivers[1];
        bool was_frozen = false;

        while (active_session->teleop_active) {
            // Check if we should freeze (waiting for next episode)
            if (active_session->waiting_for_next.load()) {
                if (!was_frozen) {
                    // Lock both arms in position mode to prevent movement
                    std::cout << "  Freezing WidowX arms (waiting for next episode)..."
                              << std::endl;
                    leader->set_all_modes(trossen_arm::Mode::position);
                    follower->set_all_modes(trossen_arm::Mode::position);
                    was_frozen = true;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
                continue;
            }

            // Unfreeze - restore external_effort mode for leader
            if (was_frozen) {
                std::cout << "  Resuming WidowX teleoperation..." << std::endl;
                leader->set_all_modes(trossen_arm::Mode::external_effort);
                leader->set_all_external_efforts(
                    std::vector<double>(leader->get_num_joints(), 0.0),
                    0.0,
                    false);
                follower->set_all_modes(trossen_arm::Mode::position);
                was_frozen = false;
            }

            try {
                // Mirror leader arm positions to follower
                auto positions = leader->get_all_positions();
                follower->set_all_positions(positions, 0.0f, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } catch (const std::exception& e) {
                std::cerr << "Error in WidowX teleop thread: " << e.what() << std::endl;
                break;
            }
        }

        std::cout << "WidowX teleoperation thread stopped" << std::endl;

        // Reset arms to rest position if all episodes complete
        if (active_session->all_episodes_complete) {
            std::cout << "Resetting WidowX arms to rest position..." << std::endl;
            const float moving_time_s = 2.0f;
            const std::vector<double> STAGED_POSITIONS = {
                0.0, 1.04719755, 0.523598776, 0.628318531, 0.0, 0.0, 0.0
            };

            try {
                auto leader = active_session->widowx_drivers[0];
                auto follower = active_session->widowx_drivers[1];

                // Set to position mode
                leader->set_all_modes(trossen_arm::Mode::position);
                follower->set_all_modes(trossen_arm::Mode::position);

                // Move to staged positions
                leader->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
                follower->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
                std::this_thread::sleep_for(
                    std::chrono::duration<float>(moving_time_s + 0.1f));

                // Move to sleep position (all zeros)
                std::vector<double> sleep_position(leader->get_num_joints(), 0.0);
                leader->set_all_positions(sleep_position, moving_time_s, false);
                follower->set_all_positions(sleep_position, moving_time_s, false);
                std::this_thread::sleep_for(
                    std::chrono::duration<float>(moving_time_s + 0.1f));

                std::cout << "  [ok] Arms returned to rest position" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error resetting arms: " << e.what() << std::endl;
            }
        }
    });

    std::cout << "  [ok] WidowX teleoperation ready" << std::endl;
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
            // TODO(trossen): Implement just testing camera recording script
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

    std::cout << "  [ok] Registered " << producers_added << " camera producers" << std::endl;
    return true;
}

bool setup_widowx_bimanual_teleop(
    std::shared_ptr<ActiveSession> active_session,
    const std::string& system_id,
    std::string& error)
{
    std::cout << "Setting up WidowX bimanual teleoperation session..." << std::endl;

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

    struct TeleopPairConfig {
        std::string stream_id;
        std::string leader_id;
        std::string follower_id;
        bool use_device_time;
    };
    std::vector<TeleopPairConfig> pairs;

    // Loop through ALL producers in the system
    std::cout << "Creating producers from system configuration..." << std::endl;
    int camera_count = 0;
    int arm_count = 0;

    for (const auto& producer_id : system_it->producers) {
        auto prod_it = std::find_if(configs.producers.begin(), configs.producers.end(),
            [&producer_id](const trossen::config::ProducerConfig& p) {
                return p.id == producer_id;
            });

        if (prod_it == configs.producers.end()) continue;

        const auto& prod = *prod_it;

        // Create camera producers
        if (prod.type == "opencv_camera") {
            // Look up camera configuration by ID
            auto cam_it = std::find_if(configs.cameras.begin(), configs.cameras.end(),
                [&prod](const trossen::config::CameraConfig& c) {
                    return c.id == prod.camera_id;
                });

            if (cam_it == configs.cameras.end()) {
                std::cerr << "Warning: Camera not found for producer: "
                          << prod.camera_id << std::endl;
                continue;
            }

            const auto& cam = *cam_it;
            trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
            cam_cfg.device_index = cam.device_index;
            cam_cfg.stream_id = "opencv_camera_" + std::to_string(cam.device_index);
            cam_cfg.encoding = cam.encoding;
            cam_cfg.width = cam.width;
            cam_cfg.height = cam.height;
            cam_cfg.fps = cam.fps;
            cam_cfg.use_device_time = prod.use_device_time;

            // TODO(user): Once OpenCvCameraProducer supports taking
            // OpenCvCameraComponent directly, use the already-connected camera from
            // ActiveHardwareRegistry (like arm producers do) to avoid device-busy
            // errors from attempting to open camera twice.
            auto camera_producer = std::make_shared<
                trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);

            // Replace the above with this once
            // OpenCvCameraProducer(shared_ptr<OpenCvCameraComponent>) is available:
            // auto hw_component = trossen::hw::ActiveHardwareRegistry::get(prod.camera_id);
            // auto camera_component = std::dynamic_pointer_cast<
            //     trossen::hw::camera::OpenCvCameraComponent>(hw_component);
            // if (!camera_component) {
            //     std::cerr << "Failed to get camera component from registry" << std::endl;
            //     continue;
            // }
            // auto camera_producer = std::make_shared<
            //     trossen::hw::camera::OpenCvCameraProducer>(camera_component);

            auto camera_period = std::chrono::milliseconds(
                static_cast<int>(1000.0f / cam.fps));
            active_session->manager->add_producer(camera_producer, camera_period);
            camera_count++;
            std::cout << "  [ok] Registered camera producer: " << cam.name << std::endl;
        } else if (prod.type == "teleop_widowx_arm") {
            // Collect WidowX teleop producers
            TeleopPairConfig pair;
            pair.stream_id = prod.id;
            pair.leader_id = prod.leader_id;
            pair.follower_id = prod.follower_id;
            pair.use_device_time = prod.use_device_time;
            pairs.push_back(pair);
        }
    }

    if (pairs.size() != 2) {
        error = "Expected exactly 2 WidowX teleop producers, found: " +
                std::to_string(pairs.size());
        return false;
    }

    // Setup both teleop pairs
    for (size_t i = 0; i < pairs.size(); ++i) {
        const auto& pair = pairs[i];

        // Get hardware components from registry using IDs
        auto leader_comp = trossen::hw::ActiveHardwareRegistry::get(pair.leader_id);
        auto follower_comp = trossen::hw::ActiveHardwareRegistry::get(pair.follower_id);

        if (!leader_comp || !follower_comp) {
            error = "WidowX arms not found in hardware registry for pair " +
                    std::to_string(i + 1);
            return false;
        }

        // Cast to TrossenArmComponent and extract drivers
        auto leader_trossen =
            std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(leader_comp);
        auto follower_trossen =
            std::dynamic_pointer_cast<trossen::hw::arm::TrossenArmComponent>(follower_comp);

        if (!leader_trossen || !follower_trossen) {
            error = "Failed to cast components to TrossenArmComponent for pair " +
                    std::to_string(i + 1);
            return false;
        }

        auto leader_driver = leader_trossen->get_hardware();
        auto follower_driver = follower_trossen->get_hardware();

        if (!leader_driver || !follower_driver) {
            error = "Failed to get arm drivers from components for pair " +
                    std::to_string(i + 1);
            return false;
        }

        // Store drivers in active session
        active_session->widowx_drivers.push_back(leader_driver);
        active_session->widowx_drivers.push_back(follower_driver);

        // Set leader to external_effort mode (gravity compensation)
        std::cout << "  Setting leader " << (i + 1) << " to external_effort mode..." << std::endl;
        leader_driver->set_all_modes(trossen_arm::Mode::external_effort);
        leader_driver->set_all_external_efforts(
            std::vector<double>(leader_driver->get_num_joints(), 0.0),
            0.0,
            false);

        // Set follower to position mode
        follower_driver->set_all_modes(trossen_arm::Mode::position);

        // Create and register the teleop producer for this pair
        trossen::hw::arm::TeleopTrossenArmProducer::Config teleop_cfg;
        teleop_cfg.stream_id = pair.stream_id;
        teleop_cfg.use_device_time = pair.use_device_time;

        auto teleop_producer = std::make_shared<
            trossen::hw::arm::TeleopTrossenArmProducer>(
                leader_driver, follower_driver, teleop_cfg);

        auto teleop_period = std::chrono::milliseconds(5);  // 200Hz
        active_session->manager->add_producer(teleop_producer, teleop_period);
        arm_count++;
        std::cout << "  [ok] Registered WidowX teleop producer " << (i + 1)
                  << ": " << pair.stream_id << std::endl;
    }

    std::cout << "Producers created: " << arm_count << " arm pair(s), "
              << camera_count << " camera(s)" << std::endl;

    const float moving_time_s = 2.0f;
    const std::vector<double> STAGED_POSITIONS = {
        0.0, 1.04719755, 0.523598776, 0.628318531, 0.0, 0.0, 0.0
    };

    auto leader1 = active_session->widowx_drivers[0];
    auto follower1 = active_session->widowx_drivers[1];
    auto leader2 = active_session->widowx_drivers[2];
    auto follower2 = active_session->widowx_drivers[3];

    // Stage all arms to ready position
    std::cout << "Staging all arms to ready position..." << std::endl;
    leader1->set_all_modes(trossen_arm::Mode::position);
    follower1->set_all_modes(trossen_arm::Mode::position);
    leader2->set_all_modes(trossen_arm::Mode::position);
    follower2->set_all_modes(trossen_arm::Mode::position);
    leader1->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    follower1->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    leader2->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    follower2->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "  [ok] All arms staged to ready position" << std::endl;

    // Sync both pairs: move followers to mirror leaders' current positions
    std::cout << "Syncing followers to leaders' positions..." << std::endl;

    // Lock leaders temporarily
    leader1->set_all_modes(trossen_arm::Mode::position);
    leader2->set_all_modes(trossen_arm::Mode::position);

    auto leader1_pos = leader1->get_all_positions();
    auto leader2_pos = leader2->get_all_positions();

    follower1->set_all_positions(leader1_pos, moving_time_s, false);
    follower2->set_all_positions(leader2_pos, moving_time_s, false);

    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

    // Unlock leaders back to external_effort mode
    leader1->set_all_modes(trossen_arm::Mode::external_effort);
    leader1->set_all_external_efforts(
        std::vector<double>(leader1->get_num_joints(), 0.0), 0.0, false);

    leader2->set_all_modes(trossen_arm::Mode::external_effort);
    leader2->set_all_external_efforts(
        std::vector<double>(leader2->get_num_joints(), 0.0), 0.0, false);

    std::cout << "  [ok] Both followers synced to leaders" << std::endl;

    // Start bimanual teleoperation thread
    active_session->teleop_active = true;
    active_session->teleop_thread = std::thread([active_session]() {
        std::cout << "WidowX bimanual teleoperation thread started" << std::endl;

        // We have 4 drivers: [leader1, follower1, leader2, follower2]
        auto leader1 = active_session->widowx_drivers[0];
        auto follower1 = active_session->widowx_drivers[1];
        auto leader2 = active_session->widowx_drivers[2];
        auto follower2 = active_session->widowx_drivers[3];
        bool was_frozen = false;

        while (active_session->teleop_active) {
            // Check if we should freeze (waiting for next episode)
            if (active_session->waiting_for_next.load()) {
                if (!was_frozen) {
                    // Lock all arms in position mode to prevent movement
                    std::cout << "  Freezing WidowX bimanual arms "
                              << "(waiting for next episode)..." << std::endl;
                    leader1->set_all_modes(trossen_arm::Mode::position);
                    follower1->set_all_modes(trossen_arm::Mode::position);
                    leader2->set_all_modes(trossen_arm::Mode::position);
                    follower2->set_all_modes(trossen_arm::Mode::position);
                    was_frozen = true;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
                continue;
            }

            // Unfreeze - restore external_effort mode for leaders
            if (was_frozen) {
                std::cout << "  Resuming WidowX bimanual teleoperation..." << std::endl;
                leader1->set_all_modes(trossen_arm::Mode::external_effort);
                leader1->set_all_external_efforts(
                    std::vector<double>(leader1->get_num_joints(), 0.0), 0.0, false);
                leader2->set_all_modes(trossen_arm::Mode::external_effort);
                leader2->set_all_external_efforts(
                    std::vector<double>(leader2->get_num_joints(), 0.0), 0.0, false);
                follower1->set_all_modes(trossen_arm::Mode::position);
                follower2->set_all_modes(trossen_arm::Mode::position);
                was_frozen = false;
            }

            try {
                // Mirror both pairs simultaneously
                auto positions1 = leader1->get_all_positions();
                follower1->set_all_positions(positions1, 0.0f, false);

                auto positions2 = leader2->get_all_positions();
                follower2->set_all_positions(positions2, 0.0f, false);

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } catch (const std::exception& e) {
                std::cerr << "Error in WidowX bimanual teleop thread: "
                          << e.what() << std::endl;
                break;
            }
        }

        std::cout << "WidowX bimanual teleoperation thread stopped" << std::endl;

        // Reset arms to rest position if all episodes complete
        if (active_session->all_episodes_complete) {
            std::cout << "Resetting WidowX bimanual arms to rest position..." << std::endl;
            const float moving_time_s = 2.0f;
            const std::vector<double> STAGED_POSITIONS = {
                0.0, 1.04719755, 0.523598776, 0.628318531, 0.0, 0.0, 0.0
            };

            try {
                auto leader1 = active_session->widowx_drivers[0];
                auto follower1 = active_session->widowx_drivers[1];
                auto leader2 = active_session->widowx_drivers[2];
                auto follower2 = active_session->widowx_drivers[3];

                // Set all to position mode
                leader1->set_all_modes(trossen_arm::Mode::position);
                follower1->set_all_modes(trossen_arm::Mode::position);
                leader2->set_all_modes(trossen_arm::Mode::position);
                follower2->set_all_modes(trossen_arm::Mode::position);

                // Move to staged positions
                leader1->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
                follower1->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
                leader2->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
                follower2->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
                std::this_thread::sleep_for(
                    std::chrono::duration<float>(moving_time_s + 0.1f));

                // Move to sleep position (all zeros)
                std::vector<double> sleep_position(leader1->get_num_joints(), 0.0);
                leader1->set_all_positions(sleep_position, moving_time_s, false);
                follower1->set_all_positions(sleep_position, moving_time_s, false);
                leader2->set_all_positions(sleep_position, moving_time_s, false);
                follower2->set_all_positions(sleep_position, moving_time_s, false);
                std::this_thread::sleep_for(
                    std::chrono::duration<float>(moving_time_s + 0.1f));

                std::cout << "  [ok] All arms returned to rest position" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error resetting bimanual arms: " << e.what() << std::endl;
            }
        }
    });

    std::cout << "  [ok] WidowX bimanual teleoperation ready" << std::endl;
    return true;
}

}  // namespace trossen::backend
