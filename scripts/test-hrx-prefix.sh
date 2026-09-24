#!/usr/bin/env bash
# Host-only tests of actual HRX wait resolution and AQL packet admission.
# No HSA initialization, GPU submission or hardware is required.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $# -le 1 ]] || { echo 'usage: scripts/test-hrx-prefix.sh [patched-HRX-source]' >&2; exit 2; }
hrx_source=${1:-"$PWD/build/hrx-macos-source"}
hrx_source=$(cd "$hrx_source" && pwd)
hrx_headers=${HSA_HEADERS:-"$PWD/upstream/hsa-runtime-headers/include"}
hrx_output="$PWD/build/tests/hrx-prefix"
mkdir -p "$hrx_output"
cc=${CC:-/usr/bin/clang}
common=(-std=gnu11 -O1 -g -DNDEBUG -fsanitize=address,undefined
        -I "$hrx_source/runtime/src" -I "$hrx_headers")
"$cc" "${common[@]}" tests/hrx_prefix_policy_test.c -o "$hrx_output/policy"
"$hrx_output/policy"
for mode in mac non-mac; do
  defines=()
  if [[ "$mode" == mac ]]; then
    defines+=(-DIREE_HAL_AMDGPU_MACOS_COARSE_HOST_ADAPTER=1)
  fi
  "$cc" "${common[@]}" "${defines[@]}" tests/hrx_prefix_resolution_test.c \
    "$hrx_source/runtime/src/iree/hal/drivers/amdgpu/host_queue_waits.c" \
    -o "$hrx_output/resolution-$mode"
  "$hrx_output/resolution-$mode"
done
