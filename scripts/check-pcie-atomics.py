#!/usr/bin/env python3
"""Read cached PCIe AtomicOp capabilities; never write PCI configuration."""
import argparse
import plistlib
import subprocess
import sys


def number(value):
    if isinstance(value, bytes):
        return int.from_bytes(value, "little")
    return value if isinstance(value, int) else None


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


def assess(path):
    rows, failures = [], []
    root_found = False
    for node in path:
        capabilities = number(node.get("IOPCIExpressCapabilities"))
        if capabilities is None:
            continue
        kind = (capabilities >> 4) & 15
        caps2 = number(node.get("IOPCIExpressDeviceCapabilities2"))
        name = node.get("IORegistryEntryName", "unnamed")
        roles = {0: "endpoint", 1: "legacy endpoint", 4: "root port", 5: "upstream port", 6: "downstream port"}
        rows.append((name, roles.get(kind, f"type {kind}"), caps2))
        if kind == 4:
            root_found = True
            if caps2 is None or caps2 & 0x180 != 0x180:
                failures.append(f"{name}: root lacks advertised 32-bit/64-bit AtomicOp completion")
        elif kind in (5, 6):
            if caps2 is None or not caps2 & 0x40:
                failures.append(f"{name}: bridge lacks advertised AtomicOp routing")
    if not root_found:
        failures.append("No PCIe root port found in this IOService ancestry; capability path is incomplete")
    return rows, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ioreg-plist", help="Inspect a saved ioreg -a -p IOService archive instead of live devices")
    parser.add_argument("--device", default="1002:7551", help="PCI vendor:device in hexadecimal (default: 1002:7551)")
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
    failed = False
    for path in paths:
        print(f"GPU {vendor:04x}:{device:04x} registry={path[-1].get('IORegistryEntryID', 'unknown')}")
        rows, failures = assess(path)
        for name, role, caps in rows:
            if caps is None:
                print(f"  {name} ({role}): DeviceCapabilities2 unavailable")
            else:
                print(f"  {name} ({role}): caps2={caps:#010x} routing={bool(caps & 0x40)} "
                      f"completer32={bool(caps & 0x80)} completer64={bool(caps & 0x100)}")
        for failure in failures:
            print(f"  UNAVAILABLE: {failure}")
        failed |= bool(failures)
        if not failures:
            print("  Required capability bits are advertised; operational support is not verified.")
    print("Cached capabilities only: requester enable, egress blocking, DART behavior and live coherence are not checked.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
