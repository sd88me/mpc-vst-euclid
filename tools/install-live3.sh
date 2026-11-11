#!/bin/sh
# Installs Euclid on an MPC Live III or another 64-bit MPC that loads plugin folders from "VST2 Plugins". Run on the device as root:
#   sh install-live3.sh [-y] [-t <VST2 Plugins folder>]
# Copies "VST2 Plugins/sd88me - VST - Euclid" into the folder (default /nvme/VST2 Plugins), REMOVES THE OLD EUCLIDIER folder if it is
# there ("sd88me - VST - Euclidier": same uid, so it would clash), and restarts MPC. Safe to run again. Or just copy the folder yourself:
# then delete an old "sd88me - VST - Euclidier" folder by hand.
# EUCLID_BETA=1 installs "Euclid 2 Beta" and removes nothing.
set -e
cd "$(dirname "$0")"
if [ -n "$EUCLID_BETA" ]; then SKIN='sd88me - VST - Euclid 2 Beta'; SO='euclid2beta-arm64.so'; MIGRATE=0; else SKIN='sd88me - VST - Euclid'; SO='euclid-arm64.so'; MIGRATE=1; fi
OLDSKIN='sd88me - VST - Euclidier'
DEST='/nvme/VST2 Plugins'; YES=0
has_euclidier() { for f in "$1"/euclidier* "$1"/VST/euclidier*; do [ -e "$f" ] && return 0; done; return 1; }   # any file named like 1.x's plugin or engine
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y) YES=1; shift ;;
        -t) [ -n "$2" ] || die "-t needs a folder"; DEST="$2"; shift 2 ;;
        *) die "usage: sh install-live3.sh [-y] [-t <VST2 Plugins folder>]" ;;
    esac
done
case "$DEST" in /?*) ;; *) die "-t must be an absolute path" ;; esac
if [ -z "$MPC_INSTALL_TEST" ]; then
    [ "$(id -u)" = 0 ] || die "run as root"
    case "$(uname -m)" in aarch64) ;; *) die "this package is for 64-bit MPCs (Live III); this device is $(uname -m): use the -armv7 package" ;; esac
    command -v systemctl >/dev/null || die "systemctl not found"
fi
[ -f "VST2 Plugins/$SKIN/VST/$SO" ] || die "this package is damaged: VST2 Plugins/$SKIN/VST/$SO is missing"
mkdir -p "$DEST" || die "cannot create $DEST"
echo "Installing into $DEST/$SKIN"
if [ $MIGRATE = 1 ] && [ -d "$DEST/$OLDSKIN" ]; then echo "  and removing the old Euclidier: $DEST/$OLDSKIN"; fi
if [ $YES = 0 ]; then
    printf "MPC will be restarted. Save your project first. Continue? [y/N] "
    read -r ok; case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi
mpc_service() { if systemctl cat acvs >/dev/null 2>&1; then echo acvs; elif systemctl cat inmusic-mpc >/dev/null 2>&1; then echo inmusic-mpc; else echo acvs; fi; }
mpc_ctl() { if [ -n "$MPC_INSTALL_TEST" ]; then return 0; fi; systemctl "$1" "$(mpc_service)"; }
mpc_ctl stop
trap 'mpc_ctl start' EXIT
NEW="$DEST/$SKIN"; STAGE="$DEST/.$SKIN.new"; OLD="$DEST/.$SKIN.old"
rm -rf "${STAGE:?}" "${OLD:?}"
cp -r "VST2 Plugins/$SKIN" "$STAGE"
if [ -e "$NEW" ]; then mv "$NEW" "$OLD"; fi
mv "$STAGE" "$NEW"
rm -rf "${OLD:?}"
if [ $MIGRATE = 1 ] && [ -d "$DEST/$OLDSKIN" ]; then
    if has_euclidier "$DEST/$OLDSKIN"; then
        # the 1.x preset bank is the user's: it moves to the new folder
        [ -f "$DEST/$OLDSKIN/euclidierBANK.bin" ] && [ ! -f "$NEW/euclidierBANK.bin" ] && cp "$DEST/$OLDSKIN/euclidierBANK.bin" "$NEW/euclidierBANK.bin" && echo "kept the preset bank"
        rm -rf "${DEST:?}/${OLDSKIN:?}" && echo "removed the old Euclidier"
    else
        echo "left $DEST/$OLDSKIN alone (no Euclidier files in it)"
    fi
fi
sync
echo "Done. Starting MPC. Add 'Euclid' from External Plugins. Its MIDI output port is called 'Euclid': a track that took input from 'Euclidier' needs its input set again."
