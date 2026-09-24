#!/usr/bin/env bash
# CPU-only: exercises dlopen teardown through real LSE owners, no GPU initialization.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
lse_source="$PWD/build/lse-macos-source"
lse_build="$PWD/build/lse-macos-adapter"
lse_fixture="$lse_build/lse-runtime-lifetime-fixture.dylib"
"$llvm_bin/clang++" -std=c++26 -dynamiclib tests/lse_runtime_lifetime_fixture.cpp -o "$lse_fixture"
for mode in place standalone; do
  lse_mode_args=()
  if [[ "$mode" == place ]]; then
    lse_mode_args=(-Wl,-force_load,"$lse_build/liblse_place.a")
  else
    lse_mode_args=(-DLSE_LIFETIME_STANDALONE=1)
  fi
  "$llvm_bin/clang++" -std=c++26 -Wall -Wextra -Werror -I"$lse_source/include" \
    tests/lse_runtime_lifetime_test.cpp "${lse_mode_args[@]}" \
    "$lse_build/src/backends/cpu/liblse_backend_cpu.a" \
    -Wl,-force_load,"$lse_build/liblse_graph.a" \
    -Wl,-force_load,"$lse_build/liblse_kernels.a" \
    "$lse_build/liblse_probe.a" "$lse_build/liblse_dist.a" "$lse_build/liblse_communication.a" \
    "$lse_build/liblse_opt.a" "$lse_build/liblse_ir.a" "$lse_build/liblse_trace.a" \
    "$lse_build/src/backends/liblse_backend_registry.a" "$lse_build/liblse_core.a" \
    -L"$llvm_bin/../lib/c++" -Wl,-rpath,"$llvm_bin/../lib/c++" \
    -o "$lse_build/lse-runtime-lifetime-$mode"
  for entry in explicit lazy strict; do
    output="$($lse_build/lse-runtime-lifetime-$mode "$lse_fixture" "$entry")"
    [[ "$output" == $'main-completed\nbackend-destroyed\naccelerator-shutdown\nruntime-destroyed' ]]
    printf 'PASS: %s/%s owner -> accelerator -> runtime teardown\n' "$mode" "$entry"
  done
done
