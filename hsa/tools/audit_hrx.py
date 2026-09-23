#!/usr/bin/env python3
"""Compare HRX's required dynamic HSA symbols against a built macOS runtime."""

import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--hrx", type=Path, required=True, help="HRX System source tree")
parser.add_argument("--library", type=Path, required=True, help="Built HSA dylib")
parser.add_argument("--require-runtime-ready", action="store_true",
                    help="Also fail if tracked GPU/HRX runtime requirements are incomplete")
args = parser.parse_args()
table = args.hrx / "runtime/src/iree/hal/drivers/amdgpu/util/libhsa_tables.h"
required = set(re.findall(
    r"IREE_HAL_AMDGPU_LIBHSA_(?:LEAK_CHECK_DISABLED_)?PFN\(\s*\w+\s*,\s*"
    r"[\w\s*]+,\s*(hsa_\w+)\s*,", table.read_text()))
if not required:
    parser.error("No required HSA symbols found; inspect the upstream table format")
symbols = subprocess.run(["nm", "-gU", str(args.library)], check=True,
                         text=True, capture_output=True).stdout
exported = set(re.findall(r"\b_(hsa_\w+)$", symbols, re.M))
missing = sorted(required - exported)
print(f"HRX requires {len(required)} HSA symbols; {len(required & exported)} exported; "
      f"{len(missing)} missing.")
for symbol in missing:
    print(f"  missing: {symbol}")
print("Symbol presence does not prove behavior, extension-table support or kernel execution.")
status = json.loads((Path(__file__).resolve().parents[1] / "api_status.json").read_text())
unsupported = sorted(required & set(status["platform_unsupported"]))
incomplete = [name for name, ready in status["runtime_requirements"].items() if not ready]
print(f"Platform-unsupported entry points returning errors: {len(unsupported)}.")
print("Incomplete runtime requirements: " + (", ".join(incomplete) or "none tracked"))
raise SystemExit(1 if missing else 2 if args.require_runtime_ready and incomplete else 0)
