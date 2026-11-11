// euclid_core.h -- the Euclidier sequencer as a library: eight Euclidean lanes stepped from a 24-PPQN tick, MIDI out through a
// callback-free event list. It is the standalone engine (src/euclidier.cpp + src/eqseq.cpp, by Amit Talwar) with the process,
// the ports, the threads and the globals taken away; the lane logic and every value's limits are the engine's, so a project
// saved by the 1.x plugin plays the same pattern. tests/core_equiv.cpp steps it against the original EQSEQ class.
//
// One thread owns a core (the audio thread, in the plugin). Nothing here allocates, locks, sleeps or does I/O after the
// pattern table is built (once per process, by the first constructor).
//
// What differs from the engine, on purpose:
//   * time is the caller's: sample counts for note lengths, not the wall clock. A note-off is due at a sample position and goes
//     out with the block that contains it, before that block's note-ons.
//   * a hanging note is ended with a note-off. The engine sent a controller (0xB0, the note number as controller) for a lane in
//     DRUM mode, which is the default mode: the note was never released and the track received a stray controller.
//   * a lane that fires while its last note is still sounding ends that note first.
//   * no controller feedback on channel 16 (it was for a hardware controller; in a plugin it only reached the driven track).
//   * randomise uses the core's own generator, not rand().
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "commontypes.h"

