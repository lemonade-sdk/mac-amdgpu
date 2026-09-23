#!/usr/bin/env bash
# Build the pinned HRX with an explicit macOS transport adapter. No GPU work.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
source scripts/amdgpu-llvm-env.sh
hrx_revision=5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c
hrx_source="$repo_root/upstream/hrx-lse-pin"
hrx_copy="$repo_root/build/hrx-macos-source"
hrx_build="$repo_root/build/hrx-macos-adapter"
hrx_patch="$repo_root/patches/hrx/macos-coarse-host-adapter.patch"
[[ "$(uname -s)" == Darwin ]] || { echo 'This adapter targets macOS.' >&2; exit 1; }
[[ "$(git -C "$hrx_source" rev-parse HEAD)" == "$hrx_revision" ]] || {
  echo 'HRX pin changed: re-audit the adapter before building.' >&2; exit 1;
}
if [[ ! -d "$hrx_copy" ]]; then
  git clone --shared --no-hardlinks "$hrx_source" "$hrx_copy"
fi
[[ "$(git -C "$hrx_copy" rev-parse HEAD)" == "$hrx_revision" ]] || {
  echo 'Existing build source is at an unexpected revision.' >&2; exit 1;
}
if git -C "$hrx_copy" apply --reverse --check "$hrx_patch" 2>/dev/null; then
  : # The exact tracked adapter is already applied.
else
  git -C "$hrx_copy" apply --check "$hrx_patch"
  git -C "$hrx_copy" apply "$hrx_patch"
fi
hrx_dependency_args=()
if [[ -f "$repo_root/build/hrx-macos/_deps/flatcc-src/include/flatcc/flatcc.h" ]]; then
  hrx_dependency_args+=("-DFETCHCONTENT_SOURCE_DIR_FLATCC=$repo_root/build/hrx-macos/_deps/flatcc-src")
fi
cmake -S "$hrx_copy" -B "$hrx_build" -G Ninja \
  -DCMAKE_C_COMPILER="$llvm_bin/clang" \
  -DCMAKE_CXX_COMPILER="$llvm_bin/clang++" \
  -DCMAKE_C_FLAGS=-DIREE_HAL_AMDGPU_MACOS_COARSE_HOST_ADAPTER=1 \
  -DIREE_BUILD_TESTS=OFF -DIREE_BUILD_BENCHMARKS=OFF \
  -DIREE_HAL_DRIVER_DEFAULTS=OFF -DIREE_HAL_DRIVER_AMDGPU=ON \
  -DIREE_HAL_AMDGPU_TARGETS=gfx1201 -DLOOM_BUILD="${HRX_BUILD_LOOM:-ON}" \
  -DLOOM_TARGET_DEFAULTS=OFF -DLOOM_TARGET_AMDGPU=ON \
  -DLOOM_TARGET_AMDGPU_TARGETS=gfx1201 \
  -DLIBHRX_BUILD_HIP_BINDING=OFF -DLIBHRX_BUILD_CTS=OFF \
  -DIREE_ENABLE_LIBBACKTRACE=OFF \
  -DFETCHCONTENT_SOURCE_DIR_HSA_RUNTIME_HEADERS="$repo_root/upstream/hsa-runtime-headers" \
  "${hrx_dependency_args[@]}"
cmake --build "$hrx_build" --target hrx -j "${HRX_BUILD_JOBS:-8}"
if [[ "${HRX_BUILD_LOOM:-ON}" == ON ]]; then
  cmake --build "$hrx_build" --target loomc_shared -j "${HRX_BUILD_JOBS:-8}"
fi
hrx_lib="$hrx_build/libhrx/src/libhrx"
bash scripts/build-hrx-compute-fixture.sh
"$llvm_bin/clang" -std=c11 -Wall -Wextra -Werror \
  -I"$hrx_copy/libhrx/include" -I"$repo_root/build/tests" hsa/tools/mac_hrx_smoke.c \
  -L"$hrx_lib" -lhrx -Wl,-rpath,"$hrx_lib" \
  -o "$hrx_build/mac-hrx-smoke"
"$hrx_build/mac-hrx-smoke" --check-library
printf 'Built HRX %s. Hardware test (explicit opt-in):\n' "$hrx_revision"
printf 'DYLD_LIBRARY_PATH=%q %q --run\n' "$repo_root/build/hsa" "$hrx_build/mac-hrx-smoke"
