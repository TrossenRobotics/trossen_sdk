"""
Temporary script to demonstration teleopration of Rivet with the Bimanual Glides.

Requirements:
- trossen_base (dev branch): https://github.com/TrossenRobotics/trossen_base/tree/dev
    1. Python installation
    2. Start CAN interface (using README)
- trossen_arm-source (lightweight-leader branch):
    https://github.com/TrossenRobotics/trossen_arm-source/tree/lightweight-leader
    1. Python installation


This is adapted from:
https://github.com/TrossenRobotics/input_controller/blob/8f73ef4d04a1b6d466649aeb36286f6c6782ad49/scripts/teleop.py

"""


import time
import numpy as np
import trossen_arm
import trossen_base


ENABLE_RIGHT = True
ENABLE_FOLLOWER = False
ENABLE_LEFT = False
ENABLE_BASE = True

# IP addresses for the leader and follower
IP_RIGHT_LEADER = "192.168.1.2"
IP_RIGHT_FOLLOWER = "192.168.1.2"
IP_LEFT_LEADER = "192.168.1.3"
IP_LEFT_FOLLOWER = "192.168.1.5"

# Gripper force feedback parameters
LEADER_MAX = 27.0       # N - leader effort at full grip (not including offset)
FOLLOWER_MAX = 87.5     # N - follower effort at full grip
LEADER_OFFSET = 8.0      # N - leader opening offset

BASE_MIN = -1           # Min translational/rotational velocity (m/s and rad/s)
BASE_MAX = 1            # Max translational/rotational velocity (m/s and rad/s)
BASE_LIFT_MAX = 8000     # 10000   # Max lift velocity (motor units/s)

MIN_JOYSTICK = 0        # Joystick min value
MAX_JOYSTICK = 4095     # Joystick max value


def scale(value, val_min, val_max, scaled_min, scaled_max):
    """ Scale a value linearly from val_min..val_max to scaled_min..scaled_max """
    scaled_val = ((value - val_min) / (val_max - val_min) * (scaled_min - scaled_max) + scaled_max)
    return scaled_val


