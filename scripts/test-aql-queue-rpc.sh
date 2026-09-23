#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('    case kMacAMDGPUMethodAQLQueueCreate:')
b=s.index('    case kMacAMDGPUMethodAQLDispatch:',a)
Path('build/tests/aql_queue_rpc.inc').write_text(s[a:b])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I build/tests tests/aql_queue_rpc_test.cpp -o build/tests/aql-queue-rpc-test
build/tests/aql-queue-rpc-test
