#!/usr/bin/env python3
"""Run the staged driver/HRX candidate checks, with native contention last."""
import argparse
import datetime
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--run", action="store_true", help="Initialize and submit GPU work; otherwise list the checks only")
parser.add_argument("--skip-atomics", action="store_true", help="Omit the separate native CPU/GPU atomic experiment")
args = parser.parse_args()
steps = [
    ("topology-clock", ["build/hsa/mac-hsa-info", "--hardware-properties"], 60),
    ("signal-api", ["build/hsa/mac-hsa-signal-api-test", "--run"], 60),
    ("public-pools-queues", ["build/hsa/mac-hsa-queue-test", "--run", "build/tests/hsa-code-object.hsaco"], 90),
    ("scratch-lds", ["build/hsa/mac-hsa-resource-test", "--run", "build/tests/hsa-resource-object.hsaco"], 90),
    ("concurrent-signals", ["build/hsa/mac-hsa-queue-signal-test", "--run", "build/tests/hsa-signal-object.hsaco"], 90),
    ("two-processes", ["python3", "scripts/test-hsa-multi-process.py"], 120),
    ("real-hrx", ["build/hrx-macos-adapter/mac-hrx-smoke", "--run"], 120),
]
if not args.skip_atomics:
    steps.append(("native-atomics", ["build/hsa/mac-hsa-atomic-contention-test", "--run",
                  "build/tests/hsa-atomic-contention.hsaco"], 700))
for name, command, timeout in steps:
    print(f"{name}: {' '.join(command)} (outer timeout {timeout}s)", flush=True)
if not args.run:
    print("No GPU work submitted. Use --run after installing driver 190 or newer.")
    raise SystemExit(0)
for _, command, _ in steps:
    for item in command:
        if item.startswith(("build/", "scripts/")) and not (root / item).is_file():
            parser.error(f"Missing prerequisite: {item}; build all candidate tools first")
environment = dict(os.environ)
environment["DYLD_LIBRARY_PATH"] = str(root / "build/hsa") + (
    ":" + environment["DYLD_LIBRARY_PATH"] if environment.get("DYLD_LIBRARY_PATH") else "")
probe = subprocess.run([str(root / "build/hsa/mac-hsa-info")], cwd=root,
                       capture_output=True, text=True, timeout=30, env=environment)
print(probe.stdout, end="", flush=True)
builds = re.findall(r"driver=(\d+)", probe.stdout)
if probe.returncode or len(builds) != 1 or int(builds[0]) < 190:
    parser.error("This HRX adapter test requires exactly one responding GPU with driver 190 or newer")
logs = root / "build/tests" / ("hrx-hardware-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
logs.mkdir(parents=True)
(logs / "driver-info.log").write_text(probe.stdout + probe.stderr)
# Preserve capability evidence beside measured behavior. Missing/unknown cached
# bits are not a reason to skip the explicitly requested contention experiment.
try:
    topology = subprocess.run(["python3", "scripts/check-pcie-atomics.py", "--json"],
                              cwd=root, capture_output=True, text=True, timeout=35)
    (logs / "pcie-atomics.json").write_text(topology.stdout)
    (logs / "pcie-atomics-status.log").write_text(
        f"diagnostic exit={topology.returncode}\n" + topology.stderr)
except subprocess.TimeoutExpired:
    (logs / "pcie-atomics-status.log").write_text("Cached topology read timed out; capability evidence unavailable.\n")
for name, command, timeout in steps:
    log = logs / (name + ".log")
    print(f"START {name}: {log}", flush=True)
    try:
        with log.open("w") as output:
            result = subprocess.run(command, cwd=root, env=environment, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"TIMEOUT {name}; no further GPU work will run. Inspect {log} and verify session recovery.", flush=True)
        raise SystemExit(1)
    print(log.read_text(), end="", flush=True)
    if result.returncode:
        print(f"FAIL {name} (exit {result.returncode}); earlier passes remain recorded in {logs}", flush=True)
        raise SystemExit(1)
    print(f"PASS {name}", flush=True)
print(f"All requested hardware checks passed. Logs: {logs}", flush=True)
print("This suite verifies the tested HRX operations and atomic mappings; it does not establish model inference or full HSA conformance.")
