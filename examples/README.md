# Trossen SDK Examples

Each binary is built into `build/examples/`. Run it from the repository root, since the default config paths are relative to it.

| Example | Binary | What it shows |
|---|---|---|
| [`trossen_solo_ai/`](trossen_solo_ai/README.md) | `trossen_solo_ai` | Recording a leader + follower arm pair and 2 cameras |
| [`trossen_stationary_ai/`](trossen_stationary_ai/README.md) | `trossen_stationary_ai` | Recording a bimanual setup with 4 cameras |
| [`trossen_mobile_ai/`](trossen_mobile_ai/README.md) | `trossen_mobile_ai` | Recording a bimanual setup on a SLATE mobile base with 3 cameras |
| [`trossen_stationary_ai_policy/`](trossen_stationary_ai_policy/README.md) | `trossen_stationary_ai_openpi`, `trossen_stationary_ai_lerobot` | Driving the followers from a policy server; built with `-DTROSSEN_SDK_ENABLE_POLICY_CLIENT=ON` |
| `callback_demo/` | `callback_demo` | When each SessionManager lifecycle callback fires |
| `backend_registry_demo.cpp` | `backend_registry_demo` | The BackendRegistry |
| `hardware_registry_demo.cpp` | `hardware_registry_demo` | The HardwareRegistry and ActiveHardwareRegistry |
| `producer_registry_demo.cpp` | `producer_registry_demo` | The ProducerRegistry, with hardware and mock producers |
