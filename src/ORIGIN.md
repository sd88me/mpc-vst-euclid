# Where the sequencer comes from

The sequencer started as [intelliriffer/EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar. It was
reworked for the Akai Force in `sd88me/force-euclidier` (now archived), then copied into this repo from that repo's commit
`1268b0e1f0f55e963c754891d94cc0055bee4cb1` (2026-09-25). Up to Euclidier 1.x the plugin ran that standalone program as a child process.

What is left of it here:

- `bjlund.cpp`, `bjlund.h`: the Bjorklund pattern generator, unchanged. Compiled into the plugin (`src/core/euclid_core.cpp` builds a
  table of every pattern from it once).
- `eqseq.cpp`, `eqseq.h`, `commontypes.h`: the original lane class and preset structs, unchanged. The plugin does not use the class: its
  logic is ported to `core/euclid_core.h`, and `tests/core_equiv.cpp` steps both with the same values and ticks and compares every
  note (`commontypes.h` is used, for the preset bank format). They stay as the test's reference.
- The rest of the original program (its console display, MIDI handler, control socket, preset code, the RtMidi library) is no longer
  built. The parts that mattered are ported into the core: the value limits, the randomiser, played notes transposing NOTE-mode lanes,
  the preset bank format. It remains in the git history (the 1.x releases).

Licence: see the note in the top-level `README.md`.
