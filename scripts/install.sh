#!/usr/bin/env bash
#
# scripts/install.sh — install + activate MacAMDGPU non-interactively.
#
# What it does:
#   1. Builds Release (if not already built).
#   2. Removes any existing /Applications/MacAMDGPUHost.app.
#   3. Copies the freshly-built app to /Applications/.
#   4. Enables developer-mode dext staging (sudo).
#   5. Launches the host app, which auto-submits the activation
#      request and opens System Settings → Privacy & Security if
#      the user needs to click Allow.
#
# Usage:
#   scripts/install.sh                # Debug build (default)
#   scripts/install.sh --release      # Release build
#   scripts/install.sh --uninstall    # systemextensionsctl uninstall +
#                                       remove /Applications/MacAMDGPUHost.app
#
# Notes:
#   - Step 4 needs sudo. We'll prompt; if you'd rather it not need
#     sudo, run `sudo systemextensionsctl developer on` once by hand.
#   - The first time this runs Apple's security UI will pop. After
#     you click Allow the dext is permanent until you uninstall.
#

set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT_ROOT="$(pwd)"
APP_NAME="MacAMDGPUHost"
DEXT_BUNDLE_ID="com.geramyloveless.MacAMDGPUHost.MacAMDGPU"

config=Debug
uninstall=0

for arg in "$@"; do
  case "$arg" in
    --release)   config=Release ;;
    --uninstall) uninstall=1 ;;
    -h|--help)
      sed -n '1,28p' "$0"; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [[ $uninstall -eq 1 ]]; then
  echo "==> uninstalling dext"
  team_id="$(security find-identity -p codesigning -v 2>/dev/null \
    | grep -oE '\([A-Z0-9]{10}\)' | head -1 | tr -d '()' || true)"
  if [[ -n "${team_id:-}" ]]; then
    sudo systemextensionsctl uninstall "$team_id" "$DEXT_BUNDLE_ID" || true
  else
    echo "warning: couldn't find team id; run \`systemextensionsctl uninstall <TEAM> $DEXT_BUNDLE_ID\` manually" >&2
  fi
  echo "==> removing /Applications/${APP_NAME}.app"
  sudo rm -rf "/Applications/${APP_NAME}.app"
  echo "done."
  exit 0
fi

# 1. Build (if needed).
if [[ ! -d "$PROJECT_ROOT/MacAMDGPU.xcodeproj" ]]; then
  echo "==> generating Xcode project"
  "$PROJECT_ROOT/scripts/build.sh" --gen
fi
: "${XCODE_TEAM_ID:=YBQ9BU6Q6F}"
echo "==> building ${APP_NAME} ($config) — team ${XCODE_TEAM_ID}"
build_log=$(mktemp)
if ! xcodebuild -project "$PROJECT_ROOT/MacAMDGPU.xcodeproj" \
           -scheme "$APP_NAME" \
           -configuration "$config" \
           -destination 'platform=macOS' \
           DEVELOPMENT_TEAM="$XCODE_TEAM_ID" \
           -allowProvisioningUpdates \
           build > "$build_log" 2>&1; then
  echo "build failed — last 30 lines of build log:" >&2
  tail -30 "$build_log" >&2
  rm -f "$build_log"
  exit 1
fi
rm -f "$build_log"

# 2. Find the built app.
APP_PATH="$(xcodebuild -project "$PROJECT_ROOT/MacAMDGPU.xcodeproj" \
            -scheme "$APP_NAME" -configuration "$config" \
            -showBuildSettings 2>/dev/null \
  | awk '/ BUILT_PRODUCTS_DIR / {print $3}')/${APP_NAME}.app"
if [[ ! -d "$APP_PATH" ]]; then
  echo "error: built app not found at $APP_PATH" >&2
  exit 1
fi
echo "==> built: $APP_PATH"

# 3. Copy to /Applications. /Applications is user-writable on macOS by
# default; only fall back to sudo if the plain copy fails.
echo "==> copying to /Applications/${APP_NAME}.app"
rm -rf "/Applications/${APP_NAME}.app" 2>/dev/null || \
  sudo rm -rf "/Applications/${APP_NAME}.app"
if ! cp -R "$APP_PATH" "/Applications/" 2>/dev/null; then
  echo "==> plain cp failed; retrying with sudo"
  sudo cp -R "$APP_PATH" "/Applications/"
fi

# 4. Enable developer-mode dext staging. Already-on case is fine
# (systemextensionsctl is idempotent). Detect and skip the sudo
# if it's already enabled.
dev_state="$(systemextensionsctl developer 2>&1 || true)"
if [[ "$dev_state" == *"Developer mode is enabled"* ]]; then
  echo "==> systemextensionsctl developer mode already on"
else
  echo "==> enabling systemextensionsctl developer mode (sudo)"
  sudo systemextensionsctl developer on || true
fi

# 5. Launch the app — it will auto-submit the activation request.
echo "==> launching /Applications/${APP_NAME}.app"
open "/Applications/${APP_NAME}.app"

cat <<'EOF'

==> Next steps:
    1. macOS may pop "System Extension Blocked" — go to
         System Settings → Privacy & Security
       and click Allow next to MacAMDGPU.
    2. Verify staging:
         systemextensionsctl list
       Look for: com.geramyloveless.MacAMDGPUHost.MacAMDGPU [activated enabled]
    3. Verify binding:
         ioreg -lw0 -c IOPCIDevice | grep -B1 -A6 'device-id.*7551'
       Look for: IOUserClientClass = MacAMDGPUUserClient
    4. Tail logs in another terminal:
         log stream --predicate 'subsystem CONTAINS "mac.amdgpu"'
    5. Run the userspace test:
         ./build/macamdgpu_ping
EOF
