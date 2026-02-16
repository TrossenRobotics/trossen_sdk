#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <map>
#include <thread>
#include <atomic>
#include <fstream>
#include "backend/config_manager.hpp"
#include "backend/hardware_helpers.hpp"
#include "backend/session_actions.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/config_registry.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/so101_teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/so101_arm_component.hpp"
#include "trossen_sdk/hw/arm/so101_arm_driver.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "libtrossen_arm/trossen_arm.hpp"

using json = nlohmann::json;
using httplib::Server;
using httplib::Request;
using httplib::Response;

const char DATA_FILE[] = "data.json";

// Configuration manager instance
trossen::config::ConfigManager config_manager(DATA_FILE);

// Use ActiveSession from session_actions.hpp
using trossen::backend::ActiveSession;

std::map<std::string, std::shared_ptr<ActiveSession>> active_sessions;

int main() {
    Server svr;

    // Enable CORS - set response for ANY request method
    svr.set_logger([](const Request& req, const Response& res) {
        std::cout << req.method << " " << req.path << " - " << res.status << std::endl;
    });

    // Set default CORS headers on all responses
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
        {"Access-Control-Max-Age", "86400"}
    });

    // Handle ALL OPTIONS requests with a catch-all
    svr.set_error_handler([](const Request& req, Response& res) {
        if (req.method == "OPTIONS") {
            res.status = 204;
            res.set_header("Content-Length", "0");
        }
    });

    // Configuration endpoints

    // POST /configure/camera
    svr.Post("/configure/camera", [](const Request& req, Response& res) {
        try {
            json request_data = json::parse(req.body);
            trossen::config::CameraConfig config =
                trossen::config::CameraConfig::from_json(request_data);

            std::string error;
            if (config_manager.add_camera_config(config, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "Camera configured successfully"},
                         {"config", config.to_json()}}.dump(2),
                    "application/json");
                std::cout << "POST /configure/camera - Camera configured: "
                          << config.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /configure/camera - Validation failed: "
                          << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Invalid JSON: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /configure/camera - Error: " << e.what()
                      << std::endl;
        }
    });

    // POST /configure/arm
    svr.Post("/configure/arm", [](const Request& req, Response& res) {
        try {
            json request_data = json::parse(req.body);
            trossen::config::ArmConfig config =
                trossen::config::ArmConfig::from_json(request_data);

            std::string error;
            if (config_manager.add_arm_config(config, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "Arm configured successfully"},
                         {"config", config.to_json()}}.dump(2),
                    "application/json");
                std::cout << "POST /configure/arm - Arm configured: "
                          << config.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /configure/arm - Validation failed: "
                          << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Invalid JSON: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /configure/arm - Error: " << e.what()
                      << std::endl;
        }
    });

    // PUT /configure/camera/:index
    svr.Put(R"(/configure/camera/(\d+))", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            json request_data = json::parse(req.body);
            trossen::config::CameraConfig config =
                trossen::config::CameraConfig::from_json(request_data);

            std::string error;
            if (config_manager.update_camera_config(index, config, error)) {
                res.set_content(
                    json{{
"success", true},
                         {"message", "Camera updated successfully"},
                         {"config", config.to_json()}}.dump(2),
                    "application/json");
                std::cout << "PUT /configure/camera/" << index
                          << " - Camera updated: " << config.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "PUT /configure/camera/" << index
                          << " - Update failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "PUT /configure/camera - Error: " << e.what() << std::endl;
        }
    });

    // DELETE /configure/camera/:index
    svr.Delete(R"(/configure/camera/(\d+))", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            std::string error;
            if (config_manager.delete_camera_config(index, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "Camera deleted successfully"}}.dump(2),
                    "application/json");
                std::cout << "DELETE /configure/camera/" << index
                          << " - Camera deleted" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "DELETE /configure/camera/" << index
                          << " - Delete failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "DELETE /configure/camera - Error: " << e.what()
                      << std::endl;
        }
    });

    // PUT /configure/arm/:index
    svr.Put(R"(/configure/arm/(\d+))", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            json request_data = json::parse(req.body);
            trossen::config::ArmConfig config =
                trossen::config::ArmConfig::from_json(request_data);

            std::string error;
            if (config_manager.update_arm_config(index, config, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "Arm updated successfully"},
                         {"config", config.to_json()}}.dump(2),
                    "application/json");
                std::cout << "PUT /configure/arm/" << index
                          << " - Arm updated: " << config.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "PUT /configure/arm/" << index
                          << " - Update failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "PUT /configure/arm - Error: " << e.what()
                      << std::endl;
        }
    });

    // DELETE /configure/arm/:index
    svr.Delete(R"(/configure/arm/(\d+))", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            std::string error;
            if (config_manager.delete_arm_config(index, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "Arm deleted successfully"}}.dump(2),
                    "application/json");
                std::cout << "DELETE /configure/arm/" << index
                          << " - Arm deleted" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "DELETE /configure/arm/" << index
                          << " - Delete failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "DELETE /configure/arm - Error: " << e.what()
                      << std::endl;
        }
    });

    // POST /configure/producer - Create new producer
    svr.Post("/configure/producer", [](const Request& req, Response& res) {
        try {
            json request_data = json::parse(req.body);

            // Read current data
            auto configs = config_manager.get_configurations();
            json data = configs.to_json();

            // Ensure producers array exists
            if (!data.contains("producers")) {
                data["producers"] = json::array();
            }

            // Validate id exists
            if (!request_data.contains("id")) {
                res.status = 400;
                res.set_content(
                    json{{"success", false},
                         {"error", "Producer id is required"}}.dump(2),
                    "application/json");
                return;
            }

            std::string producer_id = request_data["id"];

            // Check for duplicate id
            for (const auto& producer : data["producers"]) {
                if (producer.contains("id") && producer["id"] == producer_id) {
                    res.status = 400;
                    res.set_content(
                        json{{"success", false},
                             {"error", "Producer with this ID already exists"}}.dump(2),
                        "application/json");
                    return;
                }
            }

            // Flatten structure: if config exists, merge it to top level
            json flattened = request_data;
            if (request_data.contains("config")) {
                for (auto& [key, value] : request_data["config"].items()) {
                    flattened[key] = value;
                }
                // Remove the nested config object
                flattened.erase("config");
            }

            // Ensure stream_id exists at top level (fallback to id if not provided)
            if (!flattened.contains("stream_id")) {
                flattened["stream_id"] = producer_id;
            }

            // Add flattened producer
            data["producers"].push_back(flattened);

            // Save back to file
            config_manager.save_raw_json(data);

            res.set_content(
                json{{"success", true},
                     {"message", "Producer created successfully"},
                     {"producer", flattened}}.dump(2),
                "application/json");
            std::cout << "POST /configure/producer - Producer created: "
                      << flattened.value("name", "unnamed") << " (id: "
                      << producer_id << ")" << std::endl;
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /configure/producer - Error: " << e.what()
                      << std::endl;
        }
    });

    // PUT /configure/producer/:id - Update producer by ID
    svr.Put(R"(/configure/producer/([^/]+))", [](const Request& req, Response& res) {
        try {
            std::string producer_id = req.matches[1];
            json request_data = json::parse(req.body);

            auto configs = config_manager.get_configurations();
            json data = configs.to_json();

            if (!data.contains("producers")) {
                res.status = 404;
                res.set_content(
                    json{{"success", false}, {"error", "No producers found"}}.dump(2),
                    "application/json");
                return;
            }

            // Flatten structure: if config exists, merge it to top level
            json flattened = request_data;
            if (request_data.contains("config")) {
                for (auto& [key, value] : request_data["config"].items()) {
                    flattened[key] = value;
                }
                flattened.erase("config");
            }

            // Ensure stream_id exists
            if (!flattened.contains("stream_id") && flattened.contains("id")) {
                flattened["stream_id"] = flattened["id"];
            }

            // Find and update producer by id
            bool found = false;
            for (size_t i = 0; i < data["producers"].size(); ++i) {
                if (data["producers"][i].contains("id") &&
                    data["producers"][i]["id"] == producer_id) {
                    data["producers"][i] = flattened;
                    found = true;
                    break;
                }
            }

            if (!found) {
                res.status = 404;
                res.set_content(
                    json{{"success", false}, {"error", "Producer not found"}}.dump(2),
                    "application/json");
                return;
            }

            config_manager.save_raw_json(data);

            res.set_content(
                json{{"success", true},
                     {"message", "Producer updated successfully"},
                     {"producer", flattened}}.dump(2),
                "application/json");
            std::cout << "PUT /configure/producer/" << producer_id
                      << " - Producer updated" << std::endl;
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "PUT /configure/producer - Error: " << e.what()
                      << std::endl;
        }
    });

    // DELETE /configure/producer/:id - Delete producer by ID
    svr.Delete(R"(/configure/producer/([^/]+))", [](const Request& req, Response& res) {
        try {
            std::string producer_id = req.matches[1];

            auto configs = config_manager.get_configurations();
            json data = configs.to_json();

            if (!data.contains("producers")) {
                res.status = 404;
                res.set_content(
                    json{{"success", false}, {"error", "No producers found"}}.dump(2),
                    "application/json");
                return;
            }

            // Find and delete producer by id
            bool found = false;
            for (size_t i = 0; i < data["producers"].size(); ++i) {
                if (data["producers"][i].contains("id") &&
                    data["producers"][i]["id"] == producer_id) {
                    data["producers"].erase(data["producers"].begin() + i);
                    found = true;
                    break;
                }
            }

            if (!found) {
                res.status = 404;
                res.set_content(
                    json{{"success", false}, {"error", "Producer not found"}}.dump(2),
                    "application/json");
                return;
            }

            config_manager.save_raw_json(data);

            res.set_content(
                json{{"success", true},
                     {"message", "Producer deleted successfully"}}.dump(2),
                "application/json");
            std::cout << "DELETE /configure/producer/" << producer_id
                      << " - Producer deleted" << std::endl;
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "DELETE /configure/producer - Error: " << e.what()
                      << std::endl;
        }
    });

    // POST /configure/system - Create new hardware system
    svr.Post("/configure/system", [](const Request& req, Response& res) {
        try {
            json request_data = json::parse(req.body);
            trossen::config::HardwareSystem system =
                trossen::config::HardwareSystem::from_json(request_data);

            std::string error;
            if (config_manager.add_system(system, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "System created successfully"},
                         {"system", system.to_json()}}.dump(2),
                    "application/json");
                std::cout << "POST /configure/system - System created: "
                          << system.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /configure/system - Creation failed: "
                          << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /configure/system - Error: " << e.what()
                      << std::endl;
        }
    });

    // PUT /configure/system/:id - Update hardware system
    svr.Put(R"(/configure/system/([^/]+))", [](const Request& req, Response& res) {
        try {
            std::string id = req.matches[1];
            json request_data = json::parse(req.body);
            trossen::config::HardwareSystem system =
                trossen::config::HardwareSystem::from_json(request_data);

            std::string error;
            if (config_manager.update_system(id, system, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "System updated successfully"},
                         {"system", system.to_json()}}.dump(2),
                    "application/json");
                std::cout << "PUT /configure/system/" << id
                          << " - System updated: " << system.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "PUT /configure/system/" << id
                          << " - Update failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "PUT /configure/system - Error: " << e.what()
                      << std::endl;
        }
    });

    // DELETE /configure/system/:id - Delete hardware system
    svr.Delete(R"(/configure/system/([^/]+))", [](const Request& req, Response& res) {
        try {
            std::string id = req.matches[1];
            std::string error;
            if (config_manager.delete_system(id, error)) {
                res.set_content(
                    json{{"success", true},
                         {"message", "System deleted successfully"}}.dump(2),
                    "application/json");
                std::cout << "DELETE /configure/system/" << id
                          << " - System deleted" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "DELETE /configure/system/" << id
                          << " - Delete failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "DELETE /configure/system - Error: " << e.what()
                      << std::endl;
        }
    });

    // POST /configure/session - Create recording session
    svr.Post("/configure/session", [](const Request& req, Response& res) {
        try {
            json request_data = json::parse(req.body);
            trossen::config::RecordingSession session =
              trossen::config::RecordingSession::from_json(request_data);

            std::string error;
            if (config_manager.add_session(session, error)) {
                // Log activity
                config_manager.log_activity(
                    session.id, session.name, "created",
                    "Session created with " + std::to_string(session.num_episodes) +
                    " episodes @ " +
                    std::to_string(static_cast<int>(session.episode_duration)) +
                    "s each");

                res.set_content(
                  json{{"success", true},
                       {"message", "Session created successfully"},
                       {"session", session.to_json()}}.dump(2),
                  "application/json");
                std::cout << "POST /configure/session - Session created: "
                          << session.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(json{{"success", false}, {"error", error}}.dump(2),
                               "application/json");
                std::cerr << "POST /configure/session - Validation failed: "
                          << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
              json{{"success", false},
                   {"error", std::string("Invalid JSON: ") + e.what()}}.dump(2),
              "application/json");
            std::cerr << "POST /configure/session - Error: " << e.what()
                      << std::endl;
        }
    });

    // PUT /configure/session/:id - Update recording session
    svr.Put(R"(/configure/session/([^/]+))", [](const Request& req, Response& res) {
        try {
            std::string id = req.matches[1];
            json request_data = json::parse(req.body);
            trossen::config::RecordingSession session =
              trossen::config::RecordingSession::from_json(request_data);

            std::string error;
            if (config_manager.update_session(id, session, error)) {
                res.set_content(
                  json{{"success", true},
                       {"message", "Session updated successfully"},
                       {"session", session.to_json()}}.dump(2),
                  "application/json");
                std::cout << "PUT /configure/session/" << id
                          << " - Session updated: " << session.name << std::endl;
            } else {
                res.status = 400;
                res.set_content(json{{"success", false}, {"error", error}}.dump(2),
                               "application/json");
                std::cerr << "PUT /configure/session/" << id
                          << " - Update failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
              json{{"success", false},
                   {"error", std::string("Error: ") + e.what()}}.dump(2),
              "application/json");
            std::cerr << "PUT /configure/session - Error: " << e.what()
                      << std::endl;
        }
    });

    // DELETE /configure/session/:id - Delete recording session
    svr.Delete(R"(/configure/session/([^/]+))", [](const Request& req, Response& res) {
        try {
            std::string id = req.matches[1];
            std::string error;
            if (config_manager.delete_session(id, error)) {
                res.set_content(
                  json{{"success", true},
                       {"message", "Session deleted successfully"}}.dump(2),
                  "application/json");
                std::cout << "DELETE /configure/session/" << id
                          << " - Session deleted" << std::endl;
            } else {
                res.status = 400;
                res.set_content(json{{"success", false}, {"error", error}}.dump(2),
                               "application/json");
                std::cerr << "DELETE /configure/session/" << id
                          << " - Delete failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
              json{{"success", false},
                   {"error", std::string("Error: ") + e.what()}}.dump(2),
              "application/json");
            std::cerr << "DELETE /configure/session - Error: " << e.what()
                      << std::endl;
        }
    });

    // POST /session/:id/start - Start teleoperation recording session
    svr.Post(R"(/session/([^/]+)/start)", [](const Request& req, Response& res) {
        try {
            std::string session_id = req.matches[1];

            // Check if session already active
            if (active_sessions.find(session_id) != active_sessions.end()) {
                res.status = 400;
                res.set_content(
                    json{{"success", false},
                         {"error", "Session already active"}}.dump(2),
                    "application/json");
                return;
            }

            // Get session configuration
            auto configs = config_manager.get_configurations();
            auto session_it = std::find_if(
                configs.sessions.begin(), configs.sessions.end(),
                [&session_id](const trossen::config::RecordingSession& s) {
                    return s.id == session_id;
                });

            if (session_it == configs.sessions.end()) {
                res.status = 404;
                res.set_content(
                    json{{"success", false},
                         {"error", "Session not found"}}.dump(2),
                    "application/json");
                return;
            }

            auto& session = *session_it;

            // Check if hardware is already in use by another active session
            for (const auto& [active_id, active_sess] : active_sessions) {
                // Get the active session's configuration
                auto active_session_it = std::find_if(
                    configs.sessions.begin(), configs.sessions.end(),
                    [&active_id](const trossen::config::RecordingSession& s) {
                        return s.id == active_id;
                    });

                if (active_session_it != configs.sessions.end()) {
                    // Check if system_id matches (hardware conflict)
                    if (!session.system_id.empty() &&
                        session.system_id == active_session_it->system_id) {
                        res.status = 409;  // Conflict
                        json error_json = {
                            {"success", false},
                            {"error",
                             "Hardware system is already in use by another session: " +
                             active_session_it->name}};
                        res.set_content(error_json.dump(2), "application/json");
                        return;
                    }

                    // Check for camera conflicts
                    for (const auto& camera : session.cameras) {
                        if (std::find(active_session_it->cameras.begin(),
                                     active_session_it->cameras.end(),
                                     camera) != active_session_it->cameras.end()) {
                            res.status = 409;  // Conflict
                            json error_json = {
                                {"success", false},
                                {"error",
                                 "Camera '" + camera +
                                 "' is already in use by session: " +
                                 active_session_it->name}};
                            res.set_content(
                                error_json.dump(2), "application/json");
                            return;
                        }
                    }

                    // Check for robot/arm conflicts
                    for (const auto& robot : session.robots) {
                        if (std::find(active_session_it->robots.begin(),
                                     active_session_it->robots.end(),
                                     robot) != active_session_it->robots.end()) {
                            res.status = 409;  // Conflict
                            json error_json = {
                                {"success", false},
                                {"error",
                                 "Robot/arm '" + robot +
                                 "' is already in use by session: " +
                                 active_session_it->name}};
                            res.set_content(
                                error_json.dump(2), "application/json");
                            return;
                        }
                    }
                }
            }

            // Validate hardware for the selected action
            std::string validation_error;
            try {
                auto action = trossen::backend::string_to_action(
                    session.action);
                if (!trossen::backend::validate_hardware_for_action(
                        action, session.system_id, validation_error)) {
                    res.status = 400;
                    res.set_content(
                        json{{"success", false},
                             {"error", validation_error}}.dump(2),
                        "application/json");
                    return;
                }
            } catch (const std::exception& e) {
                res.status = 400;
                json error_json = {
                    {"success", false},
                    {"error", std::string("Invalid action: ") + e.what()}};
                res.set_content(error_json.dump(2), "application/json");
                return;
            }

            // Setup GlobalConfig for SessionManager
            json global_config = {
                {"session_manager", {
                    {"type", "session_manager"},
                    {"max_duration", session.episode_duration},
                    {"max_episodes", session.num_episodes},
                    {"backend_type", session.backend_type}
                }}
            };

            // Add backend configuration based on type
            if (session.backend_type == "lerobot") {
                global_config["lerobot_backend"] = {
                    {"type", "lerobot_backend"},
                    {"encoder_threads", 2},
                    {"max_image_queue", 10},
                    {"png_compression_level", 5},
                    {"override_existing", true},
                    {"encode_videos", true},
                    {"task_name", session.name},
                    {"repository_id", "TrossenRoboticsCommunity"},
                    {"dataset_id", session_id},
                    {"episode_index", 0},
                    {"robot_name", "robot"},
                    {"fps", 30.0}
                };
            } else {  // mcap
                global_config["mcap_backend"] = {
                    {"type", "mcap_backend"},
                    {"robot_name", "/robot/joint_states"},
                    {"chunk_size_bytes", 4194304},
                    {"compression", "zstd"},
                    {"dataset_id", session_id},
                    {"episode_index", 0}
                };
            }

            // Load global config
            trossen::configuration::GlobalConfig::instance().load_from_json(global_config);

            // Create ActiveSession structure
            auto active_session = std::make_shared<ActiveSession>();
            active_session->session_id = session_id;
            active_session->session_name = session.name;
            active_session->max_episodes = session.num_episodes;
            active_session->session_start_time = std::chrono::steady_clock::now();
            active_session->manager = std::make_shared<trossen::runtime::SessionManager>();

            // Setup session based on action type
            std::string setup_error;
            bool setup_success = false;

            try {
                auto action = trossen::backend::string_to_action(session.action);

                switch (action) {
                    case trossen::backend::SessionAction::TELEOP_SO101:
                        setup_success = trossen::backend::setup_so101_teleop(
                            active_session, session.system_id, setup_error);
                        break;

                    case trossen::backend::SessionAction::TELEOP_WIDOWX:
                        setup_success = trossen::backend::setup_widowx_teleop(
                            active_session, session.system_id, setup_error);
                        break;

                    case trossen::backend::SessionAction::RECORD_CAMERAS_ONLY:
                        setup_success = trossen::backend::setup_camera_recording(
                            active_session, session.system_id, setup_error);
                        break;

                    default:
                        setup_error = "Unknown session action";
                        break;
                }
            } catch (const std::exception& e) {
                setup_error = std::string("Setup failed: ") + e.what();
            }

            if (!setup_success) {
                res.status = 500;
                res.set_content(
                    json{{"success", false},
                         {"error", setup_error}}.dump(2),
                    "application/json");
                return;
            }

            // Start the first episode
            if (!active_session->manager->start_episode()) {
                if (active_session->teleop_active) {
                    active_session->teleop_active = false;
                    if (active_session->teleop_thread.joinable()) {
                        active_session->teleop_thread.join();
                    }
                }

                res.status = 500;
                res.set_content(
                    json{{"success", false},
                         {"error", "Failed to start episode"}}.dump(2),
                    "application/json");
                return;
            }

            // Start episode manager thread to handle multiple episodes
            // with manual progression
            active_session->episode_manager_active = true;
            active_session->waiting_for_next = false;  // Start first episode immediately
            active_session->episode_manager_thread = std::thread([active_session]() {
                std::cout << "  ✓ Episode manager thread started (manual progression mode)"
                          << std::endl;

                while (active_session->episode_manager_active) {
                    // Check if current episode is complete
                    if (!active_session->manager->is_episode_active()) {
                        auto stats = active_session->manager->stats();

                        if (stats.total_episodes_completed < active_session->max_episodes) {
                            // Episode finished - wait for user to click "Next" button
                            std::cout << "  → Episode " << stats.total_episodes_completed
                                      << " complete. Waiting for user to start episode "
                                      << (stats.total_episodes_completed + 1) << " of "
                                      << active_session->max_episodes << std::endl;

                            active_session->waiting_for_next = true;

                            // Wait for user to trigger next episode via /session/:id/next endpoint
                            while (active_session->waiting_for_next &&
                                   active_session->episode_manager_active) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }

                            // User clicked "Next" - start next episode
                            if (active_session->episode_manager_active) {
                                if (!active_session->manager->start_episode()) {
                                    std::cerr << "  ✗ Failed to start episode "
                                              << (stats.total_episodes_completed + 1)
                                              << std::endl;
                                    active_session->episode_manager_active = false;
                                    break;
                                }
                                std::cout << "  ✓ Starting episode "
                                          << (stats.total_episodes_completed + 1)
                                          << std::endl;
                            }
                        } else {
                            // All episodes complete
                            std::cout << "  ✓ All " << active_session->max_episodes
                                      << " episodes complete!" << std::endl;

                            config_manager.log_activity(
                                active_session->session_id,
                                active_session->session_name,
                                "completed",
                                "All " + std::to_string(active_session->max_episodes) +
                                " episodes completed");

                            if (active_session->teleop_active) {
                                active_session->teleop_active = false;
                                std::cout << "  → Stopping teleoperation thread..." << std::endl;
                            }

                            active_session->all_episodes_complete = true;
                            break;
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                std::cout << "  ✓ Episode manager thread stopped" << std::endl;
            });

            // Store active session
            active_sessions[session_id] = active_session;

            // Log activity
            config_manager.log_activity(
                session.id, session.name, "started",
                "Recording started - " + session.action);

            json response_json = {
                {"success", true},
                {"message", "Session started successfully"},
                {"episode",
                 active_session->manager->stats().current_episode_index}};
            res.set_content(response_json.dump(2), "application/json");
            std::cout << "POST /session/" << session_id
                      << "/start - Session started" << std::endl;
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /session/start - Error: " << e.what() << std::endl;
        }
    });

    // POST /session/:id/stop - Stop recording session
    svr.Post(R"(/session/([^/]+)/stop)", [](const Request& req, Response& res) {
        try {
            std::string session_id = req.matches[1];

            auto it = active_sessions.find(session_id);
            if (it == active_sessions.end()) {
                res.status = 404;
                res.set_content(
                    json{{"success", false},
                         {"error", "Session not active"}}.dump(2),
                    "application/json");
                return;
            }

            auto& active_session = it->second;

            // Stop episode manager thread
            active_session->episode_manager_active = false;
            if (active_session->episode_manager_thread.joinable()) {
                active_session->episode_manager_thread.join();
            }

            // Stop teleoperation thread
            if (active_session->teleop_thread.joinable()) {
                active_session->teleop_active = false;
                active_session->teleop_thread.join();
            }

            // Stop current episode if active
            if (active_session->manager->is_episode_active()) {
                active_session->manager->stop_episode();
            }

            // Log processing activity
            config_manager.log_activity(
                session_id,
                active_session->session_name,
                "processed",
                "Session data processed and finalized");

            // Remove from active sessions
            active_sessions.erase(it);

            res.set_content(
                json{{"success", true},
                     {"message", "Session stopped successfully"}}.dump(2),
                "application/json");
            std::cout << "POST /session/" << session_id
                      << "/stop - Session stopped" << std::endl;
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /session/stop - Error: " << e.what() << std::endl;
        }
    });

    // GET /session/:id/stats - Get session statistics
    svr.Get(R"(/session/([^/]+)/stats)", [](const Request& req, Response& res) {
        try {
            std::string session_id = req.matches[1];

            auto it = active_sessions.find(session_id);
            if (it == active_sessions.end()) {
                res.status = 404;
                res.set_content(
                    json{{"success", false},
                         {"error", "Session not active"}}.dump(2),
                    "application/json");
                return;
            }

            auto& active_session = it->second;

            // Get stats from SessionManager
            auto stats = active_session->manager->stats();

            // Convert microseconds to milliseconds
            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    stats.elapsed).count();

            // Calculate total elapsed time since session started
            auto now = std::chrono::steady_clock::now();
            auto total_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - active_session->session_start_time).count();

            json response = {
                {"success", true},
                {"stats", {
                    {"current_episode_index", stats.current_episode_index},
                    {"episode_active", stats.episode_active},
                    {"elapsed", elapsed_ms},
                    {"total_elapsed", total_elapsed},
                    {"records_written_current", stats.records_written_current},
                    {"total_episodes_completed", stats.total_episodes_completed},
                    {"all_episodes_complete", active_session->all_episodes_complete.load()},
                    {"waiting_for_next", active_session->waiting_for_next.load()}
                }}
            };

            // Add remaining time if available (convert to milliseconds)
            if (stats.remaining.has_value()) {
                auto remaining_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        *stats.remaining).count();
                response["stats"]["remaining"] = remaining_ms;
            } else {
                response["stats"]["remaining"] = nullptr;
            }

            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "GET /session/stats - Error: " << e.what() << std::endl;
        }
    });

    // POST /session/:id/next - Start next episode (manual progression)
    svr.Post(R"(/session/([^/]+)/next)", [](const Request& req, Response& res) {
        try {
            std::string session_id = req.matches[1];

            auto it = active_sessions.find(session_id);
            if (it == active_sessions.end()) {
                res.status = 404;
                res.set_content(
                    json{{"success", false},
                         {"error", "Session not active"}}.dump(2),
                    "application/json");
                return;
            }

            auto& active_session = it->second;

            // Check if we're waiting for next episode
            if (!active_session->waiting_for_next) {
                res.status = 400;
                res.set_content(
                    json{{"success", false},
                         {"error", "Session is not waiting for next episode"}}.dump(2),
                    "application/json");
                return;
            }

            // Check if all episodes are complete
            if (active_session->all_episodes_complete) {
                res.status = 400;
                res.set_content(
                    json{{"success", false},
                         {"error", "All episodes are complete"}}.dump(2),
                    "application/json");
                return;
            }

            // Signal episode manager to start next episode
            active_session->waiting_for_next = false;

            res.set_content(
                json{{"success", true},
                     {"message", "Starting next episode"}}.dump(2),
                "application/json");
            std::cout << "POST /session/" << session_id
                      << "/next - Starting next episode" << std::endl;
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"success", false},
                     {"error", std::string("Error: ") + e.what()}}.dump(2),
                "application/json");
            std::cerr << "POST /session/next - Error: " << e.what() << std::endl;
        }
    });

    // POST /hardware/camera/:index/connect
    svr.Post(R"(/hardware/camera/(\d+)/connect)", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            json request_data = json::parse(req.body);
            std::string camera_id = request_data["name"];

            std::string error;
            if (trossen::backend::connect_camera(
                    camera_id, request_data, error)) {
                json response_json = {
                    {"success", true},
                    {"message", "Camera connected successfully"}};
                res.set_content(response_json.dump(2), "application/json");
                std::cout << "POST /hardware/camera/" << index
                          << "/connect - Success" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /hardware/camera/" << index
                          << "/connect - Failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 500;
            json error_json = {
                {"success", false},
                {"error", std::string("Error: ") + e.what()}};
            res.set_content(error_json.dump(2), "application/json");
            std::cerr << "POST /hardware/camera/connect - Error: "
                      << e.what() << std::endl;
        }
    });

    // POST /hardware/camera/:index/disconnect
    svr.Post(R"(/hardware/camera/(\d+)/disconnect)", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            json request_data = json::parse(req.body);
            std::string camera_id = request_data["name"];

            std::string error;
            if (trossen::backend::disconnect_camera(camera_id, error)) {
                json response_json = {
                    {"success", true},
                    {"message", "Camera disconnected successfully"}};
                res.set_content(response_json.dump(2), "application/json");
                std::cout << "POST /hardware/camera/" << index
                          << "/disconnect - Success" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /hardware/camera/" << index
                          << "/disconnect - Failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 500;
            json error_json = {
                {"success", false},
                {"error", std::string("Error: ") + e.what()}};
            res.set_content(error_json.dump(2), "application/json");
            std::cerr << "POST /hardware/camera/disconnect - Error: "
                      << e.what() << std::endl;
        }
    });

    // POST /hardware/arm/:index/connect
    svr.Post(R"(/hardware/arm/(\d+)/connect)", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            json request_data = json::parse(req.body);
            std::string arm_id = request_data["name"];

            std::string error;
            if (trossen::backend::connect_arm(
                    arm_id, request_data, error)) {
                json response_json = {
                    {"success", true},
                    {"message", "Arm connected successfully"}};
                res.set_content(response_json.dump(2), "application/json");
                std::cout << "POST /hardware/arm/" << index
                          << "/connect - Success" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /hardware/arm/" << index
                          << "/connect - Failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 500;
            json error_json = {
                {"success", false},
                {"error", std::string("Error: ") + e.what()}};
            res.set_content(error_json.dump(2), "application/json");
            std::cerr << "POST /hardware/arm/connect - Error: "
                      << e.what() << std::endl;
        }
    });

    // POST /hardware/arm/:index/disconnect
    svr.Post(R"(/hardware/arm/(\d+)/disconnect)", [](const Request& req, Response& res) {
        try {
            int index = std::stoi(req.matches[1]);
            json request_data = json::parse(req.body);
            std::string arm_id = request_data["name"];

            std::string error;
            if (trossen::backend::disconnect_arm(arm_id, error)) {
                json response_json = {
                    {"success", true},
                    {"message", "Arm disconnected successfully"}};
                res.set_content(response_json.dump(2), "application/json");
                std::cout << "POST /hardware/arm/" << index
                          << "/disconnect - Success" << std::endl;
            } else {
                res.status = 400;
                res.set_content(
                    json{{"success", false}, {"error", error}}.dump(2),
                    "application/json");
                std::cerr << "POST /hardware/arm/" << index
                          << "/disconnect - Failed: " << error << std::endl;
            }
        } catch (const std::exception& e) {
            res.status = 500;
            json error_json = {
                {"success", false},
                {"error", std::string("Error: ") + e.what()}};
            res.set_content(error_json.dump(2), "application/json");
            std::cerr << "POST /hardware/arm/disconnect - Error: "
                      << e.what() << std::endl;
        }
    });

    // GET /hardware/status
    svr.Get("/hardware/status", [](const Request&, Response& res) {
        try {
            json all_statuses = trossen::backend::get_all_hardware_status();
            res.set_content(all_statuses.dump(2), "application/json");
            std::cout << "GET /hardware/status - Retrieved status for "
                      << all_statuses.size()
                      << " hardware components" << std::endl;
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"error", e.what()}}.dump(), "application/json");
            std::cerr << "GET /hardware/status - Error: " << e.what()
                      << std::endl;
        }
    });

    // GET /configurations
    svr.Get("/configurations", [](const Request&, Response& res) {
        try {
            // Read raw JSON to include producers
            std::ifstream file("data.json");
            if (file.is_open()) {
                json data;
                file >> data;
                file.close();

                // Ensure producers array exists
                if (!data.contains("producers")) {
                    data["producers"] = json::array();
                }

                res.set_content(data.dump(2), "application/json");
                std::cout
                    << "GET /configurations - Retrieved configurations including producers"
                    << std::endl;
            } else {
                // Fallback to structured configs if file can't be read
                trossen::config::Configurations configs =
                    config_manager.get_configurations();
                json data = configs.to_json();
                data["producers"] = json::array();
                res.set_content(data.dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"error", e.what()}}.dump(), "application/json");
            std::cerr << "GET /configurations - Error: " << e.what()
                      << std::endl;
        }
    });

    // GET /activities - Get recent session activities
    svr.Get("/activities", [](const Request& req, Response& res) {
        try {
            // Get limit parameter (default 50)
            int limit = 50;
            if (req.has_param("limit")) {
                limit = std::stoi(req.get_param_value("limit"));
            }

            auto activities = config_manager.get_recent_activities(limit);

            json activities_json = json::array();
            for (const auto& activity : activities) {
                activities_json.push_back(activity.to_json());
            }

            res.set_content(
                json{{"success", true},
                     {"activities", activities_json}}.dump(2),
                "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"success", false},
                     {"error", std::string(e.what())}}.dump(2),
                "application/json");
        }
    });

    // DELETE /activities - Clear all activities
    svr.Delete("/activities", [](const Request& req, Response& res) {
        try {
            config_manager.clear_activities();
            res.set_content(
                json{{"success", true},
                     {"message", "Activities cleared"}}.dump(2),
                "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"success", false},
                     {"error", std::string(e.what())}}.dump(2),
                "application/json");
        }
    });

    // Root endpoint
    svr.Get("/", [](const Request&, Response& res) {
        json info = {
            {"name", "Trossen SDK Backend"},
            {"version", "1.0.0"},
            {"endpoints", {
                // Configuration endpoints
                {{"method", "POST"},
                 {"path", "/configure/camera"},
                 {"description", "Add a new camera configuration"}},
                {{"method", "PUT"},
                 {"path", "/configure/camera/:index"},
                 {"description", "Update camera configuration by index"}},
                {{"method", "DELETE"},
                 {"path", "/configure/camera/:index"},
                 {"description", "Delete camera configuration by index"}},
                {{"method", "POST"},
                 {"path", "/configure/arm"},
                 {"description", "Add a new arm configuration"}},
                {{"method", "PUT"},
                 {"path", "/configure/arm/:index"},
                 {"description", "Update arm configuration by index"}},
                {{"method", "DELETE"},
                 {"path", "/configure/arm/:index"},
                 {"description", "Delete arm configuration by index"}},
                {{"method", "POST"},
                 {"path", "/configure/producer"},
                 {"description", "Create a new producer configuration"}},
                {{"method", "PUT"},
                 {"path", "/configure/producer/:id"},
                 {"description", "Update producer configuration by ID"}},
                {{"method", "DELETE"},
                 {"path", "/configure/producer/:id"},
                 {"description", "Delete producer configuration by ID"}},
                {{"method", "POST"},
                 {"path", "/configure/system"},
                 {"description", "Create a new hardware system"}},
                {{"method", "PUT"},
                 {"path", "/configure/system/:id"},
                 {"description", "Update hardware system by ID"}},
                {{"method", "DELETE"},
                 {"path", "/configure/system/:id"},
                 {"description", "Delete hardware system by ID"}},
                {{"method", "POST"},
                 {"path", "/configure/session"},
                 {"description", "Create a new recording session"}},
                {{"method", "PUT"},
                 {"path", "/configure/session/:id"},
                 {"description", "Update recording session by ID"}},
                {{"method", "DELETE"},
                 {"path", "/configure/session/:id"},
                 {"description", "Delete recording session by ID"}},

                // Session control endpoints
                {{"method", "POST"},
                 {"path", "/session/:id/start"},
                 {"description",
                  "Start recording session with configured action (teleop/recording)"}},
                {{"method", "POST"},
                 {"path", "/session/:id/stop"},
                 {"description", "Stop recording session"}},
                {{"method", "GET"},
                 {"path", "/session/:id/stats"},
                 {"description",
                  "Get real-time session statistics (episodes, elapsed time, records written)"}},

                // Hardware control endpoints
                {{"method", "POST"},
                 {"path", "/hardware/camera/:index/connect"},
                 {"description", "Connect camera hardware to the system"}},
                {{"method", "POST"},
                 {"path", "/hardware/camera/:index/disconnect"},
                 {"description", "Disconnect camera hardware from the system"}},
                {{"method", "POST"},
                 {"path", "/hardware/arm/:index/connect"},
                 {"description", "Connect robotic arm hardware to the system"}},
                {{"method", "POST"},
                 {"path", "/hardware/arm/:index/disconnect"},
                 {"description", "Disconnect robotic arm hardware from the system"}},
                {{"method", "GET"},
                 {"path", "/hardware/status"},
                 {"description",
                  "Get connection status of all hardware components"}},

                // Data retrieval endpoints
                {{"method", "GET"},
                 {"path", "/configurations"},
                 {"description",
                  "Get all configurations (cameras, arms, producers, systems, sessions)"}},
                {{"method", "GET"},
                 {"path", "/"},
                 {"description", "API information and endpoint listing"}}
            }}
        };
        res.set_content(info.dump(2), "application/json");
    });

    // Start server
    std::cout << "Loaded "
              << config_manager.get_configurations().cameras.size()
              << " cameras and "
              << config_manager.get_configurations().arms.size()
              << " arms from data.json" << std::endl;
    std::cout << "Starting Trossen SDK Backend on http://localhost:8080"
              << std::endl;

    if (!svr.listen("0.0.0.0", 8080)) {
        std::cerr << "Failed to start server on port 8080" << std::endl;
        return 1;
    }

    return 0;
}
