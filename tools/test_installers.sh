#!/bin/bash
# Offline test of install-force.sh and install-live3.sh against a fake device (no MPC, no root): MPC_INSTALL_TEST=1 skips the root,
# CPU and systemctl checks. The old Euclidier (1.x) must go, with its plugin-list entry; everything else must stay; running twice
# changes nothing; the beta installs next to the release and removes nothing.
#   tools/test_installers.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
[ -n "$T" ] && [ -d "$T" ] || { echo "no temp dir"; exit 1; }
trap 'rm -rf "${T:?}"' EXIT
fails=0
check() { if [ "$1" = ok ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }
yes_() { if eval "$1"; then check ok "$2"; else check no "$2"; fi; }

# ---- the package, as package.py lays it out
PKG="$T/pkg"; mkdir -p "$PKG"
for n in "Euclid" "Euclid 2 Beta"; do
    s=$(echo "$n" | tr 'A-Z ' 'a-z' | tr -d ' ')
    d="$PKG/VST2 Plugins/sd88me - VST - $n"; mkdir -p "$d/VST" "$d/Plugin Skins"
    echo '{}' > "$d/plugin.json"; echo skin > "$d/Plugin Skins/TUI.json"
    [ "$n" = Euclid ] && s=euclid || s=euclid2beta
    : > "$d/VST/$s-arm32.so"; : > "$d/VST/$s-arm64.so"
done
cp "$HERE/install-force.sh" "$HERE/install-live3.sh" "$HERE/plugin_list.awk" "$PKG/"

# ---- Force
S="$T/synths"; mkdir -p "$S/sd88me - VST - Euclidier" "$S/sd88me - VST - Other" "$S/sd88me - VST - Euclid 2 Beta"
: > "$S/sd88me - VST - Euclidier/euclidier.so"; : > "$S/sd88me - VST - Euclidier/euclidier"; echo old > "$S/sd88me - VST - Euclidier/skin.json"; echo bank1 > "$S/sd88me - VST - Euclidier/euclidierBANK.bin"
: > "$S/sd88me - VST - Other/other.so"
cat > "$T/MPC.settings" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<PROPERTIES>
  <VALUE name="pluginList-arm">
    <KNOWNPLUGINS>
      <PLUGIN name="Euclidier" descriptiveName="Euclidier" format="VST" category="Synth" manufacturer="sd88me" version="1.0.8" file="$S/sd88me - VST - Euclidier/euclidier.so" uid="45714472" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>
      <PLUGIN name="Other" descriptiveName="Other" format="VST" category="Synth" manufacturer="sd88me" version="1" file="$S/sd88me - VST - Other/other.so" uid="6f746872" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>
    </KNOWNPLUGINS>
  </VALUE>
</PROPERTIES>
EOF
run_force() { (cd "$PKG" && MPC_INSTALL_TEST=1 MPC_SETTINGS="$T/MPC.settings" "$@" sh install-force.sh -y -t "$S" >"$T/out.txt" 2>&1); }

# the beta first: next to everything, removing nothing
(cd "$PKG" && EUCLID_BETA=1 MPC_INSTALL_TEST=1 MPC_SETTINGS="$T/MPC.settings" sh install-force.sh -y -t "$S" >"$T/out.txt" 2>&1)
yes_ '[ -d "$S/sd88me - VST - Euclidier" ] && grep -q "uid=\"45714472\"" "$T/MPC.settings"' "beta: the old Euclidier is untouched"
yes_ '[ "$(grep -c "uid=\"45714442\"" "$T/MPC.settings")" = 1 ] && [ -f "$S/sd88me - VST - Euclid 2 Beta/VST/euclid2beta-arm32.so" ]' "beta: installed with its own uid"

run_force env
cat "$T/out.txt" | sed 's/^/     /'
yes_ '[ ! -e "$S/sd88me - VST - Euclidier" ]' "release: the old Euclidier folder (and its engine) is gone"
yes_ '[ "$(cat "$S/sd88me - VST - Euclid/euclidierBANK.bin" 2>/dev/null)" = bank1 ]' "release: the 1.x preset bank moved to the new folder"
yes_ '[ -f "$S/sd88me - VST - Euclid/VST/euclid-arm32.so" ] && [ -f "$S/sd88me - VST - Euclid/Plugin Skins/TUI.json" ]' "release: Euclid is installed with its skin"
yes_ '[ "$(grep -c "uid=\"45714472\"" "$T/MPC.settings")" = 1 ] && grep "uid=\"45714472\"" "$T/MPC.settings" | grep -q "name=\"Euclid\"" && ! grep -q "name=\"Euclidier\"" "$T/MPC.settings"' "release: one entry for the uid, now called Euclid"
yes_ 'grep "uid=\"45714472\"" "$T/MPC.settings" | grep -q "file=\"$S/sd88me - VST - Euclid/VST/euclid-arm32.so\""' "release: the entry points at the new file"
yes_ '[ -f "$S/sd88me - VST - Other/other.so" ] && grep -q "name=\"Other\"" "$T/MPC.settings"' "release: another plugin and its entry are untouched"
yes_ '[ -f "$S/sd88me - VST - Euclid 2 Beta/VST/euclid2beta-arm32.so" ] && grep -q "uid=\"45714442\"" "$T/MPC.settings"' "release: a beta beside it is untouched"
yes_ 'ls "$T"/MPC.settings.bak-euclid-* >/dev/null 2>&1' "release: MPC.settings was backed up"
cp "$T/MPC.settings" "$T/after1"
run_force env
yes_ 'cmp -s "$T/MPC.settings" "$T/after1" || [ "$(grep -c "<PLUGIN" "$T/MPC.settings")" = 3 ]' "release: running it again changes nothing that matters (3 entries)"

# a folder with the old name that holds none of Euclidier's files is left alone
mkdir -p "$S/sd88me - VST - Euclidier"; echo keep > "$S/sd88me - VST - Euclidier/notes.txt"
run_force env
yes_ '[ -f "$S/sd88me - VST - Euclidier/notes.txt" ]' "release: a folder named Euclidier without Euclidier's files is left alone"

# ---- Live III
V="$T/vst2"; mkdir -p "$V/sd88me - VST - Euclidier/VST" "$V/sd88me - VST - Other"
: > "$V/sd88me - VST - Euclidier/VST/euclidier-arm64.so"; echo bank2 > "$V/sd88me - VST - Euclidier/euclidierBANK.bin"; : > "$V/sd88me - VST - Other/o.so"
(cd "$PKG" && MPC_INSTALL_TEST=1 sh install-live3.sh -y -t "$V" >"$T/out.txt" 2>&1)
sed 's/^/     /' "$T/out.txt"
yes_ '[ "$(cat "$V/sd88me - VST - Euclid/euclidierBANK.bin" 2>/dev/null)" = bank2 ]' "live3: the 1.x preset bank moved too"
yes_ '[ ! -e "$V/sd88me - VST - Euclidier" ] && [ -f "$V/sd88me - VST - Euclid/VST/euclid-arm64.so" ] && [ -f "$V/sd88me - VST - Other/o.so" ]' "live3: old Euclidier removed, Euclid installed, the rest untouched"
(cd "$PKG" && MPC_INSTALL_TEST=1 sh install-live3.sh -y -t "$V" >"$T/out.txt" 2>&1)
yes_ '[ -f "$V/sd88me - VST - Euclid/VST/euclid-arm64.so" ]' "live3: running it again is fine"
(cd "$PKG" && EUCLID_BETA=1 MPC_INSTALL_TEST=1 sh install-live3.sh -y -t "$V" >"$T/out.txt" 2>&1)
yes_ '[ -f "$V/sd88me - VST - Euclid 2 Beta/VST/euclid2beta-arm64.so" ] && [ -f "$V/sd88me - VST - Euclid/VST/euclid-arm64.so" ]' "live3: the beta installs next to the release"

echo
[ $fails = 0 ] && echo "INSTALLERS OK" || { echo "INSTALLERS FAILED ($fails)"; exit 1; }
