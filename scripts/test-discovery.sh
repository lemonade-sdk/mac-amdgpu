#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
h = Path('dext/amdgpu/amdgpu_discovery.h').read_text()
Path('build/tests/discovery_types_under_test.inc').write_text(h[h.index('#pragma pack(push'):h.index('//\n// discovery_parse')])
s = Path('dext/amdgpu/amdgpu_discovery.cpp').read_text()
Path('build/tests/discovery_parser_under_test.inc').write_text(s[s.index('namespace {'):s.index('//\n// discover_ips_on_die')])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/discovery_test.cpp -o build/tests/discovery-test
build/tests/discovery-test
