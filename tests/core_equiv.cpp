// The in-process core (src/core/euclid_core.h) against the original lane class (src/eqseq.cpp, the 1.x engine): the same lane
// values and the same clock ticks must give the same note-ons at the same ticks. Then the core's own guarantees: every note-on
// gets a note-off, the off comes before the next on of that lane, Stop releases everything, nothing but notes goes out.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "eqseq.h"
#include "../src/core/euclid_core.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static unsigned rs = 12345;
static int rnd(int lo, int hi) { rs = rs * 1103515245u + 12345u; return lo + (int)((rs >> 8) % (unsigned)(hi - lo + 1)); }

int main()
{
    // ---- 1. lane equivalence: random lanes, 4 bars of ticks each ----
    long compared = 0, ons = 0;
    for (int trial = 0; trial < 4000; trial++)
    {
        const int steps = rnd(2, 64), fill = rnd(1, 64), shift = rnd(0, 64), loop = rnd(0, 3) ? 0 : rnd(1, 64), div = rnd(0, 10);
        const int mode = rnd(1, 2), note = rnd(0, 127), ch = rnd(1, 15), vel = rnd(1, 127);
        EQSEQ old;
        RtMidiOut port;
        old.setPORT(&port);
        old.steps = steps; old.pulses = fill; old.shift = shift; old.loop = loop; old.div = div == 0 ? 1 : div;
        old.mode = mode; old.note = note; old.ch = ch; old.vel = vel; old.velh = 0; old.enabled = true;
        old.updateSeq();
        old.setBPM(120);

        euclid::Core core;
        for (int i = 1; i < 8; i++) core.set(i, euclid::P_ENABLE, 0);
        core.set(0, euclid::P_MODE, 0);              // NOTE first, so that entering DRUM below is seen as a change
        core.set(0, euclid::P_STEPS, steps); core.set(0, euclid::P_FILL, fill); core.set(0, euclid::P_SHIFT, shift);
        core.set(0, euclid::P_LOOP, loop); core.set(0, euclid::P_DIV, div); core.set(0, euclid::P_CH, ch);
        core.set(0, euclid::P_VEL, vel); core.set(0, euclid::P_VELH, 0);
        core.set(0, euclid::P_MODE, mode == 2);
        core.set(0, euclid::P_NOTE, note);           // after the mode: DRUM picks its own note
        core.set(0, euclid::P_ENABLE, 1);
        core.outN = 0;
        core.beginBlock(0, 64, 120, 44100);
        core.start();
        const long start = rnd(0, 3) ? 0 : rnd(0, 500) * 6;   // sometimes from a song position
        if (start) core.songpos(start / 6);
        for (long t = start; t < start + 4 * 96; t++)
        {
            port.sent.clear();
            old.tick(t, 1000000);
            core.outN = 0;
            core.beginBlock((t - start) * 900, 900, 120, 44100);   // one tick per 900-sample block (about 20 ms at 120 BPM)
            const int offs = core.outN;
            core.pulse();
            std::vector<euclid::MidiOut> newOn;
            for (int k = offs; k < core.outN; k++) if ((core.out[k].status & 0xF0) == 0x90) newOn.push_back(core.out[k]);
            std::vector<std::vector<unsigned char> > oldOn;
            for (auto& m : port.sent) if ((m[0] & 0xF0) == 0x90) oldOn.push_back(m);
            compared++;
            ons += (long)oldOn.size();
            if (oldOn.size() != newOn.size()) { CHECK(0, "trial %d tick %ld: %zu note-ons from the engine lane, %zu from the core (steps %d fill %d shift %d loop %d div %d)", trial, t, oldOn.size(), newOn.size(), steps, fill, shift, loop, div); break; }
            if (!oldOn.empty() && (oldOn[0][0] != newOn[0].status || oldOn[0][1] != newOn[0].d1 || oldOn[0][2] != newOn[0].d2))
            { CHECK(0, "trial %d tick %ld: engine %02x %d %d, core %02x %d %d", trial, t, oldOn[0][0], oldOn[0][1], oldOn[0][2], newOn[0].status, newOn[0].d1, newOn[0].d2); break; }
            if (old.playStep() != core.lane[0].step && (t % (96 / old.getDiv()) == 0)) { CHECK(0, "trial %d tick %ld: play-head %d vs %d", trial, t, old.playStep(), core.lane[0].step); break; }
        }
        if (fails > 5) break;
    }
    printf("lane equivalence: %ld ticks compared, %ld note-ons, %d differences\n", compared, ons, fails);

    // ---- 2. note bookkeeping over a long run with edits, tempo changes, stops and locates ----
    {
        euclid::Core core;
        core.seed(7);
        for (int i = 0; i < 8; i++) { core.set(i, euclid::P_ENABLE, 1); core.set(i, euclid::P_MODE, i % 2); core.set(i, euclid::P_CH, 1 + i); }
        std::map<int, int> open;   // (ch << 8 | note) -> sounding count
        long onN = 0, offN = 0, other = 0, overlap = 0, orphan = 0;
        int64_t now = 0;
        double bpm = 120;
        bool playing = false;
        long tick = 0;
        double acc = 0;
        auto drain = [&] {
            for (int k = 0; k < core.outN; k++)
            {
                const euclid::MidiOut& e = core.out[k];
                const int key = ((e.status & 15) << 8) | e.d1;
                if ((e.status & 0xF0) == 0x90) { onN++; if (open[key]++ > 0) overlap++; }
                else if ((e.status & 0xF0) == 0x80) { offN++; if (open[key]-- <= 0) { orphan++; open[key] = 0; } }
                else other++;
            }
            core.outN = 0;
        };
        for (long blk = 0; blk < 400000; blk++)
        {
            const int frames = 128;
            core.beginBlock(now, frames, bpm, 44100);
            if (!playing && rnd(0, 400) == 0) { playing = true; core.start(); tick = rnd(0, 2) ? 0 : rnd(0, 64) * 6; if (tick) core.songpos(tick / 6); acc = 0; }
            else if (playing && rnd(0, 3000) == 0) { playing = false; core.stop(); drain(); long snd = 0; for (auto& kv : open) snd += kv.second; CHECK(snd == 0 && core.sounding() == 0, "notes left sounding after Stop: %ld", snd); }
            if (playing)
            {
                if (rnd(0, 2500) == 0) { tick = rnd(0, 64) * 6; core.songpos(tick / 6); }   // loop wrap / locate
                acc += frames * bpm * 24.0 / (60.0 * 44100.0);
                while (acc >= 1.0) { core.pulse(); tick++; acc -= 1.0; }
            }
            if (rnd(0, 40) == 0)   // an edit under the playing notes
            {
                int p = rnd(0, euclid::P_COUNT - 1);
                if (p == euclid::P_CH) p = euclid::P_GATE;   // channels stay one per lane here, so a (channel, note) is one lane; see 4.
                const int l = rnd(0, 7);
                static const int hi[euclid::P_COUNT] = {1, 127, 10, 32, 32, 32, 95, 15, 127, 50, 32, 1};
                core.set(l, p, rnd(0, hi[p]));
                if (p == euclid::P_ENABLE && rnd(0, 1)) core.set(l, p, 1);
            }
            if (rnd(0, 900) == 0) core.randomize(rnd(0, 7));
            if (rnd(0, 5000) == 0) { EUPATCH E = core.patch(); core.applyPatch(E); }
            if (rnd(0, 2000) == 0) bpm = rnd(40, 300);
            drain();
            now += frames;
        }
        if (playing) { core.stop(); drain(); }
        long snd = 0;
        for (auto& kv : open) snd += kv.second;
        printf("bookkeeping: %ld note-ons, %ld note-offs, %ld other messages, %ld overlapping ons, %ld orphan offs, %ld left sounding\n", onN, offN, other, overlap, orphan, snd);
        CHECK(onN > 10000, "the run played notes");
        CHECK(onN == offN, "every note-on has a note-off");
        CHECK(other == 0, "nothing but notes goes out in NOTE / DRUM mode");
        CHECK(overlap == 0, "a lane's note ends before its next begins");
        CHECK(orphan == 0, "no note-off without a note-on");
        CHECK(snd == 0, "nothing left sounding");
    }

    // ---- 3. gate length in samples: 1/16 at 120 BPM, gate 50% -> 62.5 ms = 2756 samples, off sent with the block that holds it ----
    {
        euclid::Core core;
        for (int i = 1; i < 8; i++) core.set(i, euclid::P_ENABLE, 0);
        core.set(0, euclid::P_GATE, 50);
        core.outN = 0;
        core.beginBlock(0, 128, 120, 44100);
        core.start();
        core.pulse();
        CHECK(core.outN == 1 && (core.out[0].status & 0xF0) == 0x90, "the first tick plays lane 1");
        core.outN = 0;
        int64_t now = 128, offAt = -1;
        for (; now < 6000 && offAt < 0; now += 128) { core.beginBlock(now, 128, 120, 44100); if (core.outN) offAt = now; }
        CHECK(offAt >= 0 && offAt <= 2756 && offAt + 128 > 2756, "note-off goes out with the block containing sample 2756 (block at %lld)", (long long)offAt);
    }
    // ---- 4. a channel change under a sounding note: the note-off goes to the channel the note-on went to ----
    {
        euclid::Core core;
        for (int i = 1; i < 8; i++) core.set(i, euclid::P_ENABLE, 0);
        core.set(0, euclid::P_CH, 3);
        core.outN = 0;
        core.beginBlock(0, 128, 120, 44100);
        core.start();
        core.pulse();
        CHECK(core.outN == 1 && core.out[0].status == 0x92, "note-on on channel 3");
        core.outN = 0;
        core.set(0, euclid::P_CH, 7);
        CHECK(core.outN == 1 && core.out[0].status == 0x82, "the channel change releases the note on channel 3");
    }
    printf(fails ? "CORE FAILED (%d)\n" : "CORE OK\n", fails);
    return fails ? 1 : 0;
}
