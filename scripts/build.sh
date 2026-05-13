#!/usr/bin/env bash
#
# scripts/build.sh — generate + build the MacAMDGPU Xcode project.
#
# Usage:
#   scripts/build.sh              # generate (if needed) + build
#   scripts/build.sh --gen        # generate the .xcodeproj only, no build
#   scripts/build.sh --open       # generate + open in Xcode (no build)
#   scripts/build.sh --clean      # remove generated artifacts
#   scripts/build.sh --release    # build a Release configuration
#
# Required env:
#   XCODE_TEAM_ID — your 10-char Apple Developer team prefix.
#                   `security find-identity -p codesigning` lists yours.
#

set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT_ROOT="$(pwd)"
PROJECT_NAME="MacAMDGPU"
PROJECT_FILE="$PROJECT_ROOT/$PROJECT_NAME.xcodeproj"

config=Debug
do_gen=1
do_build=1
do_open=0

for arg in "$@"; do
  case "$arg" in
    --gen)     do_build=0 ;;
    --open)    do_open=1; do_build=0 ;;
    --clean)   rm -rf "$PROJECT_FILE" build/; exit 0 ;;
    --release) config=Release ;;
    -h|--help)
      sed -n '1,20p' "$0"; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

ensure_xcodegen() {
  if command -v xcodegen >/dev/null 2>&1; then
    return 0
  fi
  echo "==> xcodegen not found"
  if command -v brew >/dev/null 2>&1; then
    echo "==> installing via Homebrew: brew install xcodegen"
    brew install xcodegen
  else
    cat <<EOF
xcodegen is required to generate the .xcodeproj from project.yml.
Install it via Homebrew (\`brew install xcodegen\`) or build from
source: https://github.com/yonaskolb/XcodeGen

If you'd rather wire up the project by hand in Xcode, see
dext/README.md.
EOF
    exit 1
  fi
}

if [[ $do_gen -eq 1 ]]; then
  ensure_xcodegen

  # project.yml defaults to YBQ9BU6Q6F (Geramy's team) if env unset.
  # Override on the command line for builds against a different team.
  echo "==> using DEVELOPMENT_TEAM=${XCODE_TEAM_ID:-YBQ9BU6Q6F}"

  echo "==> generating $PROJECT_FILE from project.yml"
  xcodegen generate --spec project.yml --project "$PROJECT_ROOT" --quiet
fi

if [[ $do_open -eq 1 ]]; then
  echo "==> opening Xcode"
  open "$PROJECT_FILE"
  exit 0
fi

if [[ $do_build -eq 1 ]]; then
  echo "==> xcodebuild ($config)"
  xcodebuild \
    -project "$PROJECT_FILE" \
    -scheme  "$PROJECT_NAME"Host \
    -configuration "$config" \
    -destination 'platform=macOS' \
    build | tee build.log
  echo ""
  echo "==> built — locate the app:"
  echo "    xcodebuild -showBuildSettings -scheme MacAMDGPUHost \\"
  echo "      | awk '/ BUILT_PRODUCTS_DIR / {print \$3}'"
  echo ""
  echo "Next step: drag MacAMDGPUHost.app into /Applications and"
  echo "launch it once to activate the dext. See dext/README.md."
fi
