# Trossen Robotics Data Collection SDK

This SDK provides tools for recording and replaying datasets using Trossen Robotics arms, specifically the WidowXAI series arms and kits.
The goal of this SDK is to facilitate data collection for robotics research and development using C++. We made the datasets compatible with HuggingFace(LeRobot) dataset. This allows user to train models using the datasets recorded with this SDK.


# Installation


## Trossen Arm Lib

If the repo is not already cloned, clone it using the following command:
```bash
git clone https://github.com/TrossenRobotics/trossen_arm.git
```

If you want to use the Trossen Arm Lib, you need to build and install it manually. Follow these steps:
```bash
cd trossen_arm
mkdir build
cd build
cmake ..
sudo make install
```

## Apache Parquet

In order to read and write Parquet files, you need to install the Apache Parquet C++ library. You can do this by adding the Apache Arrow APT repository and installing the necessary packages. This was tested on Ubuntu 24.04.

```bash
sudo apt update
sudo apt install -y -V ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt update
sudo apt install -y -V \
libarrow-dev \
libarrow-glib-dev \
libarrow-dataset-dev \
libarrow-dataset-glib-dev \
libarrow-acero-dev \
libarrow-flight-dev \
libarrow-flight-glib-dev \
libarrow-flight-sql-dev \
libarrow-flight-sql-glib-dev \
libgandiva-dev \
libgandiva-glib-dev \
libparquet-dev \
libparquet-glib-dev
```


## RealSense2 (2.55.1) 

If for any reason you need to reinstall or upgrade/downgrade librealsense2, follow these steps.

Note:
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

If OpenCV is not installed, install it using the following command:

```bash
sudo apt update
sudo apt install libopencv-dev
```

## Nvidia Driver

If you are using a system with an Nvidia GPU, you may need to install the appropriate Nvidia driver for your GPU model.
This does not play any role in the SDK functionality but is required for the realsense-viewer to work properly. This drivers will be used to display the camera streams using the realsense-viewer.

List available drivers

```bash
ubuntu-drivers devices
```

Install recommended driver

```bash
sudo ubuntu-drivers autoinstall
```

The following is an example output of `nvidia-smi` command on a system with an Nvidia RTX 5090 GPU.
This is the setup used for testing the SDK.
```bash
Fri Sep 12 10:49:47 2025
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 580.65.06              Driver Version: 580.65.06      CUDA Version: 13.0     |
+-----------------------------------------+------------------------+----------------------+
| GPU  Name                 Persistence-M | Bus-Id          Disp.A | Volatile Uncorr. ECC |
| Fan  Temp   Perf          Pwr:Usage/Cap |           Memory-Usage | GPU-Util  Compute M. |
|                                         |                        |               MIG M. |
|=========================================+========================+======================|
|   0  NVIDIA GeForce RTX 5090        On  |   00000000:02:00.0  On |                  N/A |
|  0%   43C    P8             19W /  575W |    1124MiB /  32607MiB |      0%      Default |
|                                         |                        |                  N/A |
+-----------------------------------------+------------------------+----------------------+

+-----------------------------------------------------------------------------------------+
| Processes:                                                                              |
|  GPU   GI   CI              PID   Type   Process name                        GPU Memory |
|        ID   ID                                                               Usage      |
|=========================================================================================|
|    0   N/A  N/A            2372      G   /usr/lib/xorg/Xorg                      553MiB |
|    0   N/A  N/A            2620      G   /usr/bin/gnome-shell                     56MiB |
|    0   N/A  N/A            3568      G   ...exec/xdg-desktop-portal-gnome          9MiB |
|    0   N/A  N/A            5273      G   ...slack/212/usr/lib/slack/slack         51MiB |
|    0   N/A  N/A            5798      G   ...85b68b8157e8c917bdaf1308fa936        111MiB |
|    0   N/A  N/A           38204      G   /proc/self/exe                           93MiB |
|    0   N/A  N/A           48960      G   /usr/bin/gnome-control-center            21MiB |
|    0   N/A  N/A           79911      G   /usr/bin/nautilus                        20MiB |
+-----------------------------------------------------------------------------------------+
```

## Build the SDK

Clone the repo if you haven't already

```bash
git clone https://github.com/TrossenRobotics/trossen_sdk.git
cd trossen_sdk
```

Create a build directory and navigate to it

```bash
mkdir build
cd build
```

Run cmake to configure the project

```bash
cmake ..
```

Build the project using make

```bash
make
```


# Usage

TODO [TDS-42](shantanuparab-tr): Add instructions for configuring up the robot arms and cameras.

## Recording a dataset

```bash
./record \
  --robot_name trossen_ai_stationary \
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

## Replaying a dataset

```bash
./replay -d test_dataset_01 --robot trossen_ai_stationary --episode 0 --repo TrossenRoboticsCommunity --root ~/.cache/huggingface/lerobot/ --fps 30
```

## Put Arms to Sleep

To ensure the robotic arms are safely positioned prior to system shutdown, use the following command. This procedure helps prevent unintended movements and safely powers down the actuators:

```bash
./sleep --robot trossen_ai_stationary
```

## Teleoperation

If you want to do a dry run of your experiment without recording, you can use the teleop script to control the robot.

```bash
./teleop --robot trossen_ai_stationary --fps 30
```

# LeRobot Compatibility

## Uploading the dataset to HuggingFace

You need to setup the HuggingFace CLI and authenticate first. Follow the instructions [here](https://huggingface.co/docs/lerobot/il_robots#record-a-dataset).

Uploading the dataset allows you to visualize it in the online dataset viewer and also use it for training models using the HuggingFace tools.

As the LeRobot dataset does not expect you to upload large image files, you can exclude them using the `--exclude` flag. This does not delete the files locally, it just prevents them from being uploaded to HuggingFace. This will not affect the usability of the dataset for training models as the LeRobot models use video files instead of individual images.

```bash
 huggingface-cli upload TrossenRoboticsCommunity/record-test ~/.cache/huggingface/lerobot/TrossenRoboticsCommunity/test_dataset_03 --repo-type dataset --revision v2.1 --exclude *.jpg
```

Note:
  * The `--exclude *.jpg` flag is used to avoid uploading large image files.
  * The revision tag is important for passing the version checks in the online dataset viewer.


In case you want to upload all files including images, you can omit the `--exclude` flag.

```bash
 huggingface-cli upload TrossenRoboticsCommunity/record-test ~/.cache/huggingface/lerobot/TrossenRoboticsCommunity/test_dataset_03 --repo-type dataset --revision v2.1
```

The above command uploads all files including images.


## Replaying using LeRobot

If you have `LeRobot` frame work installed, you can also replay using the replay scripts.
Note: This was tested using the new api integration for Trossen Arms with LeRobot.

```bash
python -m lerobot.replay
--robot.type=bi_widowxai_follower
--robot.left_arm_ip_address=192.168.1.5
--robot.right_arm_ip_address=192.168.1.4
--robot.id=bimanual_follower
--dataset.repo_id=TrossenRoboticsCommunity/test_dataset_03
--dataset.episode=0
```

## Visualizing the dataset

You can visualize the recorded dataset using the `lerobot` dataset viewer. This requires you to have the `lerobot` package installed.

```bash
 python src/lerobot/scripts/visualize_dataset_html.py  --repo-id TrossenRoboticsCommunity/test_dataset_03
```

To view the dataset online in the HuggingFace dataset viewer, you can use the following link:

[LeRobot Dataset Viewer](https://huggingface.co/spaces/lerobot/visualize_dataset)

Just dataset name and repo id in the input box and click on "Go".
