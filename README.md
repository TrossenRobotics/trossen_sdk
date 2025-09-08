# trossen_sdk


# Installation

## Apache Parquet

```bash
sudo apt update
sudo apt install -y -V ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt update
sudo apt install -y -V libarrow-dev # For C++
sudo apt install -y -V libarrow-glib-dev # For GLib (C)
sudo apt install -y -V libarrow-dataset-dev # For Apache Arrow Dataset C++
sudo apt install -y -V libarrow-dataset-glib-dev # For Apache Arrow Dataset GLib (C)
sudo apt install -y -V libarrow-acero-dev # For Apache Arrow Acero
sudo apt install -y -V libarrow-flight-dev # For Apache Arrow Flight C++
sudo apt install -y -V libarrow-flight-glib-dev # For Apache Arrow Flight GLib (C)
sudo apt install -y -V libarrow-flight-sql-dev # For Apache Arrow Flight SQL C++
sudo apt install -y -V libarrow-flight-sql-glib-dev # For Apache Arrow Flight SQL GLib (C)
sudo apt install -y -V libgandiva-dev # For Gandiva C++
sudo apt install -y -V libgandiva-glib-dev # For Gandiva GLib (C)
sudo apt install -y -V libparquet-dev # For Apache Parquet C++
sudo apt install -y -V libparquet-glib-dev # For Apache Parquet GLib (C)
```


## RealSense2 (2.55.1) 

2.56.1 version doesn't have support for Ubuntu 24.04/22.04. The installation completes but cmake configuration fails.
We use 2.55.1 version instead as a workaround. This allows realsense-viewer support.

Refer to [librealsense v2.56.4 breaks build due to CMake issue](https://github.com/IntelRealSense/librealsense/issues/14158)

For the current software stack we do not need `librealsense2-dkms`. So we can remove it from the ISO. This prevents any blocking of the dpkg.

List all source files

```bash
grep -i realsense /etc/apt/sources.list.d/*.list /etc/apt/sources.list 2>/dev/null
```

Example Output
```bash
/etc/apt/sources.list.d/librealsense2.list:deb https://packages.realsense.intel.com/ubuntu noble main
```

Edit to use Jammy as it has support for 2.55.1
```bash
sudo sed -i 's/noble/jammy/' /etc/apt/sources.list.d/librealsense.list
```

Update and Install
```bash
sudo apt update
apt list -a librealsense2 librealsense2-dev
```

Install the highest version
```bash
sudo apt install --allow-downgrades \
  librealsense2=2.55.1-0~realsense.12474 \
  librealsense2-dev=2.55.1-0~realsense.12474 \
  librealsense2-utils=2.55.1-0~realsense.12474 \
  librealsense2-gl=2.55.1-0~realsense.12474

```


## OpenCV

```bash
sudo apt update
sudo apt install libopencv-dev
```

## Nvidia Driver

List available drivers

```bash
ubuntu-drivers devices
```

Install recommended driver

```bash
sudo ubuntu-drivers autoinstall
```


## Dummy Control Command


        ("help", "produce help message")
        ("fps", po::value<double>(&fps)->default_value(30.0), "frames per second")
        ("single_task", po::value<std::string>(&single_task)->default_value("pick_place"), "single task name")
        ("repo_id", po::value<std::string>(&repo_id)->default_value("trossen-ai/trossen-widowx"), "HuggingFace repo ID for model")
        ("tags", po::value<std::vector<std::string>>(&tags)->multitoken(), "comma-separated list of tags")
        ("dataset", po::value<std::string>(&dataset_name)->default_value("test_dataset_01"), "dataset name")
        ("robot", po::value<std::string>(&robot_name)->default_value("trossen_ai_solo"), "robot name")
        ("num_episodes", po::value<int>(&num_episodes)->default_value(2), "number of episodes")
        ("recording_time", po::value<double>(&recording_time)->default_value(10.0), "recording time per episode (seconds)")
        ("warmup_time", po::value<double>(&warmup_time)->default_value(5.0), "warmup time for the robot arms (seconds)")
        ("reset_time", po::value<double>(&reset_time)->default_value(2.0), "reset time between episodes (seconds)")
        ("num_image_writer_threads_per_camera", po::value<int>(&num_image_writer_threads_per_camera)->default_value(4), "number of threads for image writer per camera")
        ("root", po::value<std::string>(&root)->default_value(""), "root directory for dataset")
        ("video",po::value<bool>(&video)->default_value(true), "flag to encode frames to video after each episode");
        ("run_compute_stats",po::value<bool>(&run_compute_stats)->default_value(false), "flag to compute statistics after all episodes");
        ("num_image_writer_processes", po::value<int>(&num_image_writer_processes)->default_value(1), "number of processes for image writer");
        ("display_cameras", po::value<bool>(&display_cameras)->default_value(true), "flag to display camera feeds during recording");
        ("overwrite", po::value<bool>(&overwrite)->default_value(false), "flag to overwrite existing dataset");


```bash
./control --robot trossen_ai_stationary \
--recording_time 10 \
--num_episodes 1 --fps 30 \
--display_cameras true \
--tags test \
--overwrite true \
--dataset test_dataset \
--root ~/.cache/huggingface/lerobot/ \
--repo_id trossen-ai/trossen-widowx \
--single_task pick_place \
--num_image_writer_threads_per_camera 4 \
--num_image_writer_processes 1 \
--video true \
--run_compute_stats true