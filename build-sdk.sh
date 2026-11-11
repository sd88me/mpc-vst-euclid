#!/usr/bin/env bash
# Build Euclidier on the MPC Plugin SDK front end (src/sdk/EuclidierPlugin.cpp) into build/sdk/"sd88me - VST - Euclidier"/ :
# arm32 (Force, MPC Live / One / X) and arm64 (Live III). The sequencer is inside the plugin (src/core). It links ALSA (the notes
# go out through an ALSA sequencer port), so the SDK's zig build is not used: both targets are compiled with gcc in Docker
# (scripts/Dockerfile).
#   ./build-sdk.sh            gen params, build both targets, assemble the folder (plugin.json, version.xml, Plugin Skins)
#   ./build-sdk.sh beta       the same as "Euclidier 2 Beta" (uid EqDB): installs next to the release
#   ./build-sdk.sh test       the offline test (tests/run.sh) and the SDK's host simulator
# MPC_SDK: the mpc-plugin-builder-sdk checkout.
set -euo pipefail
cd "$(dirname "$0")"
SDK="${MPC_SDK:-$HOME/mpc-310-private/mpc-plugin-builder-sdk}"
[ -f "$SDK/framework/mpc_plugin.h" ] || { echo "need the SDK (MPC_SDK)" >&2; exit 1; }
NAME="Euclid"; UID_="'E','q','D','r'"; SLUG=euclid
if [ "${1:-}" = beta ]; then NAME="Euclid 2 Beta"; UID_="'E','q','D','B'"; SLUG=euclid2beta; fi
python3 tools/gen_sdk_params.py
if [ "${1:-}" = test ]; then
  tests/run.sh
  docker image inspect euclidier-sdk-run >/dev/null 2>&1 || docker build -q -t euclidier-sdk-run -f scripts/Dockerfile.run scripts >/dev/null
  mkdir -p build/sdk-native
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b euclidier-sdk-x64 sh -c \
    'g++ -std=c++17 -O1 -w -fPIC -shared -fvisibility=hidden -I/sdk/framework -Isrc/sdk -Isrc -Ivst -o build/sdk-native/euclidier.so src/sdk/EuclidierPlugin.cpp src/core/euclid_core.cpp src/bjlund.cpp -lasound -lpthread -lm'
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b euclidier-sdk-run sh -c \
    '/sdk/.sdk-tools/host_sim build/sdk-native/euclidier.so dump > build/sdk-native/params.json; /sdk/.sdk-tools/host_sim build/sdk-native/euclidier.so test --playing --notes 60 2>&1 | grep -v "ALSA lib" | tail -n 14'
  exit
fi
FOLDER="build/sdk/sd88me - VST - $NAME"
rm -rf "$FOLDER"; mkdir -p "$FOLDER/VST" "$FOLDER/Plugin Skins"
build() {   # <docker platform> <image> <base image> <cpu>
  docker image inspect "$2" >/dev/null 2>&1 || docker build -q -t "$2" --platform "$1" --build-arg BASE="$3" scripts >/dev/null
  docker run --rm --platform "$1" -u "$(id -u):$(id -g)" -e EUC_N="$NAME" -e EUC_U="$UID_" -e EUC_S="$SLUG" -e TAG="$2" \
    -e OUT="$FOLDER/VST/$SLUG-$4.so" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b "$2" scripts/compile.sh
}
mkdir -p build
build linux/arm/v7 euclidier-sdk-arm32 arm32v7/gcc:11-bullseye arm32 &
build linux/arm64 euclidier-sdk-arm64 arm64v8/gcc:12-bookworm arm64 &
wait
python3 - "$NAME" "$SLUG" "$FOLDER" <<'PY'
import json, sys
name, slug, folder = sys.argv[1:]
m = json.load(open("plugin.json"))
m["name"] = name
if name != "Euclid":
    m["uid"] = "EqDB"
m["file32"] = "VST/%s-arm32.so" % slug
m["file64"] = "VST/%s-arm64.so" % slug
json.dump(m, open(folder + "/plugin.json", "w"), indent=2)
open(folder + "/version.xml", "w").write('<?xml version="1.0" encoding="utf-8"?>\n<plugincontent version="1.0">\n    <version>%s</version>\n    <uiversion>0.1.0.0</uiversion>\n</plugincontent>\n' % m["version"])
PY
[ -d skins ] || python3 tools/make_tui.py
cp -r skins/. "$FOLDER/Plugin Skins/"
python3 "$SDK/tools/validate.py" "$FOLDER" || true
echo "built: $FOLDER"
