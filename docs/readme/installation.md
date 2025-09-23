
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

Once the installation is complete, you can now move to the [Configuration](configuration.md) section to learn how to configure the SDK for your robot setup.