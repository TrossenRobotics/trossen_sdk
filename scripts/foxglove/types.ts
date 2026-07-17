/**
 * Minimal type shims for the Foxglove user scripts in this directory.
 *
 * Foxglove Studio generates an equivalent "./types" module at runtime from the
 * connected data source; this file lets the scripts resolve their imports and
 * typecheck standalone in the repo. Extend MessageShapes when a script needs a
 * new schema or input topic.
 */

export interface Time {
  sec: number;
  nsec: number;
}

export interface Vector3 {
  x: number;
  y: number;
  z: number;
}

export interface Quaternion {
  x: number;
  y: number;
  z: number;
  w: number;
}

export interface FrameTransform {
  timestamp: Time;
  parent_frame_id: string;
  child_frame_id: string;
  translation: Vector3;
  rotation: Quaternion;
}

/**
 * Shapes keyed by the schema/topic name used with Message<> and Input<>.
 * Keys are the string literals the scripts pass (e.g. "std_msgs/String").
 */
export interface MessageShapes {
  "std_msgs/String": { data: string };
  "foxglove.FrameTransforms": { transforms: FrameTransform[] };
  "/robot_description": { robot_description: string };
}

/** Message shape for a known schema name. */
export type Message<T extends keyof MessageShapes> = MessageShapes[T];

/** A single input event delivered to a script for the given topic. */
export interface Input<T extends string> {
  topic: T;
  receiveTime: Time;
  message: T extends keyof MessageShapes ? MessageShapes[T] : unknown;
}
