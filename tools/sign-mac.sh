#!/bin/sh
# Re-sign the studio app with a stable local identity so macOS keeps its
# microphone permission across rebuilds (ad-hoc signatures change every build).
# Usage: tools/sign-mac.sh [identity] [config]
IDENTITY="${1:-Woodshed Local Signing}"
CONFIG="${2:-Release}"
APP="$(dirname "$0")/../build/WoodshedStudio_artefacts/$CONFIG/Woodshed.app"
codesign --force --sign "$IDENTITY" "$APP" && codesign -d -r- "$APP" 2>&1 | tail -1
