#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu tests/vram_accounting_test.cpp -o build/tests/vram-accounting-test
build/tests/vram-accounting-test
