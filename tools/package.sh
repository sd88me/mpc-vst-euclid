#!/usr/bin/env bash
# The release zips (one per CPU). Builds the release identity ("Euclid", uid EqDr) first.
#   tools/package.sh   -> dist/Euclid-<version>-VST2-Plugins-{aarch64,armv7}.zip   (version from plugin.json)
set -euo pipefail
cd "$(dirname "$0")/.."
./build-sdk.sh
python3 tools/package.py
