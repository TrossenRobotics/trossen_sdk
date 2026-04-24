# find_cameras

Discovers all connected RealSense and OpenCV/V4L2 cameras, captures a preview image from each, and prints a summary table. Use the previews to identify which physical camera maps to which serial number or device index, then populate your config files accordingly.

## Build

```bash
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && cmake --build build --target find_cameras
```

## Usage

```bash
./build/scripts/find_cameras [--output DIR]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--output DIR` | `./scripts/find_cameras/camera_discovery` | Directory to write preview images |
| `--help` | | Show usage |

## Output

```
scripts/find_cameras/camera_discovery/
  realsense_<serial>.jpg
  opencv_<index>.jpg
```

Example summary table:

```
Type        Identifier             Resolution  FPS   Preview
--------------------------------------------------------------------------------
realsense   230422271855           640x480     30    .../realsense_230422271855.jpg
realsense   230422271449           640x480     30    .../realsense_230422271449.jpg
opencv      0                      640x480     30    .../opencv_0.jpg
```

## Config mapping

Once you've identified your cameras, use the serial number or device index in your config:

```json
"hardware": {
  "cameras": [
    {
      "id": "camera_high",
      "type": "realsense_camera",
      "serial_number": "230422271855",
      "width": 640, "height": 480, "fps": 30, "use_depth": false
    },
    {
      "id": "camera_wrist",
      "type": "opencv_camera",
      "device_index": 0,
      "width": 640, "height": 480, "fps": 30, "backend": "v4l2"
    }
  ]
}
```
