#!/usr/bin/env bash
# Build the pinned LSE host/HRX adapter and run CPU-only portability tests.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
source scripts/amdgpu-llvm-env.sh
lse_revision=b5637a7109d409c21f75586edb75e7631277bce8
lse_source="$repo_root/upstream/lse"
lse_copy="$repo_root/build/lse-macos-source"
lse_build="$repo_root/build/lse-macos-adapter"
lse_tests="$repo_root/build/lse-macos-host-tests"
lse_patch="$repo_root/patches/lse/macos-host-adapter.patch"
hrx_build="$repo_root/build/hrx-macos-adapter"
hrx_copy="$repo_root/build/hrx-macos-source"
[[ "$(uname -s)" == Darwin ]] || { echo 'This adapter targets macOS.' >&2; exit 1; }
[[ "$(git -C "$lse_source" rev-parse HEAD)" == "$lse_revision" ]] || {
  echo 'LSE pin changed: re-audit the adapter before building.' >&2; exit 1;
}
[[ -f "$hrx_build/loom/binding/c/libloomc.dylib" &&
   -f "$hrx_build/libhrx/src/libhrx/libhrx.dylib" ]] || {
  echo 'Build native HRX and Loom first: bash scripts/build-hrx-macos.sh' >&2; exit 1;
}
if [[ ! -d "$lse_copy" ]]; then
  git clone --shared --no-hardlinks "$lse_source" "$lse_copy"
fi
[[ "$(git -C "$lse_copy" rev-parse HEAD)" == "$lse_revision" ]] || {
  echo 'Existing LSE build source is at an unexpected revision.' >&2; exit 1;
}
if git -C "$lse_copy" apply --reverse --check "$lse_patch" 2>/dev/null; then
  : # Exact adapter is already applied.
else
  git -C "$lse_copy" apply --check "$lse_patch"
  git -C "$lse_copy" apply "$lse_patch"
fi
# New libc++ headers need the matching library, not the older SDK library.
lse_link_flags="-L$llvm_bin/../lib/c++ -Wl,-rpath,$llvm_bin/../lib/c++"
lse_common_args=(-G Ninja "-DCMAKE_CXX_COMPILER=$llvm_bin/clang++"
  "-DCMAKE_EXE_LINKER_FLAGS=$lse_link_flags" -DLSE_ENABLE_CPU=ON)
cmake -S "$lse_copy" -B "$lse_build" "${lse_common_args[@]}" \
  -DLSE_ENABLE_HRX=ON -DLSE_BUILD_TESTS=OFF -DLSE_GPU_TARGETS=gfx1201 \
  "-DLSE_HRX_INCLUDE_DIR=$hrx_copy/libhrx/include" \
  "-DLSE_HRX_LIBRARY=$hrx_build/libhrx/src/libhrx/libhrx.dylib" \
  "-DLSE_LOOMC_INCLUDE_DIR=$hrx_copy/loom/binding/c/include" \
  "-DLSE_LOOMC_LIBRARY=$hrx_build/loom/binding/c/libloomc.dylib"
cmake --build "$lse_build" --target lse lse-server --parallel "${LSE_BUILD_JOBS:-4}"
# --help exits before backend initialization: this does not access the GPU.
"$lse_build/lse" --help > "$lse_build/help.txt"
python3 scripts/test-lse-server-cli.py "$lse_build/lse-server"
cmake -S "$lse_copy" -B "$lse_tests" "${lse_common_args[@]}" \
  -DLSE_ENABLE_HRX=OFF -DLSE_BUILD_TESTS=ON
lse_test_targets=(test_kernel_env test_ir test_dtype test_shape test_quant test_backend_cpu test_primitive test_trace
  test_loom_print test_loom_repeat test_loom_gdn test_loom_extent test_loom_conv test_loom_words test_loom_flash
  test_generation_stats test_server_shutdown test_dispatch_profile test_loom_cache test_pointwise_fusion test_probe_measurement test_probe_policy test_quant_prefill test_attention_decode test_token_ids test_submission_tuner test_submission_constants test_submission_decode test_decode_sample
  test_loaded_runtime test_hrx_copy_route test_gdn_pair test_gdn_scheduler
  test_scheduler_epilogue test_loom_matrix test_loom_dot test_fp8_conversion test_quant_operand_policy)
cmake --build "$lse_tests" --target "${lse_test_targets[@]}" lse_communication --parallel "${LSE_BUILD_JOBS:-4}"
ctest --test-dir "$lse_tests" --output-on-failure \
  -R '^test_(kernel_env|ir|dtype|shape|quant|backend_cpu|primitive|trace|loom_print|loom_repeat|loom_gdn|loom_extent|loom_conv|loom_words|loom_flash|generation_stats|server_shutdown|dispatch_profile|loom_cache|pointwise_fusion|probe_measurement|probe_policy|quant_prefill|token_ids|submission_tuner|submission_constants|submission_decode|decode_sample|loaded_runtime|hrx_copy_route|gdn_pair|gdn_scheduler|scheduler_epilogue|loom_matrix|loom_dot|fp8_conversion|quant_operand_policy)$'
bash scripts/test-lse-runtime-lifetime.sh
"$llvm_bin/clang++" -std=c++26 -Wall -Wextra -Werror \
  -I"$lse_copy/include" tests/lse_macos_poller_test.cpp \
  "$lse_tests/liblse_core.a" \
  -Wl,-force_load,"$lse_tests/liblse_communication.a" \
  -L"$llvm_bin/../lib/c++" -Wl,-rpath,"$llvm_bin/../lib/c++" \
  -o "$lse_tests/lse-macos-poller-test"
"$lse_tests/lse-macos-poller-test"
printf 'Built LSE %s with native HRX/Loom; CPU-only tests passed. No GPU work run.\n' "$lse_revision"
