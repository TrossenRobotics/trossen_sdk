import type { Route } from "./+types/dashboard";
import { Dashboard } from "../Dashboard";

export function meta({}: Route.MetaArgs) {
  return [
    { title: "Dashboard - SOMA" },
    { name: "description", content: "Robot data collection dashboard" },
  ];
}

export default function DashboardRoute() {
  return <Dashboard />;
}
