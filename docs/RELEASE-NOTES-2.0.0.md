# Euclid 2.0.0

Euclidier is now **Euclid**. Same plugin id, so saved projects still find it.

## New
- **ALL view** on the LANES tab: FILL, VELOCITY, HUMANIZE and GATE change all eight lanes together, relative to each lane's own value. The circle shows the longest pattern.
- **Tap to draw steps** on the circle and on the lane rows. A hand-made pattern is kept and saved with the project until STEPS, FILL or SHIFT change, or you randomise.
- **DRUM MODE ALL** gives lanes 1-8 the notes 36-43.
- **MIDI CH** is a stepper. NOTE is last in its row, MODE sits under it, and the knobs have the orange highlight.
- Randomise now gives a fill from 1 up to half the new step count.

## Better timing
- The sequencer now runs inside the plugin, in step with the project transport (loops, locates, tempo changes), instead of as a separate program.
- Note lengths are exact, and every note-on has its note-off.

## Changed
- **The MIDI port is now called "Euclid".** A track routed from the old "Euclidier" port needs its MIDI input set again.
- Steps, fill, shift and loop go up to 32 (they were 64).
- The 128-slot preset page is not on the screen. Your 1.x preset bank file is kept.

## Installing
- Plugin Manager: updates Euclidier in place; the old folder is removed and the preset bank moved across.
- Or use the zip for your device (`-aarch64` for Live III, `-armv7` for Force) and follow `INSTALL.txt`. A rooted device is required.
