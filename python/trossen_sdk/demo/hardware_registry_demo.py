# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates the HardwareRegistry and ActiveHardwareRegistry
# systems using mock hardware (no real devices required).

# The script does the following:
# 1. Lists all registered hardware types
# 2. Creates a mock hardware component via a Python subclass
# 3. Creates hardware via the registry (uses built-in types)
# 4. Registers hardware in the ActiveHardwareRegistry
# 5. Retrieves and lists all active hardware
# 6. Cleans up

import trossen_sdk


class MockArmComponent(trossen_sdk.HardwareComponent):
    """A mock arm component for demo purposes (no real hardware needed)."""

    def __init__(self, identifier):
        super().__init__(identifier)
        self.num_joints = 6

    def configure(self, config):
        self.num_joints = config.get("num_joints", 6)
        print(f"  [ok] Configured '{self.get_identifier()}' ({self.num_joints} joints)")

    def get_type(self):
        return "mock_arm"

    def get_info(self):
        return {"type": "mock_arm", "num_joints": self.num_joints}


def main():
    print("\nHardware Registry Demo")
    print("======================\n")

    # 1. List registered hardware types
    print("Registered hardware types:")
    for hw_type in trossen_sdk.HardwareRegistry.get_registered_types():
        print(f"  - {hw_type}")
    print()

    # 2. Create mock hardware via Python subclass (not registered in ActiveHardwareRegistry)
    print("Creating mock hardware (Python subclass):")
    mock_arm = MockArmComponent("mock_arm_0")
    mock_arm.configure({"num_joints": 7})
    print(f"  Type: {mock_arm.get_type()}, Info: {mock_arm.get_info()}")

    # 3. Create hardware via the registry (auto-registered in ActiveHardwareRegistry)
    # Note: This requires real hardware drivers to be available, so we guard it.
    # For this demo, we show the API without calling into real hardware.
    print("\nUsing HardwareRegistry.create() (auto-registers in ActiveHardwareRegistry):")
    print("  (Skipped — requires real hardware drivers)")
    print(f"  ActiveHardwareRegistry count: {trossen_sdk.ActiveHardwareRegistry.count()}")

    # 4. Retrieve hardware from the active registry
    print("\nRetrieving from ActiveHardwareRegistry:")
    for hw_id in trossen_sdk.ActiveHardwareRegistry.get_ids():
        hw = trossen_sdk.ActiveHardwareRegistry.get(hw_id)
        print(f"  [ok] Retrieved '{hw.get_identifier()}' (type: {hw.get_type()})")

    # 5. List all active hardware
    print("\nActive hardware:")
    for hw_id in trossen_sdk.ActiveHardwareRegistry.get_ids():
        hw = trossen_sdk.ActiveHardwareRegistry.get(hw_id)
        print(f"  - {hw_id} ({hw.get_type()})")

    # 6. Cleanup
    trossen_sdk.ActiveHardwareRegistry.clear()
    print(f"\n[ok] Demo complete!\n")


if __name__ == "__main__":
    main()
