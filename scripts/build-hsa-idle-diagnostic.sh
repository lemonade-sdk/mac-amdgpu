#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
mkdir -p build/hsa-idle-diagnostic
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -Ihsa/include -Ihsa/third_party/hsa/include \
  hsa/tools/mac_hsa_idle_diagnostic.cpp hsa/src/device_init.cpp \
  -framework IOKit -framework CoreFoundation \
  -o build/hsa-idle-diagnostic/mac-hsa-idle-diagnostic
build/hsa-idle-diagnostic/mac-hsa-idle-diagnostic --check
