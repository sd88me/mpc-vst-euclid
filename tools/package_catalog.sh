#!/usr/bin/env bash
# The catalog-shaped release zips, from the release build (build/sdk/"sd88me - VST - Euclid", made by ./build-sdk.sh):
#   dist/Euclid-<ver>-mpc-armv7.zip, -mpc-aarch64.zip   per CPU, made by mpc-vst-plugins' tools/release.py (manifest, install.sh /
#                                                        uninstall.sh that Plugin Manager and the desktop installer run)
#   dist/Euclid-<ver>-mpc-sdk.zip                       one folder for every MPC (tools/sdk_zip.py), for copying into "VST2 Plugins"
# Euclid replaces Euclidier under a new folder name with the same uid and the same catalog id (euclidier): release.py --retire makes
# the installer remove the old "sd88me - VST - Euclidier" folder (only one whose mpc-plugin.json has this id) and move its preset bank
# (euclidierBANK.bin, user data) across. The .so sits at the folder root as euclid.so.
# MPC_VST: an mpc-vst-plugins checkout with tools/release.py (--retire) and tools/sdk_zip.py.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MPC_VST="${MPC_VST:-$HOME/mpc-vst-plugins-postlaunch}"
[ -f "$MPC_VST/tools/release.py" ] && [ -f "$MPC_VST/tools/sdk_zip.py" ] || { echo "need mpc-vst-plugins with tools/sdk_zip.py (MPC_VST)" >&2; exit 1; }
grep -q -- "--retire" "$MPC_VST/tools/release.py" || { echo "this mpc-vst-plugins has no release.py --retire" >&2; exit 1; }
DIR="sd88me - VST - Euclid"; SRC="$here/build/sdk/$DIR"
[ -f "$SRC/VST/euclid-arm32.so" ] || { echo "missing $SRC: run ./build-sdk.sh first" >&2; exit 1; }
VER=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$SRC/plugin.json")
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); sys.exit(0 if m["uid"]=="EqDr" and "Beta" not in m["name"] else "not the release identity")' "$SRC/plugin.json"
out="$here/dist"; mkdir -p "$out"
for pair in armv7:arm32:file32 aarch64:arm64:file64; do
  IFS=: read -r cpu so key <<<"$pair"
  st="$here/build/catalog/$cpu"; rm -rf "${st:?}"; mkdir -p "$st/$DIR"
  cp -r "$SRC/Plugin Skins" "$SRC/version.xml" "$st/$DIR/"
  python3 - "$SRC/plugin.json" "$st/$DIR/plugin.json" "$key" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m.pop("file32", None); m.pop("file64", None)
m[sys.argv[3]] = "euclid.so"
json.dump(m, open(sys.argv[2], "w"), indent=2)
PY
  cp "$SRC/VST/euclid-$so.so" "$st/euclid.so"
  printf '<PLUGIN name="Euclid" descriptiveName="Euclid" format="VST" category="Synth" manufacturer="sd88me" version="%s" file="euclid.so" uid="45714472" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>\n' "$VER" > "$st/entry.xml"
  python3 "$MPC_VST/tools/release.py" --so "$st/euclid.so" --skin "$st/$DIR" --entry "$st/entry.xml" --version "$VER" \
    --user-data euclidierBANK.bin --retire "sd88me - VST - Euclidier" \
    --repo sd88me/mpc-vst-euclid --license MIT --id euclidier \
    --about "Eight-lane Euclidean MIDI sequencer (formerly Euclidier): circle and lane rows with a play-head, tap to draw steps, an ALL view, randomise, drum mode, and a native MPC skin with Q-Links." \
    -o "$st/dist"
  z=$(ls "$st/dist"/*.zip); cp "$z" "$out/Euclid-$VER-mpc-$cpu.zip"
done
python3 "$MPC_VST/tools/sdk_zip.py" "$out/Euclid-$VER-mpc-sdk.zip" "$out/Euclid-$VER-mpc-armv7.zip" "$out/Euclid-$VER-mpc-aarch64.zip"
ls -la "$out"/Euclid-"$VER"-mpc-*.zip
