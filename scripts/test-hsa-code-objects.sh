#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c tests/shaders/dispatch_gfx1201.cl -o build/tests/hsa-code-object.o
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/hsa-code-object.o -o build/tests/hsa-code-object.hsaco
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I hsa/src tests/hsa_code_object_test.cpp \
  hsa/src/code_object.cpp -o build/tests/hsa-code-object-test
bash scripts/build-resource-test.sh > build/tests/hsa-resource-metadata.txt
build/tests/hsa-code-object-test build/tests/hsa-code-object.hsaco build/tests/hsa-resource-object.hsaco \
  upstream/hrx-lse-pin/runtime/src/iree/hal/drivers/amdgpu/device/binaries/prebuilt/amdgcn-amd-amdhsa--gfx12-generic.so
