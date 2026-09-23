#!/usr/bin/env python3
"""Offline query coverage check for the pinned HRX macOS adapter.

This checks queried attribute coverage, not hardware behavior or full HSA
conformance. Runtime tests separately validate values, types and error paths.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HRX = ROOT / "upstream/hrx-lse-pin/runtime/src/iree/hal/drivers/amdgpu"
TOKEN = re.compile(r"\bHSA_(?:(?:AMD_)?(?:SYSTEM|AGENT|AGENT_MEMORY_POOL|MEMORY_POOL)|ISA|REGION)_INFO_[A-Z0-9_]+\b")
# These queries are deliberately unsupported, with narrow caller exceptions.
# A new call site must be reviewed rather than inheriting the exception.
OPTIONAL = {
    "HSA_AMD_AGENT_INFO_HDP_FLUSH": ({"physical_device.c"}, "optional raw query; failure clears flush pointers"),
    "HSA_AMD_AGENT_INFO_NUM_XCC": ({"profile_aqlprofile.c"}, "profiling is rejected by the macOS adapter"),
}


def main():
    runtime = "\n".join(path.read_text() for path in (ROOT / "hsa/src").glob("*.cpp"))
    implemented = set(re.findall(r"\bcase\s+(HSA_[A-Z0-9_]+)\s*:", runtime))
    queried = {}
    for path in HRX.rglob("*.c"):
        if "_test." in path.name:
            continue
        for token in TOKEN.findall(path.read_text()):
            queried.setdefault(token, set()).add(str(path.relative_to(HRX)))
    errors = []
    for token, callers in sorted(queried.items()):
        if token in implemented:
            continue
        if token in OPTIONAL and callers <= OPTIONAL[token][0]:
            print(f"OPTIONAL {token}: {OPTIONAL[token][1]}")
        else:
            errors.append(f"MISSING {token}: {', '.join(sorted(callers))}")
    # This HRX alias preserves compilation against older public HSA headers.
    target = (HRX / "util/agent_target.c").read_text()
    headers = (ROOT / "hsa/third_party/hsa/include/hsa/hsa_ext_amd.h").read_text()
    alias = re.search(r"IREE_HAL_AMDGPU_AGENT_INFO_ASIC_REVISION\s*=\s*(0x[0-9A-Fa-f]+)", target)
    public = re.search(r"HSA_AMD_AGENT_INFO_ASIC_REVISION\s*=\s*(0x[0-9A-Fa-f]+)", headers)
    if not alias or not public or int(alias[1], 16) != int(public[1], 16) or "HSA_AMD_AGENT_INFO_ASIC_REVISION" not in implemented:
        errors.append("MISSING/MISMATCHED HRX numeric ASIC revision alias")
    for error in errors:
        print(error)
    if not errors:
        print(f"PASS: {len(queried)} pinned HRX system/agent/ISA/region/pool query attributes covered or explicitly optional; no GPU calls.")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
