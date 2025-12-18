#pragma once
#include "../../i_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

struct McapBackendConfig : public IConfig {
    std::string output_dir;
    std::string robot_name{"/robot/joint_states"};
    int chunk_size_bytes{4 * 1024 * 1024};
    std::string compression{""};
    std::string dataset_id;
    int episode_index{0};

    std::string type() const override { return "mcap_backend"; }

    static  McapBackendConfig from_json(const nlohmann::json& j) {
        McapBackendConfig c;
        c.output_dir = j.at("output_dir").get<std::string>();
        c.robot_name = j.value("robot_name", "/robot/joint_states");
        c.chunk_size_bytes = j.value("chunk_size_bytes", 4 * 1024 * 1024);
        c.compression = j.value("compression", "");
        c.dataset_id = j.at("dataset_id").get<std::string>();
        c.episode_index = j.value("episode_index", 0);

        return c;
    }
};

REGISTER_CONFIG(McapBackendConfig, "mcap_backend");
