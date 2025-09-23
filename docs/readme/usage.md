# Usage


You can run the following commands from the root of the repository after building the project.

## Recording a dataset

```bash
./build/record \
  --robot trossen_ai_stationary \
  --recording_time 10 \
  --num_episodes 1 --fps 30 \
  --display_cameras true \
  --tags test \
  --overwrite true \
  --dataset test_dataset_00 \
  --repo_id TrossenRoboticsCommunity \
  --single_task pick_place \
  --num_image_writer_threads_per_camera 4 \
  --num_image_writer_processes 1 \
  --video true \
  --run_compute_stats true
```

Arguments:

- `--robot`: The robot configuration to use. This should be one of the following: `trossen_ai_stationary` or `trossen_ai_solo`
- `--recording_time`: The duration (in seconds) for which to record each episode
- `--num_episodes`: The number of episodes to record
- `--fps`: The control frequency (in Hz)
- `--fps_tolerance`: Allowed deviation from the target FPS before warning (default: 0.05)
- `--warmup_time`: The time (in seconds) to warm up the robot arms before recording each episode
- `--reset_time`: The time (in seconds) to reset between episodes
- `--display_cameras`: Whether to display the camera feeds in a window (true/false)
- `--tags`: Comma-separated tags to associate with the dataset
- `--overwrite`: Whether to overwrite existing datasets (true/false)
- `--dataset`: The name of the dataset to create
- `--root`: The root directory where datasets will be stored (default: `~/.cache/trossen_dataset_collection_sdk/`)
- `--repo_id`: The HuggingFace repository ID where the dataset will be uploaded
- `--single_task`: The task being performed during the recording (e.g., `pick_place`, `stack_blocks`, etc.)
- `--num_image_writer_threads_per_camera`: Number of threads per camera for asynchronous image writing
- `--num_image_writer_processes`: Number of processes for image writing
- `--video`: Whether to save a video of the episode (true/false)
- `--run_compute_stats`: Whether to compute dataset statistics after recording (true/false)


## Replaying a dataset

```bash
./build/replay \
  --robot trossen_ai_stationary \
  --dataset test_dataset_00 \
  --repo_id TrossenRoboticsCommunity \
  --episode 0 \
  --fps 30
```

Arguments:

- `--robot`: The robot configuration to use. This should be one of the following: `trossen_ai_stationary` or `trossen_ai_solo`
- `--dataset`: The name of the dataset to replay
- `--root`: The root directory where datasets are stored (default: `~/.cache/trossen_dataset_collection_sdk/`)
- `--repo_id`: The HuggingFace repository ID where the dataset will be uploaded
- `--episode`: The episode number to replay
- `--fps`: The frames per second for the replay


## Put Arms to Sleep

To ensure the robotic arms are safely positioned prior to system shutdown, use the following command. This procedure helps prevent unintended movements and safely powers down the actuators:

```bash
./build/sleep --robot trossen_ai_stationary
```

Arguments:
- `--robot`: The robot configuration to use. This should be one of the following: `trossen_ai_stationary` or `trossen_ai_solo`

## Teleoperation

If you want to do a dry run of your experiment without recording, you can use the teleop script to control the robot.

```bash
./build/teleop --robot trossen_ai_stationary \
  --fps 30 \
  --teleop_time 10
```

Arguments:
- `--robot`: The robot configuration to use. This should be one of the following: `trossen_ai_stationary` or `trossen_ai_solo`
- `--fps`: Frames per second for teleoperation (default: 30)
- `--teleop_time`: Teleoperation time per episode in seconds (default: -1 for unlimited)
- `--display_cameras`: Whether to display camera feeds during teleoperation (1 for true, 0 for false; default: 1)
- `--fps_tolerance`: Allowed deviation from the target FPS before warning (default: 0.05)


If you want to use the dataset with LeRobot, please refer to the [LeRobot Compatible Usage](lerobot.md) section.