#pragma once
#include "../../i_config.hpp"
// #include "../../json.hpp"
#include "../../config_registry.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

struct LeRobotBackendConfig : public IConfig {
    std::string output_dir;
    int encoder_threads{1};
    int max_image_queue{0};
    int png_compression_level{3};
    bool overwrite_existing{false};
    bool encode_videos{false};
    std::string task_name{"default_task"};
    std::string repository_id{"default_repo"};
    std::string dataset_id{"default_dataset"};
    std::string root_path{"/data/trossen"};
    int episode_index{0};
    std::string robot_name{"trossen_ai_generic"};
    float fps{30.0f};

    std::string type() const override { return "lerobot_backend"; }

    static LeRobotBackendConfig from_json(const nlohmann::json& j) {
        LeRobotBackendConfig c;
        c.output_dir = j.at("output_dir").get<std::string>();
        c.encoder_threads = j.value("encoder_threads", 1);
        c.max_image_queue = j.value("max_image_queue", 0);
        c.png_compression_level = j.value("png_compression_level", 3);
        // c.drop_policy = j.value("drop_policy", "DropNewest");
        c.overwrite_existing = j.value("overwrite_existing", false);
        c.encode_videos = j.value("encode_videos", false);
        c.task_name = j.value("task_name", "default_task");
        c.repository_id = j.value("repository_id", "default_repo");
        c.dataset_id = j.value("dataset_id", "default_dataset");
        c.root_path = j.value("root_path", trossen::io::backends::get_default_root_path().string());
        c.episode_index = j.value("episode_index", 0);
        c.robot_name = j.value("robot_name", "trossen_ai_generic");
        c.fps = j.value("fps", 30.0f);

        return c;
    }
};

REGISTER_CONFIG(LeRobotBackendConfig, "lerobot_backend");
