# Configuration

This SDK supports controlling robotic systems consisting of Leader and Follower robots.
- The Leader (typically a WidowXAI arm) is used to teleoperate the Follower.
- The Follower executes the actual movement commands and records the dataset.

You can configure the arms and cameras via the JSON configuration files located in the `config/` directory.

Currently supported robot modalities are:

| Robot Modality | Description                                      | Configuration Files                                 |
|----------------|--------------------------------------------------|-----------------------------------------------------|
| `stationary`   | Bimanual robot with 2 arms and 4 cameras         | `trossen_ai_stationary.json`<br>`trossen_ai_stationary_leader.json` |
| `solo`         | Single arm robot with 2 cameras                  | `trossen_ai_solo.json`<br>`trossen_ai_solo_leader.json`             |


## Follower Robot Configuration

Below is an example configuration for a `bimanual WidowXAI Follower` robot using  `RealSense` cameras:

```json
{
  "type": "bimanual",
  "name": "bimanual_widowxai",
  "left_ip_address": "192.168.1.5",
  "right_ip_address": "192.168.1.4",
  "camera_interface": "realsense",
  "cameras": [
    {
      "name": "cam_head",
      "unique_identifier": "218622270304",
      "fps": 30,
      "width": 640,
      "height": 480,
      "use_depth": false
    },
    {
      "name": "cam_right_wrist",
      "unique_identifier": "130322272628",
      "fps": 30,
      "width": 640,
      "height": 480,
      "use_depth": false
    },
    {
      "name": "cam_left_wrist",
      "unique_identifier": "128422271347",
      "fps": 30,
      "width": 640,
      "height": 480,
      "use_depth": true
    },
   {
      "name": "cam_low",
      "unique_identifier": "218622274938",
      "fps": 30,
      "width": 640,
      "height": 480,
      "use_depth": false
    }
  ]
}
```
Configuration Parameters:

- `type`: Either `bimanual` or `solo` depending on the robot modality.
- `name`: A name to identify the robot. This is used in the dataset metadata.
- `left_ip_address` and `right_ip_address`: The IP addresses of the left and right arms respectively. Refer to the [Arm Network Setup Guide](https://docs.trossenrobotics.com/trossen_arm/main/getting_started/software_setup.html#arm-network-setup).
- `camera_interface`: Chose between `realsense` or `opencv`.
    - `realsense`: via Intel RealSense SDK.
    - `opencv`: via OpenCV. This is useful for generic USB cameras. Note that depth cameras are not supported with the `opencv` interface.
- `cameras`: A list of camera configurations. Each camera configuration contains the following parameters:
    - `name`: A name to identify the camera. This is used in the dataset metadata.
    - `unique_identifier`: The unique identifier for the camera. For RealSense cameras, this is the serial number. For OpenCV cameras, this is the index.
    - `fps`: Frames per second for image capture.
    - `width`: Width of the camera image.
    - `height`: Height of the camera image.
    - `use_depth`: Boolean flag to indicate if depth images should be captured. This is only applicable for RealSense cameras.


How to find camera unique identifiers:

For Realsense Cameras:
- Use the realsense-viewer application to find the serial number of your camera. More details [here](https://docs.trossenrobotics.com/trossen_arm/main/tutorials/lerobot/configuration.html#camera-unique_identifier-number).

For OpenCV Cameras:
- On first run, the program will scan through all available cameras and store images from the cameras in the output folder along with their indices. You can use this index to configure the camera in the configuration file.
- You can check the output folder to match camera views with their corresponding index and update the configuration file accordingly.

## Leader Robot Configuration

Below is an example configuration for a `bimanual WidowXAI Leader` robot:

```json
{
  "type": "bimanual",
  "name": "bimanual_widowxai_leader",
  "left_ip_address": "192.168.1.3",
  "right_ip_address": "192.168.1.2"
}
```

Configuration Parameters:
- `type`: Either `bimanual` or `solo` depending on the robot modality.
- `name`: A name to identify the robot. This is used in the dataset metadata.
- `left_ip_address` and `right_ip_address`: The IP addresses of the left and right arms respectively. Refer to the [Arm Network Setup Guide](https://docs.trossenrobotics.com/trossen_arm/main/getting_started/software_setup.html#arm-network-setup).



Once the configuration is done you can now move to the [Usage](usage.md) section to learn how to use the SDK.