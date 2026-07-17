/**
 * Animates the solo robot in Foxglove from a trossen_sdk recording.
 *
 * Publishes transforms on /tf: forward kinematics for every URDF joint
 * (driven by follower/joints/state) plus a follower_eef frame for the
 * recorded end-effector pose.
 *
 * Setup:
 *   1. Add this script and robot_description_bridge.ts under User Scripts.
 *   2. In a 3D panel, set the display frame to "base_link" and enable /tf.
 *   3. Add a URDF layer: Source: Topic -> /robot_description_string.
 *   4. Enable the follower_eef frame under Transforms to see the pose axes.
 *   5. Play from the start of the file.
 */

import { Message } from "./types";

type FrameTransforms = Message<"foxglove.FrameTransforms">;
type FrameTransform = FrameTransforms["transforms"][number];
type Time = FrameTransform["timestamp"];
type Quat = { x: number; y: number; z: number; w: number };

export const inputs = [
  "/robot_description",
  "follower/joints/state",
  "follower/end_effector/pose",
];
export const output = "/tf";

interface Joint {
  type: string;
  parent: string;
  child: string;
  xyz: [number, number, number];
  rpy: [number, number, number];
  axis: [number, number, number];
}

let joints: Joint[] | undefined;

function quatMultiply(a: Quat, b: Quat): Quat {
  return {
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

// URDF rpy is fixed-axis roll/pitch/yaw in radians: R = Rz(yaw) Ry(pitch) Rx(roll).
function rpyToQuat(roll: number, pitch: number, yaw: number): Quat {
  const cr = Math.cos(roll / 2), sr = Math.sin(roll / 2);
  const cp = Math.cos(pitch / 2), sp = Math.sin(pitch / 2);
  const cy = Math.cos(yaw / 2), sy = Math.sin(yaw / 2);
  return {
    x: sr * cp * cy - cr * sp * sy,
    y: cr * sp * cy + sr * cp * sy,
    z: cr * cp * sy - sr * sp * cy,
    w: cr * cp * cy + sr * sp * sy,
  };
}

// Rotation about an axis. Without an explicit angle, the axis magnitude is
// the angle (axis-angle vector, as used by the end-effector pose record).
function axisAngleToQuat(x: number, y: number, z: number, angle?: number): Quat {
  const norm = Math.sqrt(x * x + y * y + z * z);
  const theta = angle ?? norm;
  if (Math.abs(theta) < 1e-12 || norm < 1e-12) {
    return { x: 0, y: 0, z: 0, w: 1 };
  }
  const s = Math.sin(theta / 2) / norm;
  return { x: x * s, y: y * s, z: z * s, w: Math.cos(theta / 2) };
}

function parseTriple(value: string | undefined): [number, number, number] {
  const p = (value ?? "").trim().split(/\s+/).map(Number);
  return [p[0] ?? 0, p[1] ?? 0, p[2] ?? 0];
}

function attr(block: string, tag: string, attribute: string): string | undefined {
  return block.match(new RegExp(`<${tag}\\b[^>]*\\b${attribute}="([^"]*)"`))?.[1];
}

function parseJoints(urdf: string): Joint[] {
  const result: Joint[] = [];
  for (const m of urdf.matchAll(/<joint\b[^>]*\btype="([^"]+)"[^>]*>([\s\S]*?)<\/joint>/g)) {
    const body = m[2]!;
    result.push({
      type: m[1]!,
      parent: attr(body, "parent", "link") ?? "",
      child: attr(body, "child", "link") ?? "",
      xyz: parseTriple(attr(body, "origin", "xyz")),
      rpy: parseTriple(attr(body, "origin", "rpy")),
      axis: parseTriple(attr(body, "axis", "xyz") ?? "1 0 0"),
    });
  }
  return result;
}

function robotTransforms(positions: ArrayLike<number>, timestamp: Time): FrameTransform[] {
  const gripperHalf = (positions[positions.length - 1] ?? 0) / 2;
  let revolute = 0;

  return (joints ?? []).map((joint) => {
    let translation = { x: joint.xyz[0], y: joint.xyz[1], z: joint.xyz[2] };
    let rotation = rpyToQuat(...joint.rpy);

    if (joint.type === "revolute") {
      rotation = quatMultiply(rotation, axisAngleToQuat(...joint.axis, positions[revolute++] ?? 0));
    } else if (joint.type === "prismatic") {
      translation = {
        x: translation.x + joint.axis[0] * gripperHalf,
        y: translation.y + joint.axis[1] * gripperHalf,
        z: translation.z + joint.axis[2] * gripperHalf,
      };
    }

    return { timestamp, parent_frame_id: joint.parent, child_frame_id: joint.child, translation, rotation };
  });
}

interface ScriptEvent {
  topic: string;
  receiveTime: Time;
  message: unknown;
}

export default function script(event: ScriptEvent): FrameTransforms | undefined {
  if (event.topic === "/robot_description") {
    const msg = event.message as { robot_description: string };
    joints = parseJoints(msg.robot_description);
    return { transforms: robotTransforms([], event.receiveTime) };
  }
  if (joints === undefined) {
    return undefined;
  }

  if (event.topic === "follower/joints/state") {
    const msg = event.message as { positions: ArrayLike<number> };
    return { transforms: robotTransforms(msg.positions, event.receiveTime) };
  }

  const msg = event.message as {
    x: number; y: number; z: number;
    rotation_x: number; rotation_y: number; rotation_z: number;
  };
  return {
    transforms: [{
      timestamp: event.receiveTime,
      parent_frame_id: "base_link",
      child_frame_id: "follower_eef",
      translation: { x: msg.x, y: msg.y, z: msg.z },
      rotation: axisAngleToQuat(msg.rotation_x, msg.rotation_y, msg.rotation_z),
    }],
  };
}
