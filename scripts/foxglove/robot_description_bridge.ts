/**
 * Republishes the recorded URDF as a std_msgs/String so Foxglove's URDF
 * layer can use it.
 *
 * In a 3D panel: Custom Layers -> Add URDF -> Source: Topic
 * -> /robot_description_string.
 */

import { Input, Message } from "./types";

type StdString = Message<"std_msgs/String">;

export const inputs = ["/robot_description"];
export const output = "/robot_description_string";

export default function script(event: Input<"/robot_description">): StdString | undefined {
  const urdf = event.message.robot_description;
  return urdf ? { data: urdf } : undefined;
}
