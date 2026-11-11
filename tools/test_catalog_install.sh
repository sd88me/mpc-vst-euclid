#!/bin/bash
# The catalog zip's own install.sh (the one Plugin Manager runs) on a fake Force that has Euclidier 1.x: the old folder goes (with its
# plugin-list entry), the preset bank moves, another plugin stays, a decoy folder of another plugin with the old name stays.
# Needs dist/Euclid-<ver>-mpc-armv7.zip (tools/package_catalog.sh).   tools/test_catalog_install.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
Z=$(ls "$HERE"/dist/Euclid-*-mpc-armv7.zip 2>/dev/null | tail -n 1)
[ -f "$Z" ] || { echo "no dist/Euclid-*-mpc-armv7.zip: run tools/package_catalog.sh"; exit 1; }
T="$(mktemp -d)"; [ -d "$T" ] || exit 1
trap 'rm -rf "${T:?}"' EXIT
fails=0
chk() { if eval "$1"; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$Z" "$T/pkg"
TOP="$T/pkg/$(ls "$T/pkg" | head -n 1)"
S="$T/synths"; O="$S/sd88me - VST - Euclidier"; mkdir -p "$O" "$S/sd88me - VST - Other"
: > "$O/euclidier.so"; echo bank > "$O/euclidierBANK.bin"; echo '{"id": "euclidier", "name": "Euclidier", "version": "1.0.8"}' > "$O/mpc-plugin.json"
: > "$S/sd88me - VST - Other/other.so"
cat > "$T/MPC.settings" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<PROPERTIES>
  <VALUE name="pluginList-arm">
    <KNOWNPLUGINS>
      <PLUGIN name="Euclidier" descriptiveName="Euclidier" format="VST" category="Synth" manufacturer="sd88me" version="1.0.8" file="$O/euclidier.so" uid="45714472" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>
      <PLUGIN name="Other" descriptiveName="Other" format="VST" category="Synth" manufacturer="sd88me" version="1" file="$S/sd88me - VST - Other/other.so" uid="6f746872" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>
    </KNOWNPLUGINS>
  </VALUE>
</PROPERTIES>
EOF
run() { (cd "$TOP" && MPC_INSTALL_TEST=1 MPC_SETTINGS="$T/MPC.settings" MPC_LEGACY_ROOT="$T/none" sh install.sh -y -t "$S" >"$T/out.txt" 2>&1); }
run; sed 's/^/     /' "$T/out.txt" | tail -n 8
N="$S/sd88me - VST - Euclid"
chk '[ ! -e "$O" ]' "the old Euclidier folder is gone"
chk '[ -f "$N/euclid.so" ] && [ -f "$N/Plugin Skins/TUI.json" ]' "Euclid is installed with its skin"
chk '[ "$(cat "$N/euclidierBANK.bin" 2>/dev/null)" = bank ]' "the 1.x preset bank moved to the new folder"
chk '[ "$(grep -c "uid=\"45714472\"" "$T/MPC.settings")" = 1 ] && grep "uid=\"45714472\"" "$T/MPC.settings" | grep -q "name=\"Euclid\"" && ! grep -q "name=\"Euclidier\"" "$T/MPC.settings"' "one plugin-list entry for the uid, now Euclid"
chk '[ -f "$S/sd88me - VST - Other/other.so" ] && grep -q "name=\"Other\"" "$T/MPC.settings"' "another plugin is untouched"
run; chk '[ "$(grep -c "<PLUGIN" "$T/MPC.settings")" = 2 ] && [ "$(cat "$N/euclidierBANK.bin")" = bank ]' "running it again changes nothing that matters"
# a folder with the old name that belongs to another plugin
mkdir -p "$O"; echo '{"id": "someone-else"}' > "$O/mpc-plugin.json"; : > "$O/x.so"
run; chk '[ -f "$O/x.so" ]' "an old-named folder of another plugin is left alone"
echo; [ $fails = 0 ] && echo "CATALOG INSTALL OK" || { echo "CATALOG INSTALL FAILED ($fails)"; exit 1; }