if __name__ == "__main__":
    # Enable/disable left and right arms

    print("Initializing the drivers...")
    if ENABLE_RIGHT:
        driver_right_leader = trossen_arm.TrossenArmDriver()
        if ENABLE_FOLLOWER:
            driver_right_follower = trossen_arm.TrossenArmDriver()
    if ENABLE_LEFT:
        driver_left_leader = trossen_arm.TrossenArmDriver()
        if ENABLE_FOLLOWER:
            driver_left_follower = trossen_arm.TrossenArmDriver()
    if ENABLE_BASE:
        base = trossen_base.TrossenBase()

    print("Configuring the drivers...")

    if ENABLE_RIGHT:
        driver_right_leader.configure(
            trossen_arm.Model.glide_right,
            trossen_arm.StandardEndEffector.wxai_v0_leader,
            IP_RIGHT_LEADER,
            True,
        )

        if ENABLE_FOLLOWER:
            driver_right_follower.configure(
                trossen_arm.Model.wxai_v0,
                trossen_arm.StandardEndEffector.wxai_v0_follower,
                IP_RIGHT_FOLLOWER,
                True,
            )

    if ENABLE_LEFT:
        driver_left_leader.configure(
            trossen_arm.Model.glide_left,
            trossen_arm.StandardEndEffector.wxai_v0_leader,
            IP_LEFT_LEADER,
            True,
        )

        if ENABLE_FOLLOWER:
            driver_left_follower.configure(
                trossen_arm.Model.wxai_v0,
                trossen_arm.StandardEndEffector.wxai_v0_follower,
                IP_LEFT_FOLLOWER,
                True,
            )

    if ENABLE_RIGHT and ENABLE_FOLLOWER:
        motor_parameters = driver_right_follower.get_motor_parameters()
        motor_parameters[0][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[0][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[0][trossen_arm.Mode.position].position.kp = 16.0
        motor_parameters[1][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[1][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[1][trossen_arm.Mode.position].position.kp = 16.0
        motor_parameters[2][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[2][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[2][trossen_arm.Mode.position].position.kp = 16.0
        motor_parameters[3][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[3][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[4][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[4][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[5][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[5][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[6][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[6][trossen_arm.Mode.position].velocity.ki = 0.0
        driver_right_follower.set_motor_parameters(motor_parameters)

    if ENABLE_LEFT and ENABLE_FOLLOWER:
        motor_parameters = driver_left_follower.get_motor_parameters()
        motor_parameters[0][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[0][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[0][trossen_arm.Mode.position].position.kp = 16.0
        motor_parameters[1][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[1][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[1][trossen_arm.Mode.position].position.kp = 16.0
        motor_parameters[2][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[2][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[2][trossen_arm.Mode.position].position.kp = 16.0
        motor_parameters[3][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[3][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[4][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[4][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[5][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[5][trossen_arm.Mode.position].velocity.ki = 0.0
        motor_parameters[6][trossen_arm.Mode.position].velocity.imax = 0.0
        motor_parameters[6][trossen_arm.Mode.position].velocity.ki = 0.0
        driver_left_follower.set_motor_parameters(motor_parameters)

    if ENABLE_RIGHT:
        driver_right_leader.set_all_modes(trossen_arm.Mode.external_effort)
        driver_right_leader.set_gripper_mode(trossen_arm.Mode.external_effort)

        if ENABLE_FOLLOWER:
            driver_right_follower.set_all_modes(trossen_arm.Mode.position)
            driver_right_follower.set_gripper_mode(trossen_arm.Mode.position)

    if ENABLE_LEFT:
        driver_left_leader.set_all_modes(trossen_arm.Mode.external_effort)
        driver_left_leader.set_gripper_mode(trossen_arm.Mode.external_effort)

        if ENABLE_FOLLOWER:
            driver_left_follower.set_all_modes(trossen_arm.Mode.position)
            driver_left_follower.set_gripper_mode(trossen_arm.Mode.position)

    print("Moving to home positions...")

    if ENABLE_BASE:
        if not base.wait_until_ready():
            raise RuntimeError("Base failed to become ready")


    # Follower gripper is offset by -pi/4 to match the leader's gripper position at home
    if ENABLE_RIGHT and ENABLE_FOLLOWER:
        driver_right_follower.set_all_positions(
            np.array([0.0, 0.0, 0.0, 0.0, 0.0, np.pi / 4, 0.0]), 1.0, True
        )

    if ENABLE_LEFT and ENABLE_FOLLOWER:
        driver_left_follower.set_all_positions(
            np.array([0.0, 0.0, 0.0, 0.0, 0.0, -np.pi / 4, 0.0]), 1.0, True
        )

    print("Starting to teleoperate the robots...")
    time.sleep(1)

    try:
        base_velocity_linear_x = 0.0
        base_velocity_linear_y = 0.0
        base_velocity_angular_z = 0.0
        base_velocity_lift = 0  # MUST BE INT
        while True:



            ################################### LEADER COMMANDS ####################################
            if ENABLE_RIGHT:
                right_positions = driver_right_leader.get_all_positions()
                right_input = driver_right_leader.get_input_report()
                if ENABLE_FOLLOWER:
                    right_efforts = driver_right_follower.get_all_external_efforts()
                # Scale to -1 to 1 velocity (rad/s)
                base_velocity_angular_z = scale(right_input.joystick_x, MIN_JOYSTICK, MAX_JOYSTICK,
                                BASE_MIN, BASE_MAX)

                # Button bitmap: bit n is SEL_(n+1), 1 is pressed:
                #   [SEL_1, SEL_2, SEL_3, SEL_4, SEL_5]
                left_up_btn = int(right_input.buttons & (1 << 0))
                left_down_btn = int(right_input.buttons & (1 << 2))
                base_velocity_lift = int(left_up_btn - left_down_btn) * BASE_LIFT_MAX

                # TODO: @schromya Move to other controller when available
                # TODO: See why negative needed
                base_velocity_linear_y = -scale(right_input.joystick_y, MIN_JOYSTICK, MAX_JOYSTICK,
                                BASE_MIN, BASE_MAX)


            if ENABLE_LEFT:
                left_positions = driver_left_leader.get_all_positions()
                left_input = driver_left_leader.get_input_report()

                if ENABLE_FOLLOWER:
                    left_efforts = driver_left_follower.get_all_external_efforts()

                # Scale to -1 to 1 velocity (m/s)
                base_velocity_linear_x = scale(left_input.joystick_x, MIN_JOYSTICK, MAX_JOYSTICK,
                                               BASE_MIN, BASE_MAX)
                # base_velocity_linear_y = scale(left_input.joystick_y, MIN_JOYSTICK, MAX_JOYSTICK,
                #                                BASE_MIN, BASE_MAX)



            ###################################### FOLLOWERS #######################################
            if ENABLE_BASE:
                base.update_base()
                base.set_cmd_vels(base_velocity_linear_x, base_velocity_linear_y,
                                         base_velocity_angular_z)
                base.set_actuator_velocity(base_velocity_lift)

            # RIGHT ARM
            if ENABLE_RIGHT and ENABLE_FOLLOWER:
                # Feed the external efforts from the follower robot to the leader robot
                driver_right_leader.set_arm_external_efforts(
                    np.array(
                        [
                            right_efforts[0],
                            right_efforts[1],
                            right_efforts[2],
                            -right_efforts[3],
                            -right_efforts[4],
                            right_efforts[5],
                        ]
                    ),
                    0.0,
                    False,
                )

                # Feed the positions from the leader robot to the follower robot
                driver_right_follower.set_arm_positions(
                    np.array(
                        [
                            right_positions[0],
                            right_positions[1],
                            right_positions[2],
                            -right_positions[3],
                            -right_positions[4],
                            right_positions[5] + np.pi / 4.0,
                        ]
                    ),
                    0.0,
                    False,
                )

                # RIGHT ARM GRIPPER
                # Read the leader's gripper position and follower's gripper effort
                leader_right_position = driver_right_leader.get_gripper_position()


                follower_right_effort = driver_right_follower.get_gripper_effort()

                # Cubic curve for more resistance at higher efforts and less at lower efforts, with an offset to keep the gripper open when not gripping
                effort_right_norm = min(abs(follower_right_effort) / FOLLOWER_MAX, 1.0)
                leader_right_effort = LEADER_MAX * (effort_right_norm**3) + LEADER_OFFSET

                # Set the leader's gripper effort based on the follower's gripper effort
                driver_right_leader.set_gripper_external_effort(
                    leader_right_effort, 0.2, False
                )


                # Set the follower's gripper position to match the leader's position
                driver_right_follower.set_gripper_position(
                    leader_right_position, 0.0, False
                )

                # Read the follower's gripper position for logging
                follower_right_position = driver_right_follower.get_gripper_position()

            # LEFT ARM
            if ENABLE_LEFT and ENABLE_FOLLOWER:
                # Feed the external efforts from the follower robot to the leader robot
                driver_left_leader.set_arm_external_efforts(
                    np.array(
                        [
                            left_efforts[0],
                            left_efforts[1],
                            left_efforts[2],
                            -left_efforts[3],
                            -left_efforts[4],
                            left_efforts[5],
                        ]
                    ),
                    0.0,
                    False,
                )


                # Feed the positions from the leader robot to the follower robot
                driver_left_follower.set_arm_positions(
                    np.array(
                        [
                            left_positions[0],
                            left_positions[1],
                            left_positions[2],
                            -left_positions[3],
                            -left_positions[4],
                            left_positions[5] - np.pi / 4.0,
                        ]
                    ),
                    0.0,
                    False,
                )

                # LEFT ARM GRIPPER
                # Read the leader's gripper position and follower's gripper effort
                leader_left_position = driver_left_leader.get_gripper_position()
                follower_left_effort = driver_left_follower.get_gripper_effort()

                # Cubic curve for more resistance at higher efforts and less at lower efforts, with an offset to keep the gripper open when not gripping
                effort_left_norm = min(abs(follower_left_effort) / FOLLOWER_MAX, 1.0)
                leader_left_effort = LEADER_MAX * (effort_left_norm**3) + LEADER_OFFSET

                # Set the leader's gripper effort based on the follower's gripper effort
                driver_left_leader.set_gripper_external_effort(
                    leader_left_effort, 0.2, False
                )

                # Set the follower's gripper position to match the leader's position
                driver_left_follower.set_gripper_position(
                    leader_left_position + 0.002, 0.0, False
                )

                # Read the follower's gripper position for logging
                follower_left_position = driver_left_follower.get_gripper_position()

    except KeyboardInterrupt:
        print("Moving to stop positions...")
        if ENABLE_RIGHT and ENABLE_FOLLOWER:
            driver_right_follower.set_all_modes(trossen_arm.Mode.position)
            driver_right_follower.set_all_positions(
                np.zeros(driver_right_follower.get_num_joints()), 2.0, True
            )

        if ENABLE_LEFT and ENABLE_FOLLOWER:
            driver_left_follower.set_all_modes(trossen_arm.Mode.position)
            driver_left_follower.set_all_positions(
                np.zeros(driver_left_follower.get_num_joints()), 2.0, True
            )
        if ENABLE_BASE:
            base.set_cmd_vels(0.0, 0.0, 0.0)
            base.set_actuator_velocity(0)
