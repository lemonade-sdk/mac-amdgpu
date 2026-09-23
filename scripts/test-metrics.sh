#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun clang++ -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I upstream/linux/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if \
  tests/metrics_test.cpp -o build/tests/metrics-test
build/tests/metrics-test
