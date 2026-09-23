#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
c++ -std=c++20 -Wall -Wextra -Werror \
  -Ihsa/third_party/hsa/include -Iupstream/hrx-lse-pin/runtime/src \
  tests/hrx_abi_test.cpp -o build/tests/hrx-abi-test
build/tests/hrx-abi-test
