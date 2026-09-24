#!/usr/bin/env python3
"""Build and run host-only status conversion tests; no runtime initialization."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", required=True, type=Path, help="Patched HRX source checkout")
parser.add_argument("--build", required=True, type=Path, help="Configured HRX build with compile_commands.json")
parser.add_argument("--status-source", type=Path, help="Optional pre-fix source for negative control")
options = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source, build = options.source.resolve(), options.build.resolve()
entries = json.loads((build / "compile_commands.json").read_text())
entry = next(e for e in entries if e["file"] == str(source / "libhrx/src/libhrx/status.c")
             and "hrx.objects" in e["command"])
args = shlex.split(entry["command"])
args[0] = os.environ.get("CC", "/usr/bin/clang")
for option in ("-o", "-c"):
    index = args.index(option)
    del args[index:index + 2]
args = [a for a in args if a not in ("-O3", "-DHRX_BUILDING_SHARED")]
args += ["-O1", "-g", "-fsanitize=address,undefined", "-DHRX_STATIC"]
status_source = options.status_source.resolve() if options.status_source else source / "libhrx/src/libhrx/status.c"
output = root / "build/tests/hrx_status_roundtrip_sanitized"
output.parent.mkdir(parents=True, exist_ok=True)
args += [str(status_source), str(root / "tests/hrx_status_roundtrip_test.c"),
         str(build / "runtime/src/iree/base/libiree_base_base.a"), "-o", str(output)]
subprocess.run(args, cwd=entry["directory"], check=True)
subprocess.run([str(output)], check=True)
