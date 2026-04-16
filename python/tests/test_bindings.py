"""Smoke tests for Trossen SDK Python bindings."""

import pytest


def test_import():
    """Module imports without error."""
    import trossen_sdk


def test_version():
    """Version string and constants are accessible."""
    import trossen_sdk as ts

    v = ts.version()
    assert v.startswith("v")
    assert isinstance(ts.VERSION_MAJOR, int)
    assert isinstance(ts.VERSION_MINOR, int)
    assert isinstance(ts.VERSION_PATCH, int)


def test_timestamp():
    """Timestamp types work correctly."""
    import trossen_sdk as ts

    t = ts.Timespec(sec=1, nsec=500000000)
    assert t.to_ns() == 1500000000
    assert ts.Timespec.from_ns(1500000000).sec == 1

    now = ts.make_timestamp_now()
    assert now.monotonic.sec > 0 or now.monotonic.nsec > 0


def test_record_types():
    """Record types can be constructed."""
    import trossen_sdk as ts

    # JointStateRecord
    now = ts.make_timestamp_now()
    jr = ts.JointStateRecord(
        ts=now, seq=0, id="test",
        positions=[1.0, 2.0], velocities=[0.0, 0.0], efforts=[0.0, 0.0]
    )
    assert len(jr.positions) == 2
    assert jr.id == "test"

    # Odometry2DRecord
    odom = ts.Odometry2DRecord()
    odom.pose.x = 1.0
    assert odom.pose.x == 1.0

    # TeleopJointStateRecord was removed from C++; verify it's gone
    assert not hasattr(ts, "TeleopJointStateRecord")


def test_null_backend():
    """NullBackend can be created and used."""
    import trossen_sdk as ts

    backend = ts.NullBackend()
    assert backend.open()
    backend.close()


def test_user_action_enum():
    """UserAction enum values are accessible."""
    import trossen_sdk as ts

    assert ts.UserAction.kContinue is not None
    assert ts.UserAction.kReRecord is not None
    assert ts.UserAction.kStop is not None


def test_keypress_enum():
    """KeyPress enum values are accessible."""
    import trossen_sdk as ts

    assert ts.KeyPress.kNone is not None
    assert ts.KeyPress.kRightArrow is not None


def test_mock_joint_producer():
    """MockJointStateProducer can be constructed."""
    import trossen_sdk as ts

    cfg = ts.MockJointStateProducerConfig()
    cfg.num_joints = 6
    cfg.rate_hz = 30.0
    cfg.id = "mock_arm"

    producer = ts.MockJointStateProducer(cfg)
    stats = producer.stats()
    assert stats.produced == 0


def test_mock_camera_producer():
    """MockCameraProducer can be constructed."""
    import trossen_sdk as ts

    cfg = ts.MockCameraProducerConfig()
    cfg.width = 320
    cfg.height = 240
    cfg.fps = 30
    cfg.stream_id = "mock_cam"

    producer = ts.MockCameraProducer(cfg)
    stats = producer.stats()
    assert stats.produced == 0


def test_config_types():
    """Configuration types can be constructed and have correct defaults."""
    import trossen_sdk as ts

    arm = ts.ArmConfig()
    assert arm.model == "wxai_v0"

    cam = ts.CameraConfig()
    assert cam.fps == 30

    prod = ts.ProducerConfig()
    assert prod.poll_rate_hz == 30.0

    teleop = ts.TeleoperationConfig()
    assert teleop.enabled is True


def test_scheduler_config():
    """Scheduler config types are accessible."""
    import trossen_sdk as ts

    cfg = ts.SchedulerConfig()
    assert cfg.high_res_cutover_ms == 5

    opts = ts.SchedulerTaskOptions()
    opts.name = "test_task"
    assert opts.name == "test_task"


def test_producer_metadata():
    """ProducerMetadata is accessible via a mock producer."""
    import trossen_sdk as ts

    cfg = ts.MockJointStateProducerConfig()
    prod = ts.MockJointStateProducer(cfg)
    meta = prod.metadata()
    assert meta is not None
