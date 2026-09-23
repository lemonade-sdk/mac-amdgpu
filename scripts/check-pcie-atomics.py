#!/usr/bin/env python3
"""Inspect cached GPU→root PCIe AtomicOp prerequisites without config writes.

Exit 0: all requested cached prerequisites present; 1: a prerequisite is
explicitly absent/disabled; 2: missing data prevents assessment (or input error).
None of these results establishes live CPU/GPU atomic interoperability.
"""
import argparse
import json
import plistlib
import subprocess
import sys

# PCI Express Device Capabilities 2 / Device Control 2. See Linux pci_regs.h
# and pci_enable_atomic_ops_to_root in drivers/pci/pci.c for path semantics.
COMPLETION = {32: 0x80, 64: 0x100, 128: 0x200}
ROUTING, REQUESTER_ENABLE, EGRESS_BLOCKING = 0x40, 0x40, 0x80
ROLES = {0: "endpoint", 1: "legacy endpoint", 4: "root port",
         5: "upstream port", 6: "downstream port"}


def number(value):
    if isinstance(value, bytes):
        return int.from_bytes(value, "little") if value else None
    return value if type(value) is int and value >= 0 else None


def gpu_paths(tree, vendor, device, parents=()):
    if isinstance(tree, list):
        for node in tree:
            yield from gpu_paths(node, vendor, device, parents)
    elif isinstance(tree, dict):
        path = parents + (tree,)
        if number(tree.get("vendor-id")) == vendor and number(tree.get("device-id")) == device:
            yield path
        for child in tree.get("IORegistryEntryChildren", []):
            yield from gpu_paths(child, vendor, device, path)


def bit(value, mask):
    return None if value is None else bool(value & mask)


def assess(path, widths=(32, 64)):
    rows, missing, unknown = [], [], []
    root_found = False
    endpoint_found = False

    def require(value, description):
        if value is None:
            unknown.append(description + ": cached register unavailable")
        elif not value:
            missing.append(description + ": not advertised/enabled")

    for index, node in enumerate(path):
        capabilities = number(node.get("IOPCIExpressCapabilities"))
        name = node.get("IORegistryEntryName", "unnamed")
        if capabilities is None:
            if number(node.get("vendor-id")) is not None:
                unknown.append(f"{name}: PCI function lacks cached PCIe capability/type")
            continue
        kind = (capabilities >> 4) & 15
        caps2 = number(node.get("IOPCIExpressDeviceCapabilities2"))
        control2 = number(node.get("IOPCIExpressDeviceControl2"))
        row = {"name": name, "role": ROLES.get(kind, f"type {kind}"),
               "capabilities2": caps2, "control2": control2,
               "routing": bit(caps2, ROUTING),
               "completer32": bit(caps2, COMPLETION[32]),
               "completer64": bit(caps2, COMPLETION[64]),
               "completer128": bit(caps2, COMPLETION[128]),
               "requester_enabled": bit(control2, REQUESTER_ENABLE),
               "egress_blocked": bit(control2, EGRESS_BLOCKING)}
        rows.append(row)
        if kind == 4:
            root_found = True
            for width in widths:
                require(bit(caps2, COMPLETION[width]), f"{name}: root {width}-bit AtomicOp completion")
        elif kind in (5, 6):
            require(bit(caps2, ROUTING), f"{name}: bridge AtomicOp routing")
            # GPU→root requests leave upstream ports on the upstream link.
            # Downstream egress applies to the opposite direction.
            if kind == 5:
                require(None if control2 is None else not control2 & EGRESS_BLOCKING,
                        f"{name}: upstream AtomicOp egress unblocked")
        elif index == len(path) - 1 and kind in (0, 1):
            endpoint_found = True
            require(bit(control2, REQUESTER_ENABLE), f"{name}: endpoint AtomicOp requester enable")
        else:
            unknown.append(f"{name}: unsupported PCIe ancestry role {kind}")
    if not root_found:
        unknown.append("No PCIe root port found; cached ancestry is incomplete")
    if not endpoint_found:
        unknown.append("Target is not a confirmed PCIe endpoint requester")
    return {"rows": rows, "missing": missing, "unknown": unknown,
            "required_widths": list(widths),
            "result": "missing" if missing else "unknown" if unknown else "advertised"}


def display_bool(value):
    return "unknown" if value is None else "yes" if value else "no"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ioreg-plist", help="Saved ioreg -a -p IOService archive (no live reads)")
    parser.add_argument("--device", default="1002:7551", help="PCI vendor:device hexadecimal")
    parser.add_argument("--width", action="append", type=int, choices=COMPLETION,
                        help="Required AtomicOp width; repeat to require several (default: 32 and 64)")
    parser.add_argument("--json", action="store_true", help="Machine-readable results; unknown values are null")
    args = parser.parse_args()
    try:
        vendor, device = (int(part, 16) for part in args.device.split(":"))
        if not 0 <= vendor <= 65535 or not 0 <= device <= 65535:
            raise ValueError("PCI IDs must be 16-bit")
        if args.ioreg_plist:
            with open(args.ioreg_plist, "rb") as source:
                tree = plistlib.load(source)
        else:
            tree = plistlib.loads(subprocess.check_output(
                ["/usr/sbin/ioreg", "-a", "-p", "IOService", "-c", "IOPCIDevice"], timeout=30))
    except (ValueError, OSError, plistlib.InvalidFileException, subprocess.SubprocessError) as error:
        print(f"Could not inspect PCIe capabilities: {error}", file=sys.stderr)
        return 2
    paths = list(gpu_paths(tree, vendor, device))
    if not paths:
        print(f"No {vendor:04x}:{device:04x} endpoint found", file=sys.stderr)
        return 2
    results = []
    for path in paths:
        report = assess(path, tuple(dict.fromkeys(args.width or (32, 64))))
        report["registry_id"] = path[-1].get("IORegistryEntryID")
        results.append(report)
        if args.json:
            continue
        print(f"GPU {vendor:04x}:{device:04x} registry={report['registry_id']}")
        for row in report["rows"]:
            cap = "unknown" if row["capabilities2"] is None else f"{row['capabilities2']:#010x}"
            ctl = "unknown" if row["control2"] is None else f"{row['control2']:#06x}"
            print(f"  {row['name']} ({row['role']}): caps2={cap} control2={ctl}")
            print("    " + " ".join(f"{field}={display_bool(row[field])}" for field in
                  ("routing", "completer32", "completer64", "completer128", "requester_enabled", "egress_blocked")))
        for issue in report["missing"]:
            print(f"  MISSING: {issue}")
        for issue in report["unknown"]:
            print(f"  UNKNOWN: {issue}")
        if report["result"] == "advertised":
            print("  Requested cached path prerequisites advertised; operational atomics unverified.")
    if args.json:
        print(json.dumps({"cached_only": True, "devices": results}, indent=2))
    else:
        print("Cached properties only; no PCI configuration writes. Values may be stale.")
        print("DMA coherence, DART translation, Thunderbolt transport and actual atomic completion remain unverified.")
        print("CPU ISA and endpoint completer bits alone do not establish the GPU-to-host atomic path.")
    return 1 if any(r["missing"] for r in results) else 2 if any(r["unknown"] for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
