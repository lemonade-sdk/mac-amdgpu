#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S amdgpu_mtop -B build/amdgpu_mtop -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/amdgpu_mtop --parallel 4
ctest --test-dir build/amdgpu_mtop --output-on-failure
