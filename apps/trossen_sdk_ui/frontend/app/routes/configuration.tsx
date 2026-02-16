import type { Route } from "./+types/configuration";
import { Configuration } from "../Configuration";

export function meta({}: Route.MetaArgs) {
  return [
    { title: "Configuration - SOMA" },
    { name: "description", content: "Configure hardware and systems" },
  ];
}

export default function ConfigurationRoute() {
  return <Configuration />;
}
