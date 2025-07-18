#include "trossen_sdk_utils/control_utils.hpp"
#include "trossen_dataset/dataset.hpp"

namespace trossen_sdk {

void ControlUtils::control_loop(trossen_ai_robot_devices::TrossenAIStationary* robot,
                                float control_time,
                                trossen_dataset::TrossenAIDataset& dataset) {
    using steady_clock = std::chrono::steady_clock;
    using time_point = std::chrono::steady_clock::time_point;

    trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer(4);

    for (auto& arm : robot->leader_arms_) {
        arm->write("external_efforts", std::vector<double>(7, 0.0));
    }

    auto start_time = steady_clock::now();
    auto end_time = start_time + std::chrono::duration<float>(control_time);
    
    trossen_dataset::State state;
    int episode_idx = dataset.get_num_episodes();
    trossen_dataset::EpisodeData episode_data(episode_idx);

    while (steady_clock::now() < end_time) {
        auto loop_start_time = steady_clock::now();

        state = robot->teleop_step(episode_data);

        trossen_dataset::FrameData frame_data;
        frame_data.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        frame_data.observation_state = state.observation_state;
        frame_data.action = state.action;
        frame_data.episode_idx = episode_idx;
        frame_data.frame_idx = episode_data.get_frames().size();
        episode_data.add_frame(frame_data);

        std::string image_folder_path = dataset.get_image_path();
        for (const auto& image_data : state.images) {
            std::string episode_folder_name = "episode_" + std::to_string(episode_idx);
            std::filesystem::path camera_folder = std::filesystem::path(image_folder_path) / image_data.camera_name / episode_folder_name;
            std::filesystem::create_directories(camera_folder);
            std::string image_file_path = (camera_folder / image_data.file_path).string();
            image_writer.push(image_data.image, image_file_path);
        }

        busy_wait_until(loop_start_time, 30.0);  // 30 Hz loop

        auto loop_duration = std::chrono::duration_cast<std::chrono::duration<double>>(steady_clock::now() - loop_start_time).count();
        // TODO: Improve this logging to be more elegant and less verbose
        if (loop_duration > 1 / 29.0) {
            trossen_sdk_utils::log_warning("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx)
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        }
        else {
            trossen_sdk_utils::log_info("Loop duration: " + std::to_string(loop_duration) + " seconds"
                                    + " | Frequency: " + std::to_string(1.0 / loop_duration) + " Hz"
                                    + " | Episode: " + std::to_string(episode_idx)
                                    + " | Frame: " + std::to_string(frame_data.frame_idx));
        }
    }

    dataset.save_episode(episode_data);
    std::cout << "Control loop finished." << std::endl;
    robot->teleop_safety_stop();
}





} // namespace trossen_sdk