#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu tests/pm4_packet_test.cpp -o build/tests/pm4-packet-test
build/tests/pm4-packet-test