namespace euclid {

enum LaneParam { P_ENABLE = 0, P_NOTE, P_DIV, P_STEPS, P_FILL, P_SHIFT, P_GATE, P_CH, P_VEL, P_VELH, P_LOOP, P_MODE, P_COUNT };
inline const char* laneParamName(int p)
{
    static const char* N[P_COUNT] = {"enable", "note", "div", "steps", "fill", "shift", "gate", "ch", "vel", "velh", "loop", "mode"};
    return p >= 0 && p < P_COUNT ? N[p] : "";
}

struct MidiOut { uint8_t status, d1, d2; };

inline int limit(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// bjlund(fill, steps) for steps 2..64 and fill 1..64, as bits. The generator keeps its work in globals, so it is only run here,
// once, before any core plays.
struct PatternTable
{
    uint64_t bits[65][65];   // [steps][fill], bit i = step i
    static const PatternTable& get();   // euclid_core.cpp
};

struct Lane
{
    // the engine's lane values (EQSEQ), same names where it had one
    int steps = 16, ch = 1, div = 1, pulses = 4, shift = 0, gate = 65, loop = 0, note = 60, xpose = 0, octave = 0, vel = 96, velh = 0;
    int mode = 1;            // 1 note, 2 drum (fixed note), 3 and 4 controller
    bool enabled = false;
    uint64_t seq = 0;        // the pattern, bit i = step i, `steps` long
    bool custom = false;     // the pattern was edited by hand (toggle / setPattern); steps, fill or shift build it again
    int step = 0;            // the step the play-head is on
    int lastNote = 36;
    int lastCh = 0;          // the channel the sounding note went out on (0-15)
    int64_t off = -1;        // sample position of the pending note-off, -1 = none

    void updateSeq()
    {
        const int n = limit(steps, 2, 64);
        uint64_t b = PatternTable::get().bits[n][limit(pulses, 1, 64)];
        if (shift > 0)   // rotate right, as std::rotate(begin, begin + size - shift, end)
        {
            const int s = shift > n ? shift % n : shift;
            if (s > 0 && s < n)
            {
                const uint64_t mask = n == 64 ? ~0ull : (1ull << n) - 1;
                b = ((b << s) | (b >> (n - s))) & mask;
            }
        }
        seq = b;
        custom = false;
    }
    int size() const { return limit(steps, 2, 64); }
    static int divOf(int d)   // steps per whole note
    {
        switch (d)
        {
        case 2: return 8;
        case 3: return 4;
        case 4: return 2;
        case 5: return 1;
        case 6: return 32;
        case 7: return 24;
        case 8: return 12;
        case 9: return 6;
        case 10: return 3;
        default: return 16;
        }
    }
};

class Core
{
public:
    static const int kLanes = 8;
    static const int kMaxOut = 256;

    Lane lane[kLanes];
    bool started = false;
    long long tick = 0, pendingTick = 0;
    bool velSense = true, receiveNotes = true;
    int randMaxFill = 50;

    MidiOut out[kMaxOut];   // what this call produced; the owner sends it and sets outN = 0
    int outN = 0;

    Core()
    {
        PatternTable::get();
        for (int i = 0; i < kLanes; i++)   // the engine's start-up lanes (main() in src/euclidier.cpp)
        {
            Lane& q = lane[i];
            if (i > 3) { q.ch = 10; q.mode = 2; }
            if (i == 4) q.note = 36;
            if (i == 5) { q.note = 41; q.pulses = 2; q.shift = 4; }
            if (i == 6) { q.note = 38; q.steps = 4; q.pulses = 4; }
            if (i == 7) { q.note = 39; q.pulses = 4; q.shift = 2; }
            q.updateSeq();
        }
        lane[0].enabled = true;
    }

    void seed(uint32_t s) { rng_ = s ? s : 0x9E3779B9u; }

    // ---- time ----
    // Once per audio block, before anything else: where the block starts (a running sample count), its length, and the tempo.
    // Sends the note-offs that fall inside the block.
    void beginBlock(int64_t nowSample, int frames, double bpm, double sampleRate)
    {
        now_ = nowSample;
        if (bpm >= 20.0 && bpm <= 999.0) bpm_ = bpm;
        if (sampleRate > 1000.0) sr_ = sampleRate;
        const int64_t end = nowSample + frames;
        for (int i = 0; i < kLanes; i++)
            if (lane[i].off >= 0 && lane[i].off < end) release(lane[i]);
    }

    // The MIDI clock the engine followed, now called directly (vst/transport_grid.h makes these from the song position).
    void start()
    {
        pendingTick = 0;
        tick = 0;
        for (int i = 0; i < kLanes; i++) { lane[i].step = 0; kill(lane[i]); }
        started = true;
    }
    void stop()
    {
        started = false;
        for (int i = 0; i < kLanes; i++) { lane[i].step = 0; kill(lane[i]); }
    }
    void songpos(long s16)
    {
        pendingTick = (long long)(s16 & 0x3FFF) * 6;
        if (started) tick = pendingTick;
    }
    void pulse()
    {
        if (!started) return;
        for (int i = 0; i < kLanes; i++) fire(lane[i], tick);
        tick += 1;
    }

    // ---- values: the engine's control-socket SET (ctrlSet -> the controller handler), with its limits ----
    void set(int i, int param, int v)
    {
        if (i < 0 || i >= kLanes) return;
        Lane& q = lane[i];
        v = limit(v, 0, 127);
        switch (param)
        {
        case P_ENABLE:
            if (q.enabled != (v != 0)) { q.enabled = v != 0; kill(q); }
            break;
        case P_NOTE: q.note = limit(v, 0, 127); break;
        case P_DIV: q.div = limit(v, 1, 10); break;   // the pattern does not depend on it: a hand-edited one stays
        case P_STEPS: q.steps = limit(v, 2, 64); q.updateSeq(); break;
        case P_FILL: q.pulses = limit(v, 1, 64); q.updateSeq(); break;
        case P_SHIFT: q.shift = limit(v, 0, 64); q.updateSeq(); break;
        case P_GATE: q.gate = limit(v, 10, 95); kill(q); break;
        case P_CH: kill(q); q.ch = limit(v, 1, 15); break;
        case P_VEL: q.vel = v; break;
        case P_VELH: q.velh = v; break;
        case P_LOOP: q.loop = limit(v, 0, 64); break;
        case P_MODE:   // 0 note, 1 drum from the plugin; entering drum mode moves the lane to its own General MIDI drum note
        {
            const int m = v ? 2 : 1;
            if (m == q.mode) break;
            const int was = q.mode;
            kill(q);
            q.mode = m;
            if (m == 2 && was != 2) q.note = 36 + i;
            break;
        }
        default: break;
        }
    }
    int get(int i, int param) const
    {
        if (i < 0 || i >= kLanes) return 0;
        const Lane& q = lane[i];
        switch (param)
        {
        case P_ENABLE: return q.enabled ? 1 : 0;
        case P_NOTE: return q.note;
        case P_DIV: return q.div;
        case P_STEPS: return q.steps;
        case P_FILL: return q.pulses;
        case P_SHIFT: return q.shift;
        case P_GATE: return q.gate;
        case P_CH: return q.ch;
        case P_VEL: return q.vel;
        case P_VELH: return q.velh;
        case P_LOOP: return q.loop;
        case P_MODE: return q.mode == 2 ? 1 : 0;
        default: return 0;
        }
    }

    // A hand edit: flip one step, or set the whole pattern. The lane keeps it until steps, fill, shift, a randomise or a preset
    // builds the pattern again.
    void toggle(int i, int step)
    {
        if (i < 0 || i >= kLanes) return;
        Lane& q = lane[i];
        if (step < 0 || step >= q.size()) return;
        q.seq ^= 1ull << step;
        q.custom = true;
    }
    void setPattern(int i, uint64_t bits)
    {
        if (i < 0 || i >= kLanes) return;
        Lane& q = lane[i];
        const int n = q.size();
        q.seq = bits & (n == 64 ? ~0ull : (1ull << n) - 1);
        q.custom = true;
    }

    // The engine's RandomizeLane: a new loop, steps, fill and shift. The fill is 1 to half the new steps (the engine drew it against
    // the old steps with the top excluded, which kept it at 1-2).
    void randomize(int i)
    {
        if (i < 0 || i >= kLanes) return;
        Lane& q = lane[i];
        int r = rnd(0, 32);
        r = r < 9 ? 0 : r - 8;
        q.loop = r > 16 ? 16 : r;
        const int newSteps = rnd(4, 16);
        const int mx = std::max(2, (newSteps * randMaxFill) / 100);   // against the new steps; rnd's top is exclusive
        q.steps = newSteps;
        q.pulses = rnd(1, mx + 1);
        q.shift = rnd(0, 16);
        q.updateSeq();
    }

    // Presets: the engine's bank holds one EUPATCH per slot.
    void applyPatch(const EUPATCH& E)
    {
        for (int i = 0; i < kLanes; i++)
        {
            Lane& q = lane[i];
            const lanePatch& p = E.lane[i];
            const int m = limit(p.type, 1, 4);
            if (m != q.mode) { kill(q); q.mode = m; }
            q.vel = limit(p.BV, 0, 127);
            q.velh = limit(p.VA, 0, 127);
            if (q.ch != limit(p.ch, 1, 16)) kill(q);
            q.ch = limit(p.ch, 1, 16);
            q.gate = limit(p.gate, 10, 95);
            q.loop = limit(p.loop, 0, 64);
            if (q.enabled != (p.enabled != 0)) { q.enabled = p.enabled != 0; kill(q); }
            q.steps = limit(p.steps, 2, 64);
            q.pulses = limit(p.pulses, 1, 64);
            q.shift = limit(p.shift, 0, 64);
            q.div = limit(p.div, 0, 10);
            q.note = limit(p.note, 0, 127);
            q.updateSeq();
        }
    }
    EUPATCH patch() const
    {
        EUPATCH E;
        for (int i = 0; i < kLanes; i++)
        {
            const Lane& q = lane[i];
            lanePatch& p = E.lane[i];
            p.enabled = q.enabled;
            p.steps = (unsigned char)q.steps;
            p.pulses = (unsigned char)q.pulses;
            p.shift = (unsigned char)q.shift;
            p.loop = (unsigned char)q.loop;
            p.gate = (unsigned char)q.gate;
            p.div = (unsigned char)q.div;
            p.note = (unsigned char)q.note;
            p.BV = (unsigned char)q.vel;
            p.VA = (unsigned char)q.velh;
            p.type = (unsigned char)q.mode;
            p.ch = (unsigned char)q.ch;
        }
        return E;
    }

    // A played note transposes the NOTE-mode lanes (the engine's note handler): octave 0-7 picks the lane, the pitch class is
    // the transpose, the velocity an octave down / up; notes 96-103, 108-115 and 120-127 set a lane's octave to 0, +1, -1.
    void noteIn(int note, int vel)
    {
        if (!receiveNotes || vel < 1) return;
        const int oct = note / 12, x = note % 12;
        if (oct < 8)
        {
            if (lane[oct].mode > 1) return;
            if (velSense) lane[oct].octave = vel >= 127 ? 1 : vel < 22 ? -1 : 0;
            lane[oct].xpose = x;
        }
        if (x < 8 && lane[x].mode > 1) return;
        if (oct == 8 && x < 8) lane[x].octave = 0;
        if (oct == 9 && x < 8) lane[x].octave = 1;
        if (oct == 10 && x < 8) lane[x].octave = -1;
    }

    int sounding() const { int n = 0; for (int i = 0; i < kLanes; i++) n += lane[i].off >= 0; return n; }

private:
    void emit(uint8_t status, int d1, int d2)
    {
        if (outN < kMaxOut) out[outN++] = {status, (uint8_t)(d1 & 127), (uint8_t)(d2 & 127)};
    }
    // the pending note-off is due
    void release(Lane& q)
    {
        if (q.off < 0) return;
        if (q.mode <= 2) emit((uint8_t)(0x80 | q.lastCh), q.lastNote, 0);
        else if (q.lastNote > 0 && q.lastNote < 120) emit((uint8_t)(0xB0 | q.lastCh), q.lastNote, q.vel);   // controller modes: back to the base value
        q.off = -1;
    }
    // end whatever sounds, now (a value changed under it, or the transport stopped)
    void kill(Lane& q) { release(q); }

    int velocity(Lane& q)
    {
        if (q.mode <= 2)
        {
            if (q.velh == 0) return q.vel;
            return limit(q.vel + (int)(next() % (uint32_t)q.velh), 0, 127);
        }
        if (q.mode == 3) return q.velh == 0 ? q.vel : limit(q.velh, 1, 127);
        if (q.velh == 0) return q.vel;
        const int mx = q.vel > q.velh ? q.vel : q.velh, mn = q.vel > q.velh ? q.velh : q.vel, diff = mx - mn;
        if (diff == 0) return mn;
        int val = mn + (int)(next() % (uint32_t)diff);
        for (int guard = 0; guard < 64 && diff > 10 && (val - diff < 5 && diff - val < 5); guard++) val = mn + (int)(next() % (uint32_t)diff);
        return limit(val, 0, 127);
    }

    // EQSEQ::tick: the step is a pure function of the tick, so an edit never needs a resync
    void fire(Lane& q, long long t)
    {
        const int tps = 96 / Lane::divOf(q.div);   // clock ticks per step
        if (t % tps != 0) return;
        const long long n = t / tps;
        const long long modulus = q.loop > 0 ? q.loop : q.steps;
        const int size = q.size();
        q.step = (int)((n % (modulus > 0 ? modulus : 1)) % size);
        if (!((q.seq >> q.step) & 1) || !q.enabled) return;
        release(q);   // a note still sounding ends before the next begins
        if (q.mode < 3)
        {
            int note = q.note + 12 * q.octave + q.xpose;
            const int mul = note > 127 ? -1 : 1;
            while (note < 0 || note > 127) note += 12 * mul;
            if (q.mode == 2) note = q.note;
            q.lastCh = (q.ch - 1) & 15;
            emit((uint8_t)(0x90 | q.lastCh), note, velocity(q));
            q.lastNote = note;
        }
        else
        {
            const int cc = limit(q.note, 1, 119);
            q.lastCh = (q.ch - 1) & 15;
            emit((uint8_t)(0xB0 | q.lastCh), cc, velocity(q));
            q.lastNote = cc;
        }
        const double stepSamples = sr_ * 60.0 / bpm_ * 4.0 / Lane::divOf(q.div);
        q.off = now_ + (int64_t)(stepSamples * (q.gate / 100.0));
    }

    uint32_t next() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
    int rnd(int lo, int hi) { return (int)(next() % (uint32_t)(hi - lo)) + lo; }   // [lo, hi)

    int64_t now_ = 0;
    double bpm_ = 120.0, sr_ = 44100.0;
    uint32_t rng_ = 0x9E3779B9u;
};

} // namespace euclid
