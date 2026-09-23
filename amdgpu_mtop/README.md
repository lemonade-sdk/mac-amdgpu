# amdgpu_mtop

A native macOS terminal monitor for GPUs bound to MacAMDGPU. The dashboard uses
the familiar device list, GPU activity, memory, clock and sensor groups from
[amdgpu_top](https://github.com/Umio-Yasuno/amdgpu_top), with a new IOKit backend.
It does not depend on Linux DRM, sysfs, Rust, HSA, or a graphics runtime.

The monitor reads the responding driver build, initialization stage and VRAM
capacities. It also supports the build-176 cached metrics endpoint for
GPU/UMC activity, clocks, power, temperatures and fan speed. The host app’s Sample Metrics button requests one firmware snapshot; live
firmware validation is still pending. Automatic collection is not enabled. Older drivers, absent samples,
failed requests and stale samples are explicitly **unavailable**; it does not
show fabricated zero load, temperature, power or memory use.

The tested R9700 currently reports SMU interface **0x33**; the independently
verified decoder targets **0x2e**. Its statistics remain unavailable pending
layout verification. The monitor names this compatibility mismatch explicitly
and includes both interface versions in JSON; it does not treat it as proof of
a firmware crash.

```sh
cmake -S amdgpu_mtop -B build/amdgpu_mtop -DCMAKE_BUILD_TYPE=Release
cmake --build build/amdgpu_mtop --parallel 4
build/amdgpu_mtop/amdgpu_mtop
```

In a terminal, refresh is once per second. Press `n` or `p` to switch GPUs and
`q` to quit. `Ctrl-C` also restores normal terminal input.

```sh
build/amdgpu_mtop/amdgpu_mtop --list
build/amdgpu_mtop/amdgpu_mtop --device 0x10033939d
build/amdgpu_mtop/amdgpu_mtop --json
build/amdgpu_mtop/amdgpu_mtop --json --watch
```

Use a registry ID from `--list`; the example ID is not a fixed GPU identifier.
Selection follows the registry entry rather than the enumeration index. Removal
leaves that selection disconnected until the user chooses another card. A
replugged device receives a new registry ID. All matching devices appear in JSON;
`selected_registry_id` identifies the dashboard selection.

Non-terminal output and `--json` default to a single snapshot. `--watch` emits
one snapshot per second. JSON uses `null` for unsupported statistics, byte units
for capacities, and explicit unit suffixes for dynamic statistics.
Zero-sized capacities before initialization are also shown as unavailable.

The tool requires driver build 172 or newer and permission to open its user
client. It only invokes observer selectors 43 (runtime build), 21 (cached
information), and 47 (cached metrics, build 176+). It never initializes the GPU,
claims the hardware session, sends SMU messages, changes power policy, submits
work, or resets the device. Closing
its observer connection does not stop another client's session. Cards bound to
Apple’s driver, or not bound to any driver, are outside this backend's coverage.

Offline checks:

```sh
bash scripts/test-amdgpu-mtop.sh
bash scripts/test-metrics.sh
```

The decoder test also requires this repository's local `upstream/linux` reference
tree. Integration requirements and field semantics are in
[GPU_MONITOR.md](../docs/GPU_MONITOR.md).
