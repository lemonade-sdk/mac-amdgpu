#!/usr/bin/env python3
"""Render existing HRX results as Markdown and a Discord copy/paste table."""
import argparse
import json
from pathlib import Path

from common import DEFAULT_OUTPUT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT,
                        help="Directory containing results.json and raw artifacts.")
    parser.add_argument("--report-dir", type=Path,
                        help="Report destination; default is the artifact directory.")
    args = parser.parse_args()
    folder = args.output_dir.resolve()
    destination = args.report_dir.resolve() if args.report_dir else folder
    result_path = folder / "results.json"
    if not result_path.is_file():
        parser.error(f"Missing {result_path}; summarize checked results with run.py first")
    result = json.loads(result_path.read_text())
    environment, gpu = result.get("environment", {}), result.get("gpu", {})
    compiler = result.get("compile_cases", [])
    latency = {row["test"]: row for row in result.get("latency_summary", [])}
    bandwidth = result.get("distinct_region_bandwidth")
    if bandwidth is None and (folder / "bandwidth-disjoint.json").is_file():
        bandwidth = json.loads((folder / "bandwidth-disjoint.json").read_text())
    policy = result.get("wait_policy", {})
    if not policy and (folder / "wait-policy.json").is_file():
        policy = json.loads((folder / "wait-policy.json").read_text())
    rows = []
    for case in compiler:
        rows.append((f"Loom compile: {case['case']}", f"{case['warm_uncached_ms']['median']:.3f} ms",
                     f"{case['source_lines']:,} lines / {case['source_bytes']/1024:.2f} KiB"))
    if bandwidth:
        working_mib = bandwidth["payload_working_set_bytes"] / (1 << 20)
        for direction, row in bandwidth["directions"].items():
            rows.append((f"{'Host -> VRAM' if direction == 'H2D' else 'VRAM -> Host'} sustained",
                         f"{row['median_GBps']:.3f} GB/s", f"{working_mib:g} MiB working set"))
    labels = {"hrx_kernel_record": "HRX dispatch recording",
              "hrx_kernel_issue": "HRX recording + flush return",
              "hrx_kernel_completion": "HRX observed completion",
              "hrx_kernel_post_issue_wait": "HRX wait after issue",
              "hsa_kernel_issue": "Bare HSA publication + doorbell",
              "hsa_kernel_completion": "Bare HSA observed completion",
              "hsa_kernel_cp_duration": "Minimal kernel: GPU CP interval",
              "h2d_8byte_completion": "Host -> VRAM: 8 B complete",
              "d2h_8byte_completion": "VRAM -> Host: 8 B complete",
              "cpu_gpu_cpu_mailbox_rtt": "CPU -> GPU -> CPU mailbox RTT",
              "gpu_cpu_gpu_mailbox_rtt": "GPU -> CPU -> GPU mailbox RTT"}
    brief_latency = ("hrx_kernel_issue", "hrx_kernel_completion", "hsa_kernel_completion",
                     "hsa_kernel_cp_duration", "h2d_8byte_completion", "d2h_8byte_completion",
                     "cpu_gpu_cpu_mailbox_rtt", "gpu_cpu_gpu_mailbox_rtt")
    for name in brief_latency:
        if name in latency:
            values = latency[name]["ns"]
            rows.append((labels[name], f"{values['median']/1000:.3f} us", f"P95 {values['p95']/1000:.3f} us"))
    if not rows:
        parser.error("No measured results were found")
    widths = [max(len(row[column]) for row in rows + [("Test", "Median", "Details")]) for column in range(3)]
    def table_line(row):
        return " | ".join(value.ljust(widths[i]) for i, value in enumerate(row)).rstrip()
    title = f"HRX / {gpu.get('gpu', 'GPU')} | {environment.get('host', 'host')}"
    if gpu.get("driver_build"):
        title += f" | driver {gpu['driver_build']}"
    block = [title, table_line(("Test", "Median", "Details")), "-+-".join("-"*width for width in widths)]
    block.extend(table_line(row) for row in rows)
    block.append("Compile: warm uncached. GB/s: decimal. Mailbox: round trips, not one-way.")
    if latency:
        block.append(f"HRX completion includes blocked-poll policy: {policy.get('effective_blocked_poll_us', 'unknown')} us.")
    discord = "```text\n" + "\n".join(block) + "\n```\n"
    if len(discord) > 2000:
        parser.error("Discord table exceeds 2000 characters; shorten the environment host label")
    md = ["# HRX compile, transfer and latency benchmarks", "",
          f"Captured: {environment.get('captured_utc', 'not recorded')}", "",
          f"**System:** {environment.get('host', 'not recorded')}; {environment.get('memory_gb', '?')} GiB; {environment.get('os', 'not recorded')}.", "",
          f"**GPU:** {gpu.get('gpu', 'not recorded')}; driver {gpu.get('driver_build', '?')}; timestamp frequency {gpu.get('timestamp_frequency_hz', '?')} Hz.", "",
          "## Discord copy/paste", "", discord.rstrip(), "", "## 1. Kernel compile time", "",
          "Actual Loom source-to-gfx1201 code-object compilation. A fresh compiler instance measures its first invocation separately, then seven real uncached calls. M512 follows M1 in the same process. Synthetic Q4 fixtures select INT8 explicitly. Model loading, disk-cache lookup and GPU execution are excluded.", "",
          "| Kernel | Lines | Nonblank | Source bytes | Code-object bytes | First call ms | Warm median ms | Warm P95 ms |",
          "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for case in compiler:
        warm = case["warm_uncached_ms"]
        md.append(f"|{case['case']}|{case['source_lines']:,}|{case['nonblank_lines']:,}|{case['source_bytes']:,}|{case['code_bytes']:,}|{case['first_call_ms']:.3f}|{warm['median']:.3f}|{warm['p95']:.3f}|")
    md += ["", "## 2. Host to device and 3. Device to host", "",
           "Headline rates use distinct addresses traversed once per batch, one warmup and three measured batches per direction. Timing includes recording, flush and retirement. Allocation, seeding and full payload/guard validation are excluded. A mapped host endpoint is reused for readback only after source verification. Repeated-region size-sweep rates remain diagnostic; cached logical bytes can exceed physical bus traffic.", "",
           "| Direction | Working set MiB | Regions | Median GB/s | Minimum GB/s | Maximum GB/s |",
           "|---|---:|---:|---:|---:|---:|"]
    if bandwidth:
        for direction, row in bandwidth["directions"].items():
            md.append(f"|{direction}|{bandwidth['payload_working_set_bytes']/(1<<20):g}|{bandwidth['regions']}|{row['median_GBps']:.3f}|{row['min_GBps']:.3f}|{row['max_GBps']:.3f}|")
    md += ["", "These measure the current HRX retained-buffer path, with coherent mapped host/GTT buffers and DEVICE_LOCAL VRAM. They establish measured sustained throughput for this workload, not a theoretical link limit or an SDMA-only rate. GB/s is decimal; MiB is binary.", "",
           "## 4. Kernel launch and communication latency", "",
           "| Measurement | Median us | P95 us | Minimum us | Samples | Clock |",
           "|---|---:|---:|---:|---:|---|"]
    for name, row in latency.items():
        value = row["ns"]
        md.append(f"|{labels.get(name, name)}|{value['median']/1000:.3f}|{value['p95']/1000:.3f}|{value['min']/1000:.3f}|{value['n']:,}|{row['clock']}|")
    md += ["", f"HRX completion uses its blocked timeline wait. Recorded MAC_HSA_BLOCKED_POLL_US: {policy.get('MAC_HSA_BLOCKED_POLL_US')}; effective blocked interval: {policy.get('effective_blocked_poll_us', 'unknown')} us. Bare HSA completion and mailbox loops actively poll. The recorded blocked-poll interval is part of these completion measurements.", "",
           "Kernel tests use one WG32 and a checked 4-byte store, with 32 warmups and 200 samples. Tiny transfers use 32 warmups and 256 samples per direction. CP intervals remain in the GPU clock domain and exclude host submission/polling.", "",
           "The persistent mailbox excludes a fresh kernel launch per round. Each initiator uses 128 warmups plus 1,024 measured round trips in each of three trials. Release/acquire ownership publishes an 8-byte payload without mixed CPU/GPU RMW. Timings include outward request, responder turnaround and polling. Exact one-way CPU→GPU and GPU→CPU latency is not established; no cross-domain subtraction or RTT/2 inference is used.", "",
           "P95/P99 use linear interpolation at sample position (n-1)×p. Full payloads, sequence numbers, source preservation, guards and clean retirement are required for successful output.", "",
           "## Raw artifacts", "",
           "CSV: compile.csv, bandwidth.csv, bandwidth-disjoint.csv, latency.csv. Metadata: results.json, build-metadata.json, environment.json, gpu.json, run-metadata.json, wait-policy.json. Compiler source fixtures, code objects, native binaries and logs remain in the artifact directory."]
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "DISCORD.txt").write_text(discord)
    (destination / "HRX-BENCHMARK-REPORT.md").write_text("\n".join(md) + "\n")
    print(destination / "HRX-BENCHMARK-REPORT.md")


if __name__ == "__main__":
    main()
