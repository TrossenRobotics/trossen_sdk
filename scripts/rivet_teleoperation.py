"""
Temporary script to demonstration teleopration of Rivet with the Bimanual Glides.

Requirements:
- trossen_base (dev branch): https://github.com/TrossenRobotics/trossen_base/tree/dev
    1. Python installation
    2. Start CAN interface (using README)
- trossen_arm-source (lightweight-leader branch):
    https://github.com/TrossenRobotics/trossen_arm-source/tree/actuate-demo
    1. Python installation


This is adapted from:
https://github.com/TrossenRobotics/input_controller/blob/8f73ef4d04a1b6d466649aeb36286f6c6782ad49/scripts/teleop.py

"""

import numpy as np
import trossen_arm
import trossen_base

ENABLE_LEFT = True
ENABLE_RIGHT = True
ENABLE_FOLLOWER = True
ENABLE_BASE = True

# IP addresses for the leader and follower
IP_LEFT_LEADER = "192.168.0.3"
IP_LEFT_FOLLOWER = "192.168.1.4"
IP_RIGHT_LEADER = "192.168.0.2"
IP_RIGHT_FOLLOWER = "192.168.1.5"


# Gripper force feedback parameters
LEADER_MAX = 20.0       # N - leader effort at full grip (not including offset)
FOLLOWER_MAX = 100.0    # N - follower effort at full grip
LEADER_OFFSET = 6.5     # N - leader opening offset

BASE_MIN = -1           # Min translational/rotational velocity (units/s and rad/s)
BASE_MAX = 1            # Max translational/rotational velocity (units/s and rad/s)
BASE_DEADZONE = 0.1     # Base set to 0 velocity if less than this value
BASE_LIFT_MAX = 8000    # Max lift velocity (motor units/s)

MIN_JOYSTICK = 0        # Joystick min value
MAX_JOYSTICK = 4095     # Joystick max value

# Gripper home offset (rad) so the follower gripper matches the leader's at home
GRIPPER_HOME_OFFSET_RIGHT = np.pi / 4
GRIPPER_HOME_OFFSET_LEFT = -np.pi / 4


def scale(value, val_min, val_max, scaled_min, scaled_max, scaled_deadzone=None):
    """ Scale a value linearly from val_min..val_max to scaled_min..scaled_max.
    Deadzone applied to scaled value (-scaled_deadzone->scaled_deadzone)"""

    scaled_val = ((value - val_min) / (val_max - val_min) * (scaled_max - scaled_min) + scaled_min)

    if scaled_deadzone:
        if abs(scaled_val) < scaled_deadzone:
            return 0
    return scaled_val


def configure_leader(ip, model):
    """Configure a leader arm and cap its gripper's open position."""
    driver = trossen_arm.TrossenArmDriver()
    driver.configure(model, trossen_arm.StandardEndEffector.wxai_v0_leader, ip, True)

    joint_limits = driver.get_joint_limits()
    joint_limits[-1].position_max = 0.05
    driver.set_joint_limits(joint_limits)

    return driver


