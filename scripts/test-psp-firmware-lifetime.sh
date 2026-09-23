#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
source = Path('dext/amdgpu/psp_v14_0.cpp').read_text()
start = source.index('static void\nset_sub_bin(')
end = source.index('// psp_load_sos_package —', start)
functions = source[start:end]
assert 'psp_parse_sos_microcode(' in functions
assert 'psp_parse_sos_views(' in functions
Path('build/tests/psp_snapshot_under_test.inc').write_text(functions)
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/psp_firmware_lifetime_test.cpp \
  -o build/tests/psp-firmware-lifetime-test
build/tests/psp-firmware-lifetime-test
