# Euclid

💬 Questions or feedback? Join the [Open MPC Discord](https://discord.gg/sRRysZSgu3).

Euclid (formerly Euclidier) is an 8-lane Euclidean MIDI sequencer for Akai MPC OS standalone devices (Force, MPC Live / One / X / Key,
Live III), with its own MPC screen skin and Q-Links. It makes no sound: it plays MIDI into other tracks, and follows the project
transport and tempo, so edits stay in phase with the beat. The sequencer is based on
[intelliriffer/EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar (see [credits](#credits-and-licence)).

<img width="320" height="200" alt="2026-10-03T124201676Z" src="https://github.com/user-attachments/assets/94c88fd0-436b-4d1a-8aa4-cb8a376adb89" />
<img width="320" height="200" alt="2026-10-03T124157924Z" src="https://github.com/user-attachments/assets/708f8f74-1ec5-4dd7-bf24-7a0d2a1bbebe" />

## What it does

Eight independent lanes each spread a number of hits (**fill**) as evenly as possible over a number of **steps**, with a **shift**
(rotation), an optional **loop** point (shorter than the steps for a short pattern, longer for a polyrhythm), a note or drum note,
MIDI channel, gate, velocity, humanise, and a time division. Tap a step to turn it on or off by hand.

A new instance starts with every lane in drum mode, lanes 1-4 on and 5-8 off, each of the four with its own Euclidean pattern: 16 steps
with 4 hits (four on the floor), 8 with 3 (tresillo), 12 with 5, and 16 with 5. A saved project restores its own settings instead.

## The screen

Two tabs, each with its own Q-Link page. DRUM MODE ALL and RANDOMISE ALL are in the top bar of both.

| Tab | What is on it |
|---|---|
| **LANES** | Lane keys **1-8 and ALL**. The lane you pick shows as a circle of dots with the play-head (tap a dot to flip that step), its FILL / STEPS, a RANDOMISE button, and its twelve controls in three blocks of four, one block per Q-Link group: STEPS, FILL, SHIFT, LOOP / VELOCITY, HUMANIZE, GATE, NOTE / DIV, MIDI CH, LANE ON, MODE (NOTE or DRUM, under the NOTE knob). |
| **ALL** | All 8 lanes as rows: an enable lamp, the pattern as a row of cells with the play-head (tap a cell to flip that step), the fill / steps, and a RANDOMISE button per lane. Q-Links 1-8 are the lane enables. |

**ALL on the LANES tab** edits every lane at once. Only the controls that make sense for that remain: FILL, VELOCITY, HUMANIZE and GATE.
They are relative: the knob shows the mean of the eight lanes, and turning it moves every lane by the same amount (a lane stops at its own
limit). The circle shows the longest pattern, with its play-head. RANDOMISE then randomises every lane.

**Drawing steps.** A tap flips one step. The pattern is then hand-made: it is kept, played and saved with the project until the lane's
STEPS, FILL or SHIFT change, or it is randomised, which build the pattern again. A hand-made pattern survives changes to DIV, gate, velocity and the rest.

**DRUM MODE ALL** puts every lane in DRUM mode and gives lane 1 to 8 the notes 36 to 43 (also a lane that was already in DRUM mode).
Switching one lane to DRUM sets its note to its own slot in the same way (lane 1 is 36, lane 2 is 37, and so on); the NOTE knob follows.

Pattern displays change size with the lane's step count: the row cells and the circle come in size classes (rows: 8, 16, 32 cells; circle:
8, 12, 16, 24, 32 dots). Counts between classes use the next class up. The 32-step row doesn't show the play-head.

## Limits

- **32 steps.** STEPS, FILL, SHIFT and LOOP stop at 32 (the sequencer allows 64). The step displays are built from many small images, and 64
  steps made pages too slow to open.
- **Slow first draw.** A page can take a moment to fill in when the plugin is first inserted, while MPC decodes the skin's images.
- **No preset page.** The 128 preset slots of the original are not on the screen. The plugin's settings are saved with the MPC project like
  any other plugin.
- **Notes arrive within one audio block** of their step (about 3 ms at 128 frames), because MPC does not take a plugin's own MIDI output
  and the notes go out through a MIDI port instead.

## Setting it up

1. Download the zip for your device from the Releases page: `Euclid-<version>-VST2-Plugins-aarch64.zip` (Live III and other 64-bit MPCs)
   or `-armv7.zip` (Force and other 32-bit MPCs). Each holds the plugin, its skin and an installer; follow the zip's `INSTALL.txt`
   (copy it to the device, run the installer as root: it stops MPC, so save your project first). A rooted device is required.
   **The installer removes an old Euclidier (1.x) for you:** its folder, its engine program and its plugin-list entry. The plugin id
   is the same, so saved projects keep working, and your 1.x preset bank is moved across.
2. Insert Euclid on a track (it is an instrument, with no sound).
3. Euclid appears as a MIDI port named **Euclid**. MPC lists it as a MIDI input: set the MIDI input of the drum or synth tracks that
   should play it to that port. (Euclidier's port was called Euclidier: **a track routed from the old port needs its input set again.**)
4. Press play: Euclid follows the project's transport, position, loop and tempo.

## How it works

- `src/core/euclid_core.h` is the sequencer: eight lanes stepped from a 24-ticks-per-quarter clock; the step is a pure function of the
  tick, so editing never needs a resync. Note lengths are counted in samples. It is a library with no threads, I/O or allocation.
- `src/sdk/EuclidierPlugin.cpp` is the plugin. The audio thread owns the core: each block it applies the commands the UI
  queued, turns the host's song position into ticks (`vst/transport_grid.h`: ticks of the block are sent at its start, cut at the loop
  end, and re-anchored to the song position at Start, at every loop wrap or locate and once a bar), steps the core, writes the block's
  notes to the plugin's ALSA sequencer port, and publishes a snapshot of every value. A worker thread reads the snapshot to keep the
  parameters, the step displays and the play-head current.
- The parameters are the 669 of Euclidier 1.x, in the same order (`vst/params.lock`), with the new ones appended (`vst/params.json`): the
  lane keys, each lane's hand-made pattern, the touch areas. Saved projects bind to the index, so the order never changes.
- The step displays are display-only parameters, one per cell per size class; the skin shows one image per state. Taps go through
  separate touch areas (32 slices per lane row, 32 sectors round the circle) that never overlap, as MPC gives a tap to the topmost button.
- `tools/make_tui.py` draws the skin (`TUI.json`, `Q-Links.json`, the images) from code.

## Building and testing

```
./build-sdk.sh test    # the offline tests, then the host simulator
./build-sdk.sh beta    # "Euclid 2 Beta" (its own id), to install next to the release for testing
tools/package.sh       # the release zips in dist/ (release identity)
tools/package_catalog.sh  # the catalog zips (-mpc-armv7, -mpc-aarch64): what Plugin Manager installs; needs mpc-vst-plugins with release.py --retire
tools/test_installers.sh   # the installers against a fake device
tools/test_catalog_install.sh   # the catalog zip's installer against a fake Force that has Euclidier 1.x
```

Needs Docker and Akai's MPC plugin builder (`MPC_SDK` points at it). The tests (`tests/run.sh`) run under ASan/UBSan: `core_equiv` steps the core against
the original sequencer's lane class (1.5 million ticks, identical notes) and checks note bookkeeping; `host_test` drives the plugin through
a fake host, and checks every note against the song position through loops, tempo changes, locates, stops and edits; `grid_test` checks the clock.

## Files

| Path | What |
|---|---|
| `src/core/` | the sequencer core |
| `src/sdk/EuclidierPlugin.cpp`, `params_gen.h` | the plugin and its parameter table (generated by `tools/gen_sdk_params.py`) |
| `src/bjlund.*`, `src/eqseq.*`, `src/commontypes.h` | the original Bjorklund generator (used) and lane class (the test's reference); `src/ORIGIN.md` says where they came from |
| `vst/params.json`, `vst/params.lock` | the parameter list (edit by appending only) and the frozen 1.x order it must start with (`tools/gen_sdk_params.py` checks it) |
| `vst/transport_grid.h`, `vst/grid_test.cpp` | the song-position clock and its test |
| `tools/make_tui.py` | the skin generator |
| `tools/install-force.sh`, `install-live3.sh`, `plugin_list.awk`, `package.py` | the one-off installers in the plain zips (they remove the old Euclidier) |
| `tools/package_catalog.sh` | the catalog zips; their installer removes the old Euclidier through `release.py --retire` |
| `docs/progressions/` | chord-pad progression files from the original, with their own readme (not used by the plugin) |

## Credits and licence

- The sequencer is [EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar
  ([intelliriffer](https://github.com/intelliriffer)): the Bjorklund generator is his code, unchanged, and the lane logic in
  `src/core` is a port of his lane class. The chord progressions come from it too.
- The Force rework (stateless step position), the in-plugin core, the plugin, the skin and the installers are by
  [sd88me](https://github.com/sd88me).

MIT, see [LICENSE](LICENSE). That covers sd88me's own work. Upstream publishes no licence file, so code inherited from
`intelliriffer/EUCLIDIER-CONSOLE` stays subject to whatever terms its author intends, once clarified.
