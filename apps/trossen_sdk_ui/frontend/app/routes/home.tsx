import { redirect } from "react-router";
import type { Route } from "./+types/home";

export function loader({}: Route.LoaderArgs) {
  return redirect("/dashboard");
}
