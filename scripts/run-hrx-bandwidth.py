#!/usr/bin/env python3
"""Run the explicit HRX bandwidth workload with a process deadline and log."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, default=Path("build/tests/hrx-bandwidth.log"))
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--hsa-library-dir", type=Path, help="HSA runtime directory (default: build/hsa)")
    args = parser.parse_args()
    if not 10 <= args.timeout_seconds <= 600:
        parser.error("--timeout-seconds must be 10..600")
    root = Path(__file__).resolve().parent.parent
    binary = root / "build/hrx-macos-adapter/mac-hrx-bandwidth"
    if not binary.is_file():
        parser.error("run bash scripts/build-hrx-bandwidth.sh first")
    log_path = args.log if args.log.is_absolute() else root / args.log
    log_path.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    runtime_dir = (args.hsa_library_dir or root / "build/hsa").resolve()
    runtime = runtime_dir / "libhsa-runtime64.dylib"
    if not runtime.is_file():
        parser.error("HSA runtime library does not exist")
    env["DYLD_LIBRARY_PATH"] = str(runtime_dir)
    print(f"Running explicit GPU bandwidth workload; log: {log_path}", flush=True)
    with log_path.open("w") as log:
        log.write(json.dumps({"binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                              "hsa_library": str(runtime), "hsa_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
                              "blocked_poll_us": env.get("MAC_HSA_BLOCKED_POLL_US", "default")}) + "\n")
        log.flush()
        process = subprocess.Popen([str(binary), "--run"], cwd=root, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=args.timeout_seconds)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            print("Stopped the process; GPU retirement is unconfirmed. "
                  "Inspect driver state before another workload.", file=sys.stderr)
            return 124
    print(f"Bandwidth process exit: {code}")
    return code if code >= 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
