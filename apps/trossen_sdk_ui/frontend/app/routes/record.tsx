import type { Route } from "./+types/record";
import { RecordPage } from "../RecordPage";

export function meta({}: Route.MetaArgs) {
  return [
    { title: "Record - SOMA" },
    { name: "description", content: "Manage recording sessions" },
  ];
}

export default function RecordRoute() {
  return <RecordPage />;
}
