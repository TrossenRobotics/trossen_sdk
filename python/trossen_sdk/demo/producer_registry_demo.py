# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates the ProducerRegistry system — creating producers
# via the registry with both hardware-backed and mock producers.

# The script does the following:
# 1. Lists all registered producer types
# 2. Creates a MockJointStateProducer (no hardware needed)
# 3. Creates a MockCameraProducer (no hardware needed)
# 4. Polls each producer and collects emitted records
# 5. Displays producer stats

import trossen_sdk


def main():
    print("\nProducer Registry Demo")
    print("======================\n")

    # 1. List registered producer types
    print("Registered producer types:")
    for prod_type in trossen_sdk.ProducerRegistry.get_registered_types():
        print(f"  - {prod_type}")

    print("\nRegistered push producer types:")
    for prod_type in trossen_sdk.PushProducerRegistry.get_registered_types():
        print(f"  - {prod_type}")

    # 2. Create a mock joint producer (no hardware)
    print("\nStep 1: Create mock joint producer")
    joint_cfg = trossen_sdk.MockJointStateProducerConfig()
    joint_cfg.num_joints = 7
    joint_cfg.rate_hz = 30.0
    joint_cfg.id = "mock_arm/joints"
    joint_cfg.amplitude = 1.0

    joint_producer = trossen_sdk.MockJointStateProducer(joint_cfg)
    print(f"  [ok] Created MockJointStateProducer (stream: {joint_cfg.id})")

    # 3. Create a mock camera producer (no hardware)
    print("\nStep 2: Create mock camera producer")
    cam_cfg = trossen_sdk.MockCameraProducerConfig()
    cam_cfg.width = 320
    cam_cfg.height = 240
    cam_cfg.fps = 30
    cam_cfg.stream_id = "mock_cam/color"
    cam_cfg.encoding = "rgb8"
    cam_cfg.pattern = trossen_sdk.MockCameraPattern.Gradient

    cam_producer = trossen_sdk.MockCameraProducer(cam_cfg)
    print(f"  [ok] Created MockCameraProducer (stream: {cam_cfg.stream_id})")

    # 4. Poll producers and collect records
    print("\nStep 3: Poll producers")
    records = []

    def emit_callback(record):
        records.append(record)

    joint_producer.poll(emit_callback)
    cam_producer.poll(emit_callback)
    print(f"  [ok] Emitted {len(records)} records")

    for rec in records:
        print(f"    - {type(rec).__name__} (id: {rec.id})")

    # 5. Display stats
    print("\nStep 4: Producer stats")
    js = joint_producer.stats()
    print(f"  Joint producer: produced={js.produced}, dropped={js.dropped}")
    cs = cam_producer.stats()
    print(f"  Camera producer: produced={cs.produced}, dropped={cs.dropped}")

    print(f"\n[ok] Demo complete!\n")


if __name__ == "__main__":
    main()
