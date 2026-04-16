# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates the BackendRegistry system — creating backends at
# runtime without hardcoded type checking.

# The script does the following:
# 1. Lists registered backend types
# 2. Creates a NullBackend via the registry
# 3. Writes a JointStateRecord through the backend
# 4. Closes the backend

import trossen_sdk


def main():
    print("\nBackend Registry Demo")
    print("=====================\n")

    # 1. Check which backends are registered
    print("Checking registered backends:")
    for backend_type in ("null", "trossen_mcap", "lerobot_v2"):
        registered = trossen_sdk.BackendRegistry.is_registered(backend_type)
        if registered:
            print(f"  Is Registered: {backend_type}")
    print()

    # 2. Create a null backend via the registry
    backend = trossen_sdk.BackendRegistry.create("null")
    backend.open()

    # 3. Write some data
    record = trossen_sdk.JointStateRecord(
        ts=trossen_sdk.make_timestamp_now(),
        seq=0,
        id="demo/joints",
        positions=[0.1, 0.2, 0.3],
        velocities=[0.0, 0.0, 0.0],
        efforts=[0.0, 0.0, 0.0],
    )

    backend.write(record)
    backend.flush()
    backend.close()

    print("Successfully wrote data through registry-created backend!\n")


if __name__ == "__main__":
    main()
