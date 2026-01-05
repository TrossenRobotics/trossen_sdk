import { type RouteConfig, index, layout, route } from "@react-router/dev/routes";

export default [
  index("routes/home.tsx"),
  layout("routes/_layout.tsx", [
    route("dashboard", "routes/dashboard.tsx"),
    route("configuration", "routes/configuration.tsx"),
    route("record", "routes/record.tsx"),
  ]),
] satisfies RouteConfig;
