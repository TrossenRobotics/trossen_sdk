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

## Build and Install trossen_sdk

```bash
git clone https://github.com/TrossenRobotics/trossen_sdk.git
cd trossen_sdk
mkdir build && cd build
cmake ..
make
```


# Usage
## Recording a dataset

```bash
./record \
--robot_ip <robot_ip> \
--recording_time 10 \
--num_episodes 1 --fps 30 \
--display_cameras true \
--tags test \
--overwrite false \
--dataset test_dataset_00 \
--root ~/.cache/huggingface/lerobot/ \
--repo_id trossen-ai/trossen-widowx \
--single_task pick_place \
--num_image_writer_threads_per_camera 4 \
--num_image_writer_processes 1 \
--video true \
--run_compute_stats true
```

### Uploading the dataset to HuggingFace

```bash
 huggingface-cli upload TrossenRoboticsCommunity/record-test ~/.cache/huggingface/lerobot/TrossenRoboticsCommunity/test_dataset_03 --repo-type dataset --revision v2.1 --exclude *.jpg
```

The `--exclude *.jpg` flag is used to avoid uploading large image files.
The revision tag is important for passing the version checks in the online dataset viewer.

```bash
 huggingface-cli upload TrossenRoboticsCommunity/record-test ~/.cache/huggingface/lerobot/TrossenRoboticsCommunity/test_dataset_03 --repo-type dataset --revision v2.1
```

The above command uploads all files including images.


# Replaying a dataset

```bash
./replay -d test_dataset_01 --robot trossen_ai_stationary --episode 0 --repo TrossenRoboticsCommunity --root ~/.cache/huggingface/lerobot/ --fps 30
```

### Put Arms to Sleep

```bash
./sleep --robot trossen_ai_stationary
```

### Teleoperation

```bash
./teleop --robot trossen_ai_stationary --fps 30
```