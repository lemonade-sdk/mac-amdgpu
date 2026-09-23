#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests build/ModuleCache
python3 - <<'PY'
from pathlib import Path
host = Path('Host/MacAMDGPUHostApp.swift').read_text()
start = host.index('private struct DriverRuntimeIdentity:')
end = host.index('// MARK: - User-client selectors', start)
identity = host[start:end]
start = host.index('    nonisolated private static func probeRuntime(')
end = host.index('    private func startRuntimeVerification()', start)
probe = host[start:end].replace('nonisolated private static func', 'static func', 1)
test = Path('tests/runtime_identity_test.swift').read_text()
test = test.replace('// PRODUCTION_IDENTITY', identity).replace('// PRODUCTION_PROBE', probe)
Path('build/tests/runtime_identity_test.swift').write_text(test)
source = Path('dext/MacAMDGPU.cpp').read_text()
start = source.index('    case kMacAMDGPUMethodRuntimeBuild: {')
end = source.index('    case kMacAMDGPUMethodPing:', start)
Path('build/tests/runtime_selector_under_test.inc').write_text(source[start:end])
PY
xcrun swiftc -swift-version 5 -module-cache-path build/ModuleCache \
  build/tests/runtime_identity_test.swift -o build/tests/runtime-identity-test
build/tests/runtime-identity-test
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I build/tests tests/runtime_selector_test.cpp -o build/tests/runtime-selector-test
build/tests/runtime-selector-test
