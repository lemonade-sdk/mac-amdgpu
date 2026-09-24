#!/usr/bin/env bash
# Build and validate the host oracle only; GPU access requires a separate --run.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
source scripts/amdgpu-llvm-env.sh
hrx_source="$repo_root/build/hrx-macos-source"
hrx_build="$repo_root/build/hrx-macos-adapter"
hrx_lib="$hrx_build/libhrx/src/libhrx"
if [[ ! -f "$hrx_source/libhrx/include/hrx_runtime.h" || ! -f "$hrx_lib/libhrx.dylib" ]]; then
  printf 'Build pinned HRX first with: bash scripts/build-hrx-macos.sh\n' >&2
  exit 1
fi
"$llvm_bin/clang++" -std=c++20 -O2 -Wall -Wextra -Werror \
  -I"$hrx_source/libhrx/include" hsa/tools/mac_hrx_bandwidth.cpp \
  -L"$hrx_lib" -lhrx -Wl,-rpath,"$hrx_lib" \
  -o "$hrx_build/mac-hrx-bandwidth"
"$hrx_build/mac-hrx-bandwidth" --check-host
printf 'Built %s. GPU test: python3 scripts/run-hrx-bandwidth.py\n' "$hrx_build/mac-hrx-bandwidth"
