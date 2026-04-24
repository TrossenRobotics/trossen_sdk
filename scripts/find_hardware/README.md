# find_hardware

Discovers connected hardware of a chosen type (today: RealSense and OpenCV/V4L2 cameras), captures a preview image (or other side artifact) from each, and prints a summary table. Use the previews to identify which physical device maps to which serial number or device index, then populate your config files accordingly.

This executable is a thin CLI over `trossen::hw::DiscoveryRegistry`. Each hardware component that supports enumeration self-registers a `find()` function next to its existing `REGISTER_HARDWARE(...)` line; the registry dispatches by the same type key the config system uses (e.g. `"realsense_camera"`). Components with no enumeration API simply don't register — the CLI reports "no discovery support" for those types.

Which types appear in the registry depends on which vendor integrations the library was built with. Run `--help` to see exactly what is available in the current build.

## Usage

```bash
./build/scripts/find_hardware <type> [--output DIR]
```

Run once per hardware type you want to inspect, using the same type key you would use in config:

```bash
./build/scripts/find_hardware realsense_camera
./build/scripts/find_hardware opencv_camera
```

| Argument | Default | Description |
|----------|---------|-------------|
| `<type>` | — (required) | Hardware type key. Run `--help` for the list registered in this build |
| `--output DIR` | `./scripts/find_hardware/discovery` | Directory to write preview images |
| `--help` | | Show usage and list supported types |

## Output

```
scripts/find_hardware/discovery/
  realsense_<serial>.jpg    # from ./find_hardware realsense_camera
  opencv_<index>.jpg        # from ./find_hardware opencv_camera
```

Example summary table:

```
Identifier            Resolution  FPS   Preview
----------------------------------------------------------------------
230422271855          640x480     30    .../realsense_230422271855.jpg
230422271449          640x480     30    .../realsense_230422271449.jpg
```

The `--output` directory is cleared before each run, so re-invoke with different `--output` paths if you want to keep previews from multiple runs side by side.

## Adding discovery for a new hardware type

In the component's `.cpp`, alongside the existing `REGISTER_HARDWARE(...)` line:

```cpp
#include "trossen_sdk/hw/discovery_registry.hpp"

// ... existing implementation, including a static find() method ...

REGISTER_HARDWARE(MyComponent, "my_hardware")
REGISTER_HARDWARE_DISCOVERY(MyComponent, "my_hardware")
```

The `find()` method signature must be:

```cpp
static std::vector<DiscoveredHardware> find(const std::filesystem::path& output_dir);
```

No changes to `find_hardware.cpp`, CMake, or docs are required — the new type appears in `--help` automatically.

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
