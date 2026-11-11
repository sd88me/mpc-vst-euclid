#!/usr/bin/env python3
"""Packs the release zips from build/sdk/"sd88me - VST - Euclid" (made by ./build-sdk.sh, release identity) into dist/:
  dist/Euclid-<ver>-VST2-Plugins-aarch64.zip   VST2 Plugins/<folder>/ + install-live3.sh (Live III and other 64-bit MPCs)
  dist/Euclid-<ver>-VST2-Plugins-armv7.zip     VST2 Plugins/<folder>/ + install-force.sh (Force and other 32-bit MPCs)
Both installers remove the old Euclidier (1.x). Refuses a beta build."""
import json, os, shutil, stat, sys, tempfile, zipfile
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
VER = json.load(open(os.path.join(ROOT, "plugin.json")))["version"]
NAME = "sd88me - VST - Euclid"
DIST = os.path.join(ROOT, "dist")
DATE = (2026, 11, 11, 0, 0, 0)

TEXT = {
"aarch64": """Euclid %s for MPC Live III and other 64-bit (aarch64) MPCs

Euclid replaces Euclidier (1.x). Saved projects keep working: it has the same plugin id.

Easiest (root shell on the device):
   scp -r <this unzipped folder> root@<device-ip>:/tmp/euclid
   ssh root@<device-ip> sh /tmp/euclid/install-live3.sh
   This removes the old Euclidier folder, installs Euclid into /nvme/VST2 Plugins and restarts the MPC (save your project first).

By hand: copy the folder "VST2 Plugins/%s" into a folder named exactly "VST2 Plugins" at the root of a drive the MPC can see,
DELETE an old "sd88me - VST - Euclidier" folder from there, make sure Preferences > Enhancements > VST2 Support is on, restart the MPC.
Then add "Euclid" to a track from External Plugins.

Euclid plays through a MIDI output port named Euclid (Euclidier's was called Euclidier): set a drum or synth track's MIDI input to it.
More: https://github.com/sd88me/mpc-vst-euclid
""" % (VER, NAME),
"armv7": """Euclid %s for Force and other 32-bit (armv7) MPCs

Euclid replaces Euclidier (1.x). Saved projects keep working: it has the same plugin id.

A) Force and MPC OS that lists plugins in MPC.settings (root shell on the device):
   scp -r <this unzipped folder> root@<device-ip>:/tmp/euclid
   ssh root@<device-ip> sh /tmp/euclid/install-force.sh
   This stops the MPC (save your project first), removes the old Euclidier (folder, engine program, plugin-list entry), copies Euclid
   to /sdcard/Synths, backs up and edits MPC.settings, and restarts the MPC.

B) MPC OS that loads plugin folders from "VST2 Plugins": copy "VST2 Plugins/%s" into a folder named exactly "VST2 Plugins" at the
   root of a drive, delete an old "sd88me - VST - Euclidier" folder, enable VST2 Support (Preferences > Enhancements), restart.

Then add "Euclid" to a track from the plugin browser.
Euclid plays through a MIDI output port named Euclid (Euclidier's was called Euclidier): set a drum or synth track's MIDI input to it.
More: https://github.com/sd88me/mpc-vst-euclid
""" % (VER, NAME),
}


def add_tree(z, src, arc):
    for d, _, files in os.walk(src):
        for f in sorted(files):
            full = os.path.join(d, f)
            zi = zipfile.ZipInfo(os.path.join(arc, os.path.relpath(full, src)).replace(os.sep, "/"), DATE)
            zi.external_attr = (stat.S_IFREG | (0o755 if stat.S_IMODE(os.stat(full).st_mode) & 0o100 else 0o644)) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, open(full, "rb").read())


def add_file(z, path, arc, mode=0o644):
    zi = zipfile.ZipInfo(arc, DATE)
    zi.external_attr = (stat.S_IFREG | mode) << 16
    zi.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(zi, open(path, "rb").read() if os.path.isfile(path) else path.encode())


def main():
    os.makedirs(DIST, exist_ok=True)
    full_src = os.path.join(ROOT, "build", "sdk", NAME)
    if not os.path.isdir(full_src):
        sys.exit("missing %s: run ./build-sdk.sh first" % full_src)
    for cpu in ("aarch64", "armv7"):
        tmp = tempfile.mkdtemp()
        src = os.path.join(tmp, NAME)
        shutil.copytree(full_src, src)   # one build folder holds both binaries; each zip keeps only its CPU's
        drop, other = ("file32", "arm32") if cpu == "aarch64" else ("file64", "arm64")
        meta = json.load(open(os.path.join(src, "plugin.json")))
        if "beta" in meta["name"].lower():
            sys.exit("%s is a beta build" % full_src)
        meta.pop(drop, None)
        json.dump(meta, open(os.path.join(src, "plugin.json"), "w"), indent=2)
        for f in os.listdir(os.path.join(src, "VST")):
            if other in f:
                os.remove(os.path.join(src, "VST", f))
        out = os.path.join(DIST, "Euclid-%s-VST2-Plugins-%s.zip" % (VER, cpu))
        with zipfile.ZipFile(out, "w") as z:
            add_tree(z, src, "VST2 Plugins/" + NAME)
            add_file(z, os.path.join(ROOT, "LICENSE"), "VST2 Plugins/%s/LICENSE" % NAME)
            add_file(z, TEXT[cpu], "INSTALL.txt")
            if cpu == "armv7":
                add_file(z, os.path.join(ROOT, "tools", "install-force.sh"), "install-force.sh", 0o755)
                add_file(z, os.path.join(ROOT, "tools", "plugin_list.awk"), "plugin_list.awk")
            else:
                add_file(z, os.path.join(ROOT, "tools", "install-live3.sh"), "install-live3.sh", 0o755)
        shutil.rmtree(tmp)
        print("wrote", os.path.normpath(out), "%.1f MB" % (os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
