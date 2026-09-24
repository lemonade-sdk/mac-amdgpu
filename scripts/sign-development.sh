#!/usr/bin/env bash
# Sign the development host and dext using the tested entitlement/runtime flags.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
app_path="${1:-$repo_root/build/compute-smoke/Build/Products/Debug/MacAMDGPUHost.app}"
if [[ -z "${SIGN_IDENTITY:-}" ]]; then
  echo 'Set SIGN_IDENTITY to your Apple Development identity (security find-identity -v -p codesigning).' >&2
  exit 2
fi
dexts=("$app_path/Contents/Library/SystemExtensions/"*.dext)
[[ ${#dexts[@]} -eq 1 && -d "${dexts[0]}" ]] || {
  echo "Expected one built dext inside $app_path" >&2; exit 2;
}
dext_path="${dexts[0]}"
codesign --force --sign "$SIGN_IDENTITY" -o library,runtime \
  --entitlements "$repo_root/dext/Development.entitlements" "$dext_path"
codesign --force --sign "$SIGN_IDENTITY" --options 0 \
  --entitlements "$repo_root/Host/Host.entitlements" "$app_path"
codesign --verify --deep --strict --verbose=2 "$app_path"