def configure_follower(ip):
    """Configure a follower arm and tune its position-mode gains."""
    driver = trossen_arm.TrossenArmDriver()
    driver.configure(trossen_arm.Model.pro, trossen_arm.StandardEndEffector.pro_base, ip, True)

    motor_parameters = driver.get_motor_parameters()
    for i in range(7):
        motor_parameters[i][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[i][trossen_arm.Mode.position].velocity.imax = 0.0
    for i in range(3):
        motor_parameters[i][trossen_arm.Mode.position].position.kp = 16.0
    driver.set_motor_parameters(motor_parameters)

    return driver


def init_leader_modes(driver):
    driver.set_all_modes(trossen_arm.Mode.effort)
    driver.set_gripper_mode(trossen_arm.Mode.effort)
    driver.set_gripper_effort(LEADER_OFFSET, 0.2, False)


def init_follower_modes(driver):
    driver.set_all_modes(trossen_arm.Mode.position)
    driver.set_gripper_mode(trossen_arm.Mode.position)


def home_follower(driver, gripper_home_offset):
    # Follower gripper is offset to match the leader's gripper position at home
    driver.set_all_positions(
        np.array([0.0, 0.0, 0.0, 0.0, 0.0, gripper_home_offset, 0.0]), 1.0, True
    )


def stop_follower(driver):
    driver.set_all_modes(trossen_arm.Mode.position)
    driver.set_all_positions(np.zeros(driver.get_num_joints()), 2.0, True)


def scaled_leader_gripper_effort(follower_effort):
    """Cubic curve for more resistance at higher efforts and less at lower efforts,
    with an offset to keep the gripper open when not gripping."""
    effort_norm = min(abs(follower_effort) / FOLLOWER_MAX, 1.0)
    return LEADER_MAX * (effort_norm**3) + LEADER_OFFSET


def teleop_arm_step(leader, follower, gripper_home_offset):
    """Mirror one leader/follower arm pair for a single control loop iteration.

    Feeds follower efforts back to the leader, leader positions forward to the
    follower, and drives the follower gripper from the leader's gripper position
    while feeding the follower's gripper effort back as leader resistance.
    """
    positions = leader.get_all_positions()
    efforts = follower.get_all_efforts()

    # Feed the efforts from the follower robot to the leader robot
    leader.set_arm_efforts(
        np.array([efforts[0], efforts[1], efforts[2], -efforts[3], -efforts[4], efforts[5]]),
        0.0,
        False,
    )

    # Feed the positions from the leader robot to the follower robot
    follower.set_arm_positions(
        np.array([positions[0], positions[1], positions[2], -positions[3], -positions[4],
                positions[5] + gripper_home_offset]),
        0.0,
        False,
    )

    # Read the leader's gripper position and follower's gripper effort
    leader_gripper_position = leader.get_gripper_position()
    follower_gripper_effort = follower.get_gripper_effort()

    # Set the leader's gripper effort based on the follower's gripper effort
    leader.set_gripper_effort(
        scaled_leader_gripper_effort(follower_gripper_effort), 0.1, False
    )

    # Set the follower's gripper position to match the leader's position
    follower.set_gripper_position(leader_gripper_position, 0.0, False)


if __name__ == "__main__":
    # Enable/disable left and right arms

    print("Initializing the drivers...")
    if ENABLE_RIGHT:
        driver_right_leader = configure_leader(IP_RIGHT_LEADER, trossen_arm.Model.glide_right)
        if ENABLE_FOLLOWER:
            driver_right_follower = configure_follower(IP_RIGHT_FOLLOWER)
    if ENABLE_LEFT:
        driver_left_leader = configure_leader(IP_LEFT_LEADER, trossen_arm.Model.glide_left)
        if ENABLE_FOLLOWER:
            driver_left_follower = configure_follower(IP_LEFT_FOLLOWER)
    if ENABLE_BASE:
        base = trossen_base.TrossenBase()

    if ENABLE_RIGHT:
        init_leader_modes(driver_right_leader)
        if ENABLE_FOLLOWER:
            init_follower_modes(driver_right_follower)

    if ENABLE_LEFT:
        init_leader_modes(driver_left_leader)
        if ENABLE_FOLLOWER:
            init_follower_modes(driver_left_follower)

    print("Moving to home positions...")

    if ENABLE_BASE:
        if not base.wait_until_ready():
            raise RuntimeError("Base failed to become ready")

    if ENABLE_RIGHT and ENABLE_FOLLOWER:
        home_follower(driver_right_follower, GRIPPER_HOME_OFFSET_RIGHT)

    if ENABLE_LEFT and ENABLE_FOLLOWER:
        home_follower(driver_left_follower, GRIPPER_HOME_OFFSET_LEFT)

    print("Starting to teleoperate the robots...")

    try:
        base_velocity_linear_x = 0.0
        base_velocity_linear_y = 0.0
        base_velocity_angular_z = 0.0
        base_velocity_lift = 0          # MUST BE INT
        while True:

            ################################### LEADER COMMANDS ####################################
            if ENABLE_RIGHT:
                right_input = driver_right_leader.get_input_report()
                base_velocity_angular_z = -scale(right_input.joystick_x, MIN_JOYSTICK, MAX_JOYSTICK,
                                BASE_MIN, BASE_MAX, BASE_DEADZONE)

                right_up_btn = int(right_input.buttons & (1 << 0))
                right_down_btn = int(right_input.buttons & (1 << 2))

                base_velocity_lift = int(right_up_btn - right_down_btn) * BASE_LIFT_MAX

            if ENABLE_LEFT:
                left_input = driver_left_leader.get_input_report()
                base_velocity_linear_x = scale(left_input.joystick_x, MIN_JOYSTICK, MAX_JOYSTICK,
                                               BASE_MIN, BASE_MAX, BASE_DEADZONE)
                base_velocity_linear_y = -scale(left_input.joystick_y, MIN_JOYSTICK, MAX_JOYSTICK,
                                               BASE_MIN, BASE_MAX, BASE_DEADZONE)

            ###################################### FOLLOWERS #######################################
            if ENABLE_BASE:
                base.update_base()
                base.set_cmd_vels(base_velocity_linear_x, base_velocity_linear_y,
                                         base_velocity_angular_z)
                base.set_actuator_velocity(base_velocity_lift)

            if ENABLE_RIGHT and ENABLE_FOLLOWER:
                teleop_arm_step(driver_right_leader, driver_right_follower,
                                GRIPPER_HOME_OFFSET_RIGHT)

            if ENABLE_LEFT and ENABLE_FOLLOWER:
                teleop_arm_step(driver_left_leader, driver_left_follower, GRIPPER_HOME_OFFSET_LEFT)

    except KeyboardInterrupt:
        print("Moving to stop positions...")
        if ENABLE_RIGHT and ENABLE_FOLLOWER:
            stop_follower(driver_right_follower)

        if ENABLE_LEFT and ENABLE_FOLLOWER:
            stop_follower(driver_left_follower)

        if ENABLE_BASE:
            base.set_cmd_vels(0.0, 0.0, 0.0)
            base.set_actuator_velocity(0)