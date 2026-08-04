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
IP_LEFT_LEADER = "192.168.5.3"
IP_LEFT_FOLLOWER = "192.168.1.4"
IP_RIGHT_LEADER = "192.168.5.2"
IP_RIGHT_FOLLOWER = "192.168.1.5"


# Gripper force feedback parameters
LEADER_MAX = 20.0       # N - leader effort at full grip (not including offset)
FOLLOWER_MAX = 100.0    # N - follower effort at full grip
LEADER_OFFSET = 6.5     # N - leader opening offset
Y_ARM_MOUNT_OFFSET_RIGHT = 0.28   # m - right arm mount offset in y direction
Y_ARM_MOUNT_OFFSET_LEFT = -0.28   # m - left arm mount offset in y direction

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


def nearest_point_on_circle(radius, x, y):
    """Return the nearest point on a circle of given radius to the point (x, y)."""
    dist = np.sqrt(x**2 + y**2)
    if dist == 0:
        return radius, 0

    nearest_x = x / dist * radius
    nearest_y = y / dist * radius
    return nearest_x, nearest_y



def configure_leader(ip, model):
    """Configure a leader arm and cap its gripper's open position."""
    driver = trossen_arm.TrossenArmDriver()
    driver.configure(model, trossen_arm.StandardEndEffector.wxai_v0_leader, ip, True)

    joint_limits = driver.get_joint_limits()
    joint_limits[-1].position_max = 0.05
    for i in range (6):
        joint_limits[i].velocity_tolerance = joint_limits[i].velocity_max
        joint_limits[i].effort_tolerance = joint_limits[i].effort_max
    driver.set_joint_limits(joint_limits)

    return driver


def configure_follower(ip):
    """Configure a follower arm and tune its position-mode gains."""
    driver = trossen_arm.TrossenArmDriver()
    driver.configure(trossen_arm.Model.pro, trossen_arm.StandardEndEffector.pro_base, ip, True)

    joint_limits = driver.get_joint_limits()
    for i in range (6):
        joint_limits[i].velocity_tolerance = joint_limits[i].velocity_max
        joint_limits[i].effort_tolerance = joint_limits[i].effort_max
    driver.set_joint_limits(joint_limits)


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


def teleop_arm_step(leader, follower, gripper_home_offset, y_arm_mount_offset, temp_side):
    """Mirror one leader/follower arm pair for a single control loop iteration.

    Feeds follower efforts back to the leader, leader positions forward to the
    follower, and drives the follower gripper from the leader's gripper position
    while feeding the follower's gripper effort back as leader resistance.
    """

    # TODO: REMOVE
    # efforts = follower.get_all_efforts()

    # # Feed the efforts from the follower robot to the leader robot
    # leader.set_arm_efforts(
    #     np.array([efforts[0], efforts[1], efforts[2], -efforts[3], -efforts[4], efforts[5]]),
    #     0.0,
    #     False,
    # )


    # Read the leader's gripper position and follower's gripper effort
    leader_gripper_position = leader.get_gripper_position()
    follower_gripper_effort = follower.get_gripper_effort()

    # Set the leader's gripper effort based on the follower's gripper effort
    leader.set_gripper_effort(
        scaled_leader_gripper_effort(follower_gripper_effort), 0.1, False
    )

    # Set the follower's gripper position to match the leader's position
    follower.set_gripper_position(leader_gripper_position, 0.0, False)


    # Safety check if arms could hit cameras
    SAFETY_RADIUS_M =  0.3
    x, y, z, rx, ry, rz = leader.get_cartesian_position()
    y -= y_arm_mount_offset
    if (x**2 + y**2) < SAFETY_RADIUS_M**2:
        print(f"Safety limit reached, {temp_side}: x={x}, y={y}")

        # nearest_x, nearest_y = nearest_point_on_circle(SAFETY_RADIUS_M, x, y)
        # nearest_y += y_arm_mount_offset

        # follower.set_cartesian_position(nearest_x, nearest_y, z, rx, ry, rz,
        #     trossen_arm.InterpolationSpace.cartesian, 0.5, False)
    # else:
        # Feed the positions directly from the leader robot to the follower robot
    positions = leader.get_all_positions()
    follower.set_arm_positions(
        np.array([positions[0], positions[1], positions[2], -positions[3], -positions[4],
                positions[5] + gripper_home_offset]),
        0.2,
        False,
    )



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

                # On a new fault, print what failed, then clear and re-enable to resume
                faulted = base.has_fault()
                if faulted:
                    for fault in base.get_faults():
                        print(fault)
                    raise RuntimeError("Base faults detected")

                base.set_cmd_vels(base_velocity_linear_x, base_velocity_linear_y,
                                         base_velocity_angular_z)
                base.set_actuator_velocity(base_velocity_lift)

            if ENABLE_RIGHT and ENABLE_FOLLOWER:
                teleop_arm_step(driver_right_leader, driver_right_follower,
                                GRIPPER_HOME_OFFSET_RIGHT, Y_ARM_MOUNT_OFFSET_RIGHT, "right")

            if ENABLE_LEFT and ENABLE_FOLLOWER:
                teleop_arm_step(driver_left_leader, driver_left_follower, GRIPPER_HOME_OFFSET_LEFT,
                                Y_ARM_MOUNT_OFFSET_LEFT, "left")

    except KeyboardInterrupt:
        print("Moving to stop positions...")
        if ENABLE_RIGHT and ENABLE_FOLLOWER:
            stop_follower(driver_right_follower)

        if ENABLE_LEFT and ENABLE_FOLLOWER:
            stop_follower(driver_left_follower)

        if ENABLE_BASE:
            base.set_cmd_vels(0.0, 0.0, 0.0)
            base.set_actuator_velocity(0)
