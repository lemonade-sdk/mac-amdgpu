#!/usr/bin/env python3
"""Run the bounded HRX transfer and latency fixtures with checked retirement."""
import argparse
import csv
import datetime
import json
import platform
import signal
import subprocess
import time

from common import REPO, add_paths, digest, loaded_libraries, runtime_environment, stats


def capture_environment():
    def read(command):
        return subprocess.check_output(command, text=True).strip()

    cpu = read(["sysctl", "-n", "machdep.cpu.brand_string"])
    model = read(["sysctl", "-n", "hw.model"])
    return {"captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "host": f"{model}, {cpu}", "memory_gb": int(read(["sysctl", "-n", "hw.memsize"])) / (1 << 30),
            "cpu_cores": int(read(["sysctl", "-n", "hw.ncpu"])), "architecture": platform.machine(),
            "os": f"macOS {read(['sw_vers', '-productVersion'])} ({read(['sw_vers', '-buildVersion'])})",
            "mac_amdgpu_commit": read(["git", "-C", str(REPO), "rev-parse", "HEAD"]),
            "workload_policy": "Run with exclusive GPU ownership; other workloads must be stopped by the caller."}


def bounded_run(binary, folder, logfile, env, timeout):
    start_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
    start = time.perf_counter()
    with logfile.open("w") as log:
        process = subprocess.Popen([str(binary), "--run", str(folder)], cwd=REPO, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise RuntimeError(f"{binary.name} exceeded its deadline; GPU retirement unconfirmed. Inspect {logfile}")
    info = {"start_utc": start_utc, "elapsed_seconds": time.perf_counter()-start,
            "exit_code": code, "outer_deadline_seconds": timeout, "binary_sha256": digest(binary)}
    if code:
        raise RuntimeError(f"{binary.name} exited {code}; inspect {logfile}")
    return info


def read_csv(folder, name):
    with (folder / name).open() as stream:
        return list(csv.DictReader(stream))


def summarize_disjoint(folder):
    log = (folder / "bandwidth-disjoint.log").read_text()
    if "PASS disjoint-region transfer check; buffers/queue retired and released" not in log:
        raise ValueError("Distinct-region transfer retirement/validation has not passed")
    rows = read_csv(folder, "bandwidth-disjoint.csv")
    if len(rows) != 6 or any(row["validation"] != "verified" for row in rows):
        raise ValueError("Distinct-region transfer results are incomplete")
    first = rows[0]
    result = {"region_bytes": int(first["region_bytes"]), "regions": int(first["regions"]),
              "payload_working_set_bytes": int(first["payload_working_set_bytes"]),
              "allocation_api": "hrx_allocator_allocate_buffer; direct allocations outside timing",
              "host_endpoint_count": 1, "device_endpoint_count": 2,
              "host_buffers": "Mapped host-visible/coherent/device-visible coarse GTT memory",
              "device_buffers": "Requested DEVICE_LOCAL VRAM", "warmups_per_direction": 1,
              "trials_per_direction": 3, "guard_bytes_each_side": 4096,
              "allocation_checks": "Returned HRX capacity checked; host HSA metadata CPU/GPU bases identical and coarse-grained",
              "readback_policy": "H2D host source verified before reuse for readback; D2H host destination and VRAM source checked",
              "validation": "Full payloads/guards/source preservation each trial; clean retirement and shutdown",
              "directions": {}}
    for direction in ("H2D", "D2H"):
        values = [float(row["GBps"]) for row in rows if row["direction"] == direction]
        if len(values) != 3:
            raise ValueError(f"Incomplete {direction} distinct-region trials")
        result["directions"][direction] = {"median_GBps": stats(values)["median"],
            "min_GBps": min(values), "max_GBps": max(values), "trial_GBps": values, "n": len(values)}
    (folder / "bandwidth-disjoint.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def summarize(folder):
    result = {"schema_version": 1, "loaded_libraries": loaded_libraries(folder),
              "compile_cases": [], "bandwidth_summary": [], "latency_summary": []}
    for name, key in (("environment.json", "environment"), ("gpu.json", "gpu"),
                      ("run-metadata.json", "run"), ("wait-policy.json", "wait_policy")):
        if (folder / name).is_file():
            result[key] = json.loads((folder / name).read_text())
    if (folder / "compile.csv").is_file():
        metadata = json.loads((folder / "build-metadata.json").read_text())
        selection = metadata.get("compile_selection", "LSE_HRX_INT8=1 (historical fixture)")
        rows = read_csv(folder, "compile.csv")
        for name in sorted({row["case"] for row in rows}):
            selected = [row for row in rows if row["case"] == name]
            first = next(row for row in selected if row["kind"] == "first_call")
            warm = [float(row["compile_ms"]) for row in selected if row["kind"] == "warm_uncached"]
            source, code = folder / (name + ".loom"), folder / (name + ".hsaco")
            result["compile_cases"].append({"case": name, "source_lines": len(source.read_text().splitlines()),
                "nonblank_lines": sum(bool(line.strip()) for line in source.read_text().splitlines()),
                "source_bytes": source.stat().st_size, "code_bytes": code.stat().st_size,
                "source_sha256": digest(source), "code_sha256": digest(code),
                "first_call_ms": float(first["compile_ms"]), "warm_uncached_ms": stats(warm),
                "grid_x": int(first["grid_x"]), "wg_x": int(first["wg_x"]), "selection": selection})
    if (folder / "latency.csv").is_file():
        if "PASS four-category suite; all queues/buffers/executables retired and released" not in (folder / "native.log").read_text():
            raise ValueError("Native latency suite retirement/validation has not passed")
        rows = read_csv(folder, "latency.csv")
        if len(rows) != 8056:
            raise ValueError(f"Expected 8,056 latency samples; got {len(rows)}")
        for name in sorted({row["test"] for row in rows}):
            selected = [row for row in rows if row["test"] == name]
            clocks = {row["clock"] for row in selected}
            if len(clocks) != 1:
                raise ValueError(f"Mixed clock domains for {name}")
            result["latency_summary"].append({"test": name, "clock": clocks.pop(),
                "ns": stats([float(row["ns"]) for row in selected]),
                "trial_summaries": {trial: stats([float(row["ns"]) for row in selected if row["trial"] == trial])
                    for trial in sorted({row["trial"] for row in selected})}})
        bands = read_csv(folder, "bandwidth.csv")
        if len(bands) != 18 or any(row["validation"] != "verified" for row in bands):
            raise ValueError("Repeated-region sweep is incomplete")
        for direction in ("H2D", "D2H"):
            for size in sorted({int(row["payload_bytes"]) for row in bands}):
                values = [float(row["GBps"]) for row in bands if row["direction"] == direction and int(row["payload_bytes"]) == size]
                result["bandwidth_summary"].append({"direction": direction, "payload_bytes": size,
                    "copies_per_batch": 8, "GBps": stats(values)})
    if (folder / "bandwidth-disjoint.csv").is_file():
        result["distinct_region_bandwidth"] = summarize_disjoint(folder)
    if not result["compile_cases"] and not result["latency_summary"] and "distinct_region_bandwidth" not in result:
        raise ValueError("No completed benchmark data exists in the artifact directory")
    result["methodology"] = {
        "compile": "One first invocation per compiler instance followed by seven real uncached compile(source) calls. M512 follows M1 in the same process; no model load or GPU work.",
        "bandwidth": "Eight distinct12MiB regions per endpoint;1warmup+3measured batches per direction. Host timing includes recording, flush and completion; seeding/allocation/full validation excluded. DecimalGB/s. Repeated-region sweep is diagnostic and can be cache-sensitive.",
        "kernel_latency": "32warmup+200singleWG32 dispatches; checked4-byte store. HRX recording+flush issue and timeline completion use host clock. BareHSA active wait and separate paired CP GPU intervals. No cross-domain subtraction.",
        "transfer_latency": "32warmup+256individual8-byte copies per direction, through record/flush/completion; API-to-completion, not pure one-way link latency.",
        "pure_latency": "Persistent single-lane mailbox with8-byte request/reply, release/acquire ownership and no mixed RMW;128warmup+1024round trips×3trials per initiator. Host clock for CPU→GPU→CPU, GPU realtime clock for GPU→CPU→GPU. Poll and responder turnaround included. No exact one-way times or RTT/2 estimate.",
        "statistics": "Minimum/median and linearly interpolated P95/P99 at position(n-1)*p; pooled mailbox samples with per-trial summaries."}
    (folder / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main(*, bandwidth_only_default=False):
    parser = argparse.ArgumentParser(description=__doc__)
    add_paths(parser)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--run", action="store_true", help="Explicitly submit live GPU workloads.")
    action.add_argument("--summarize-only", action="store_true", help="Summarize existing checked artifacts without GPU work.")
    parser.add_argument("--bandwidth-only", action="store_true", default=bandwidth_only_default,
                        help="Run only the distinct-region bandwidth check.")
    parser.add_argument("--timeout-seconds", type=int, default=180,
                        help="Per-process outer deadline,10..600 seconds (default 180).")
    args = parser.parse_args()
    folder, runtime = args.output_dir.resolve(), args.runtime_dir.resolve()
    if not 10 <= args.timeout_seconds <= 600:
        parser.error("--timeout-seconds must be 10..600")
    if args.run:
        required = [folder / "bandwidth-disjoint", runtime / "libhsa-runtime64.dylib"]
        if not args.bandwidth_only:
            required += [folder / "native-bench", folder / "latency.hsaco"]
        for path in required:
            if not path.is_file():
                parser.error(f"Missing prerequisite: {path}; run build.py first")
        env = runtime_environment(runtime, trace=True)
        (folder / "environment.json").write_text(json.dumps(capture_environment(), indent=2) + "\n")
        setting = env.get("MAC_HSA_BLOCKED_POLL_US")
        effective = int(setting) if setting and setting.isascii() and setting.isdigit() and 10 <= int(setting) <= 1000 else 1000
        policy = {"MAC_HSA_BLOCKED_POLL_US": setting, "effective_blocked_poll_us": effective,
                  "HRX_completion_wait": "Timeline wait with flags0; HSA_WAIT_STATE_BLOCKED on epoch path",
                  "HSA_completion_wait": "Active hsa_signal_load_scacquire loop",
                  "pure_mailbox_wait": "Active acquire polling, no blocked wait"}
        (folder / "wait-policy.json").write_text(json.dumps(policy, indent=2) + "\n")
        runs = {}
        if not args.bandwidth_only:
            runs["native"] = bounded_run(folder / "native-bench", folder, folder / "native.log", env, args.timeout_seconds)
        runs["bandwidth_disjoint"] = bounded_run(folder / "bandwidth-disjoint", folder,
                                                 folder / "bandwidth-disjoint.log", env, args.timeout_seconds)
        (folder / "run-metadata.json").write_text(json.dumps(runs, indent=2) + "\n")
    result = summarize(folder)
    print(f"Checked results: {folder / 'results.json'}")
    if "distinct_region_bandwidth" in result:
        for direction, row in result["distinct_region_bandwidth"]["directions"].items():
            print(f"{direction}: {row['median_GBps']:.6f} GB/s median")


if __name__ == "__main__":
    main()
