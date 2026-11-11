/* Offline x86 host test for the SDK front end (src/sdk/EuclidierPlugin.cpp) with the real sequencer core inside it, run under
 * ASan/UBSan by tests/run.sh. The test container has no ALSA sequencer, so the notes are not sent: a test hook hands over every
 * message with the block it went out in, and the timing section checks them against the song position. */
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "params_gen.h"

typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*d)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void *proc;
    void (*setP)(AEffect *, int32_t, float);
    float (*getP)(AEffect *, int32_t);
    int32_t np, npar, ni, no, flags;
    intptr_t r1, r2;
    int32_t a, b, c;
    float io;
    void *obj, *user;
    int32_t uid, ver;
    void (*pr)(AEffect *, float **, float **, int32_t);
    void *pdr;
    char f[56];
};
typedef struct { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
                 int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags; } TI;
enum { kPlaying = 1 << 1, kCycle = 1 << 2, kPpq = 1 << 9, kTempo = 1 << 10, kCyclePos = 1 << 12 };

extern "C" AEffect *VSTPluginMain(cb);
extern "C" int euclidier_dbg_get(AEffect *, const char *);
struct Cap { int64_t sample; long long tick; uint8_t status, d1, d2; };
extern "C" int euclidier_dbg_take(AEffect *, void *, int);
extern "C" long euclidier_dbg_clock(AEffect *, int);

static TI g_ti;
static int automated[kNumParams];
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)v; (void)p; (void)o;
    if (op == 0 && idx >= 0 && idx < kNumParams) automated[idx]++;   /* audioMasterAutomate */
    if (op == 1) return 2400;                                        /* audioMasterVersion */
    if (op == 7) return (intptr_t)&g_ti;                             /* audioMasterGetTime */
    return 0;
}

static int fails = 0;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

static int idx(const char *key) {
    for (int i = 0; i < kNumParams; i++) if (!strcmp(g_pspecs[i].key, key)) return i;
    printf("FAIL no parameter %s\n", key); exit(1);
}
static void setv(AEffect *e, const char *key, int plain) {
    int i = idx(key);
    e->setP(e, i, (plain - g_pspecs[i].min) / (g_pspecs[i].max - g_pspecs[i].min));
}
static int getv(AEffect *e, const char *key) {
    int i = idx(key);
    return (int)lround(g_pspecs[i].min + e->getP(e, i) * (g_pspecs[i].max - g_pspecs[i].min));
}
static const char *disp(AEffect *e, const char *key) { static char d[64]; e->d(e, 7, idx(key), 0, d, 0); return d; }

static int engine(AEffect *e, const char *key) { return euclidier_dbg_get(e, key); }   /* what the sequencer core holds (-1: no such key) */

static float L[128], R[128], *out[2] = {L, R};
static double ppb;
static void run(AEffect *e, int ms) {   /* real time passes (the worker runs every 30 ms) while audio blocks are processed */
    for (int k = 0; k < ms / 10; k++) { usleep(10000); e->pr(e, 0, out, 128); if (g_ti.flags & kPlaying) g_ti.ppqPos += ppb; }
}
static void waitUp(AEffect *e) { run(e, 300); }

/* ---------------------------------------------------------------------------------------------------------------------------
 * Timing, end to end: host transport -> the plugin -> the notes it sends. A fresh instance plays its default lanes (1-4 on, all
 * 1/16: E(4,16), E(3,8), E(5,12), E(5,16), notes 36-39) through a scripted transport; every block's song position is logged.
 *   on the grid:  a note-on goes out with the block whose range [ppq, ppq + block) holds its 16th (at a loop end: the loop start)
 *   complete:     each lane plays exactly the hits of the 16ths the transport passed over, once per pass, no more
 *   released:     every note-on gets its note-off, in order, and nothing sounds after Stop
 * ------------------------------------------------------------------------------------------------------------------------- */
#define MAXBLK 200000
static double blkPpq[MAXBLK], blkLen[MAXBLK];   /* by block number of the instance: song position at its start (-1 stopped), length in beats */
static long nblk;
static double Lstart, Lend;                    /* the loop the host reports (Lend <= Lstart: none) */
static void blockM(AEffect *m) {
    const double len = (g_ti.tempo / 60.0) * (128.0 / 44100.0);
    blkPpq[nblk] = (g_ti.flags & kPlaying) ? g_ti.ppqPos : -1.0; blkLen[nblk] = len; nblk++;
    m->pr(m, 0, out, 128);
    if (g_ti.flags & kPlaying) {
        g_ti.ppqPos += len;
        if (Lend > Lstart && g_ti.ppqPos >= Lend) g_ti.ppqPos = Lstart + (g_ti.ppqPos - Lend);   /* the host wraps */
    }
}
static int laneHit(int lane, long s16) {   /* does default lane 0-3 play on this 16th? Bjorklund patterns of the defaults */
    static const char *pat[4] = {"1000100010001000", "10010010", "100101001010", "1001001001001000"};   /* checked against the core below */
    const int n = (int)strlen(pat[lane]);
    return pat[lane][s16 % n] == '1';
}
static struct Cap capbuf[1 << 16];
static long expectHits[4], gotHits[4];
/* count what the transport passed over since block b0: passes are split where the position jumps (a wrap, a locate, a start) */
static void expectFrom(long b0) {
    double hw = -1e9;   /* covered up to here in this pass */
    for (long k = b0; k < nblk; k++) {
        double p = blkPpq[k];
        if (p < 0) { hw = -1e9; continue; }
        double from = p, to = p + blkLen[k];
        if (hw > -1e8 && fabs(p - hw) <= 1.0) from = hw;                    /* carry on (jitter or a tempo step back: nothing twice) */
        else if (hw > -1e8 || k == b0 || blkPpq[k - 1] < 0) from = p - (Lend > Lstart && p >= Lstart - 0.002 && p < Lend ? fmin(0.002, p - Lstart) : 0.0);
        if (to <= from) continue;
        for (int pass = 0; pass < 2; pass++) {
            double a0 = from, a1 = to;
            if (Lend > Lstart && p < Lend && to > Lend) { if (pass == 0) a1 = Lend; else { a0 = Lstart; a1 = Lstart + (to - Lend); } }
            else if (pass == 1) break;
            for (long s = (long)ceil(a0 * 4.0 - 1e-7); s / 4.0 < a1 - 1e-7; s++)
                if (s >= 0) for (int l = 0; l < 4; l++) expectHits[l] += laneHit(l, s);
            hw = a1;
        }
    }
}
static int timingCheck(AEffect *m, long b0, const char *what) {
    const int n = euclidier_dbg_take(m, capbuf, 1 << 16);
    int offGrid = 0, open[4] = {0, 0, 0, 0}, overlap = 0, orphan = 0, other = 0;
    memset(gotHits, 0, sizeof gotHits); memset(expectHits, 0, sizeof expectHits);
    for (int i = 0; i < n; i++) {
        const struct Cap *c = &capbuf[i];
        const int lane = c->d1 - 36, kind = c->status & 0xF0;
        if (lane < 0 || lane > 3 || (kind != 0x90 && kind != 0x80)) { other++; continue; }
        if (kind == 0x80) { if (open[lane]-- <= 0) { orphan++; open[lane] = 0; } continue; }
        if (open[lane]++ > 0) overlap++;
        gotHits[lane]++;
        const long k = (long)(c->sample / 128);
        const double p = blkPpq[k], e = p + blkLen[k];
        int ok = 0;
        for (long s = (long)floor(p * 4.0) - 1; s <= (long)ceil(e * 4.0) + 1 && !ok; s++)
            if (s >= 0 && laneHit(lane, s) && s / 4.0 >= p - 0.0021 && s / 4.0 < e + 1e-7) ok = 1;
        if (!ok && Lend > Lstart && e > Lend)   /* the part of the block past the loop end plays the loop start */
            for (long s = (long)(Lstart * 4.0); s / 4.0 < Lstart + (e - Lend) + 1e-7 && !ok; s++) if (laneHit(lane, s)) ok = 1;
        if (!ok) { if (offGrid < 3) printf("     off the grid: lane %d in block %ld [%.4f, %.4f)\n", lane + 1, k, p, e); offGrid++; }
    }
    expectFrom(b0);
    int counts = 1;
    for (int l = 0; l < 4; l++) counts &= gotHits[l] == expectHits[l];
    printf("     %s: %d messages; hits %ld %ld %ld %ld (expected %ld %ld %ld %ld); off grid %d, overlapping %d, orphan offs %d, other %d, sounding %d\n",
           what, n, gotHits[0], gotHits[1], gotHits[2], gotHits[3], expectHits[0], expectHits[1], expectHits[2], expectHits[3], offGrid, overlap, orphan, other,
           open[0] + open[1] + open[2] + open[3]);
    char msg[160];
    snprintf(msg, sizeof msg, "%s: every note on its 16th, each hit once, all released", what);
    const int ok = gotHits[0] > 0 && counts && !offGrid && !overlap && !orphan && !other && open[0] + open[1] + open[2] + open[3] == 0;
    check(ok, msg);
    return ok;
}
static void stopM(AEffect *m) { g_ti.flags &= ~kPlaying; for (int k = 0; k < 40; k++) blockM(m); }
/* the 2.0 features: ALL view, taps on step cells (kept and saved), DRUM MODE ALL, the lane keys */
static void features(void) {
    AEffect *m = VSTPluginMain(host);
    g_ti.flags = kPpq | kTempo; g_ti.ppqPos = 0; g_ti.tempo = 128.0; Lstart = Lend = 0;
    run(m, 500);
    /* a tap on a step cell flips that step; the pattern is then hand-made and saved with the project */
    check(getv(m, "g1_16_1") == 1 && engine(m, "m1_on") == 0, "features: lane 1 step 2 is off, the pattern is the generated one");
    setv(m, "tg1_2", 1); run(m, 400);   /* slice 2 of 32 = cell 1 of the 16-cell row */
    check(getv(m, "g1_16_1") == 2 && engine(m, "m1_on") == 1 && (engine(m, "m1_a") & 2), "a tap on a row cell turns the step on");
    check(getv(m, "g1_16_0") == 2 && getv(m, "g1_16_4") == 2, "the other steps are as they were");
    setv(m, "ct_8", 1); run(m, 400);   /* the circle: lane 1, 16 steps in the 16-dot ring: step 4 is in sector 8 of 32 */
    check(getv(m, "g1_16_4") == 1 && !(engine(m, "m1_a") & 16), "a tap on a circle dot turns that step off");
    void *c1 = 0; intptr_t n1 = m->d(m, 23, 0, 0, &c1, 0);
    char *saved = (char *)malloc((size_t)n1); memcpy(saved, c1, (size_t)n1);
    AEffect *r = VSTPluginMain(host);
    r->d(r, 24, 0, n1, saved, 0); run(r, 600);
    check(engine(r, "m1_on") == 1 && engine(r, "m1_a") == engine(m, "m1_a") && getv(r, "g1_16_1") == 2 && getv(r, "g1_16_4") == 1, "a hand-made pattern comes back with the project");
    check(engine(r, "m2_on") == 0, "a generated pattern stays generated");
    free(saved); r->d(r, 1, 0, 0, 0, 0);
    setv(m, "l1_fill", 6); run(m, 300);
    check(engine(m, "m1_on") == 0, "changing the fill builds the pattern again");
    /* DRUM MODE ALL: every lane to its own drum note, also a lane already in DRUM mode */
    setv(m, "l2_note", 90); setv(m, "l8_note", 12); run(m, 300);
    setv(m, "all_drum", 1); run(m, 400);
    check(engine(m, "l1_note") == 36 && engine(m, "l2_note") == 37 && engine(m, "l5_note") == 40 && engine(m, "l8_note") == 43 && getv(m, "l2_mode") == 1, "DRUM MODE ALL maps lanes 1-8 to notes 36-43");
    /* the lane keys and ALL */
    setv(m, "view", 2); run(m, 300);
    check(engine(m, "sel") == 2 && getv(m, "sel") == 2 && getv(m, "sel_fill") == getv(m, "l3_fill"), "a lane key selects the lane and its controls");
    setv(m, "view", 8); run(m, 400);
    int sum = 0; for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_fill", l); sum += getv(m, k); }
    check(getv(m, "sel_fill") == (sum + 4) / 8, "ALL: FILL shows the mean of the eight lanes");
    int f[8]; for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_fill", l); f[l - 1] = getv(m, k); }
    const int d = 2, shown = getv(m, "sel_fill");
    setv(m, "sel_fill", shown + d); run(m, 500);
    int ok = 1; for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_fill", l); ok &= getv(m, k) == (f[l - 1] + d > 32 ? 32 : f[l - 1] + d); }
    check(ok, "ALL: turning FILL moves every lane by the same amount");
    const int st0 = getv(m, "l3_steps");
    setv(m, "sel_steps", 7); setv(m, "sel_note", 99); run(m, 500);
    check(getv(m, "l3_steps") == st0 && getv(m, "sel_steps") == st0 && getv(m, "sel_note") == getv(m, "l3_note"), "ALL: STEPS and NOTE are not available (they spring back)");
    setv(m, "l3_enable", 1); setv(m, "l3_steps", 24); run(m, 500);
    check(getv(m, "c24_0") > 0 && getv(m, "c16_0") == 0, "ALL: the circle shows the longest pattern (lane 3, 24 steps)");
    setv(m, "view", 0); run(m, 300);
    check(engine(m, "sel") == 0 && getv(m, "sel_fill") == getv(m, "l1_fill"), "back to lane 1: its own values again");
    m->d(m, 1, 0, 0, 0, 0);
}

static void timing(void);
static void features(void);
static void timing(void) {
    AEffect *m = VSTPluginMain(host);
    nblk = 0; Lstart = Lend = 0;
    g_ti.tempo = 128.0; g_ti.flags = kPpq | kTempo; g_ti.ppqPos = 0;
    for (int k = 0; k < 50; k++) blockM(m);
    euclidier_dbg_take(m, capbuf, 1 << 16);
    /* the default patterns, as the core made them: the model above must agree before it judges anything */
    { static const int st[4] = {16, 8, 12, 16}, fi[4] = {4, 3, 5, 5}; int ok = 1; char k[24];
      for (int l = 0; l < 4; l++) { snprintf(k, sizeof k, "l%d_steps", l + 1); ok &= engine(m, k) == st[l]; snprintf(k, sizeof k, "l%d_fill", l + 1); ok &= engine(m, k) == fi[l]; }
      check(ok, "timing: the default lanes are the four patterns the model expects"); }

    long b0 = nblk;   /* 1. steady play from the top, 40 s at 128 BPM */
    g_ti.flags |= kPlaying; g_ti.ppqPos = 0;
    for (int k = 0; k < 13800; k++) blockM(m);
    stopM(m);
    timingCheck(m, b0, "steady play");

    b0 = nblk;        /* 2. a 2-bar loop the host reports, not block aligned, 30 wraps */
    Lstart = 0; Lend = 8; g_ti.cycleStartPos = Lstart; g_ti.cycleEndPos = Lend;
    g_ti.flags |= kPlaying | kCycle | kCyclePos; g_ti.ppqPos = 0;
    for (int k = 0; k < 39000; k++) blockM(m);
    stopM(m);
    timingCheck(m, b0, "2-bar loop, 30 wraps");

    b0 = nblk;        /* 3. the same loop from bar 2 beat 3.37 (a start in the middle of a 16th) */
    g_ti.flags |= kPlaying; g_ti.ppqPos = 6.37;
    for (int k = 0; k < 9000; k++) blockM(m);
    stopM(m);
    timingCheck(m, b0, "start mid-16th inside the loop");
    Lstart = Lend = 0; g_ti.flags &= ~(kCycle | kCyclePos);

    b0 = nblk;        /* 4. tempo changes while playing; the host's position steps back a little at each (as MPC's does) */
    g_ti.flags |= kPlaying; g_ti.ppqPos = 0;
    { static const double bpm[] = {128, 90, 174, 60, 141.5, 200, 128};
      for (int seg = 0; seg < 7; seg++) { g_ti.tempo = bpm[seg]; if (seg) g_ti.ppqPos -= 0.004; for (int k = 0; k < 2500; k++) blockM(m); } }
    stopM(m);
    timingCheck(m, b0, "tempo changes");
    g_ti.tempo = 128.0;

    b0 = nblk;        /* 5. locates while playing: forward, back, onto a 16th and off one */
    g_ti.flags |= kPlaying; g_ti.ppqPos = 0;
    { static const double to[] = {40.0, 2.0, 17.13, 3.9999, 64.25};
      for (int seg = 0; seg < 5; seg++) { for (int k = 0; k < 1500; k++) blockM(m); g_ti.ppqPos = to[seg]; }
      for (int k = 0; k < 1500; k++) blockM(m); }
    stopM(m);
    timingCheck(m, b0, "locates");

    b0 = nblk;        /* 6. stop and start, 25 times, at odd positions */
    for (int r = 0; r < 25; r++) { g_ti.flags |= kPlaying; g_ti.ppqPos = r * 1.37; for (int k = 0; k < 300 + r * 7; k++) blockM(m); stopM(m); }
    timingCheck(m, b0, "25 stops and starts");

    b0 = nblk;        /* 7. the worker busy: values hammered from the host (sel_vel on lane 1) at real-time pace while playing */
    g_ti.flags |= kPlaying; g_ti.ppqPos = 0;
    for (int k = 0; k < 3000; k++) { blockM(m); if (k % 3 == 0) setv(m, "sel_vel", 40 + k % 80); if (k % 50 == 0) usleep(2000); }
    stopM(m);
    timingCheck(m, b0, "values changing while playing");

    b0 = nblk;        /* 8. an edit that ends a sounding note (gate), a lane switched off and on, a channel change: all released */
    g_ti.flags |= kPlaying; g_ti.ppqPos = 0;
    for (int k = 0; k < 4000; k++) {
        blockM(m);
        if (k % 97 == 0) setv(m, "l2_gate", 20 + k % 70);
        if (k % 211 == 0) { setv(m, "l3_enable", 0); }
        if (k % 211 == 30) { setv(m, "l3_enable", 1); }
        if (k % 40 == 0) usleep(1500);
    }
    stopM(m);
    { const int n = euclidier_dbg_take(m, capbuf, 1 << 16); int open[4] = {0, 0, 0, 0}, bad = 0, ons = 0;
      for (int i = 0; i < n; i++) { const int lane = capbuf[i].d1 - 36, kind = capbuf[i].status & 0xF0; if (lane < 0 || lane > 3) { bad++; continue; }
        if (kind == 0x90) { ons++; if (open[lane]++ > 0) bad++; } else if (kind == 0x80) { if (open[lane]-- <= 0) bad++; } else bad++; }
      printf("     edits under playing notes: %d note-ons, %d faults, %d sounding\n", ons, bad, open[0] + open[1] + open[2] + open[3]);
      check(ons > 100 && !bad && open[0] + open[1] + open[2] + open[3] == 0, "edits under playing notes: every note released, in order"); }
    m->d(m, 1, 0, 0, 0, 0);
}

int main(void) {
    g_ti.tempo = 120.0; g_ti.sampleRate = 44100.0;
    ppb = (g_ti.tempo / 60.0) * (128.0 / 44100.0);
    AEffect *a = VSTPluginMain(host), *b = VSTPluginMain(host);
    printf("magic=%x params=%d flags=%x uid=%x\n", a->magic, a->npar, a->flags, a->uid);
    check(a != b && a->npar == 984 && a->uid == 0x45714472, "two instances, 696 parameters (669 of 1.x + 315 appended), uid EqDr");
    /* MPC reads parameter 0 at start and writes the same value back once the plugin is loaded: lane 1 must stay on */
    { float v0 = b->getP(b, 0); check(v0 == 1.0f, "lane 1 reads ON from the first moment"); b->setP(b, 0, v0); }
    waitUp(a); waitUp(b);
    run(b, 300);
    check(getv(b, "l1_enable") == 1 && engine(b, "l1_enable") == 1, "the host's write-back of parameter 0 leaves lane 1 on");

    /* a fresh instance: lanes 1-4 on with their own patterns, 5-8 off, every lane DRUM; the host is told */
    {
        int bad = 0;
        for (int l = 1; l <= 8; l++) {
            char k[24];
            snprintf(k, sizeof k, "l%d_enable", l); bad += getv(b, k) != (l <= 4);
            snprintf(k, sizeof k, "l%d_mode", l); bad += getv(b, k) != 1;
        }
        bad += getv(b, "l2_steps") != 8 || getv(b, "l3_fill") != 5 || engine(b, "l2_steps") != 8;
        check(!bad, "fresh instance defaults (lanes 1-4 on, lane 2 = 8 steps, all DRUM)");
        check(getv(b, "sel_steps") == 16 && getv(b, "sel_mode") == 1, "the selected lane's copies follow lane 1");
    }

    /* values reach the engine; choices show their names */
    setv(a, "l1_mode", 0); setv(a, "l3_steps", 21); setv(a, "l4_div", 3);
    run(a, 200);
    check(!strcmp(disp(a, "l1_mode"), "NOTE") && !strcmp(disp(a, "l4_div"), "1/4") && !strcmp(disp(a, "l3_steps"), "21"), "value text");
    check(engine(a, "l1_mode") == 0 && engine(a, "l3_steps") == 21 && engine(a, "l4_div") == 3, "values reach the engine");
    setv(a, "l1_note", 50); run(a, 400);
    setv(a, "l1_mode", 1); run(a, 400);
    check(getv(a, "l1_note") == 36 && getv(a, "sel_note") == 36, "entering DRUM mode re-reads the drum note the core picked");
    setv(a, "l1_mode", 0); run(a, 200);

    /* parameter 0 (lane 1 on/off): the host's nudge-and-back on prepare must not reach the engine; a held change must */
    {
        int before = engine(a, "l1_enable");
        a->setP(a, 0, before ? 0.0f : 1.0f); a->setP(a, 0, before ? 1.0f : 0.0f);
        run(a, 150);
        check(engine(a, "l1_enable") == before && getv(a, "l1_enable") == before, "parameter 0 nudge is harmless");
        setv(a, "l1_enable", !before);
        run(a, 200);
        check(engine(a, "l1_enable") == !before && getv(a, "sel_enable") == !before, "a held change of parameter 0 arrives (and its sel_ copy)");
        setv(a, "l1_enable", 1); run(a, 150);
    }

    /* transport: a Start, a Song Position Pointer, then 24 pulses per quarter note on the song-position grid */
    {
        g_ti.flags = kPlaying | kPpq | kTempo; g_ti.ppqPos = 0.0;
        for (int k = 0; k < 400; k++) { a->pr(a, 0, out, 128); g_ti.ppqPos += ppb; }
        long want = (long)ceil(g_ti.ppqPos * 24.0);
        long st = euclidier_dbg_clock(a, 0), sp = euclidier_dbg_clock(a, 2), pu = euclidier_dbg_clock(a, 3);
        printf("     400 blocks to ppq %.3f: start %ld, songpos %ld, pulses %ld (expect about %ld)\n", g_ti.ppqPos, st, sp, pu, want);
        check(st == 1 && sp >= 1 && labs(pu - want) <= 1, "clock follows the transport");
    }

    /* step displays: lane 1 at 12 steps -> the 16-slot row shows 12 cells, the other classes stay hidden, the play-head moves */
    {
        memset(automated, 0, sizeof automated);
        setv(a, "l1_steps", 12);
        int i16[16], moved = 0, last = -1, shown = 0, wrong = 0, told = 0;
        for (int j = 0; j < 16; j++) { char k[24]; snprintf(k, sizeof k, "g1_16_%d", j); i16[j] = idx(k); }
        for (int k = 0; k < 120; k++) {
            run(a, 10);
            for (int j = 0; j < 16; j++) if (a->getP(a, i16[j]) > 0.7f && j != last) { last = j; moved++; }
        }
        for (int j = 0; j < 16; j++) { shown += a->getP(a, i16[j]) > 0.0f; told += automated[i16[j]] > 0; }
        for (int i = 0; i < kNumParams; i++) { int c, s; if (sscanf(g_pspecs[i].key, "g1_%d_%d", &c, &s) == 2 && c != 16 && a->getP(a, i) > 0.0f) wrong++; }
        printf("     16-slot cells shown %d (expect 12), play-head moved %d times, host told about %d cells\n", shown, moved, told);
        check(shown == 12 && moved > 3 && !wrong && told >= 12, "lane row cells");
        int c12 = 0; for (int j = 0; j < 12; j++) { char k[24]; snprintf(k, sizeof k, "c12_%d", j); c12 += getv(a, k) > 0; }
        check(c12 == 12, "circle shows the selected lane (12 dots)");
    }

    /* the lane stepper and the selected lane's copies */
    {
        setv(a, "sel_next", 1);
        run(a, 300);
        check(!strcmp(disp(a, "sel"), "LANE 2") && engine(a, "sel") == 1, "sel_next -> LANE 2");
        check(getv(a, "sel_next") == 0 && automated[idx("sel_next")] > 0, "the arrow drops back and the host is told");
        check(getv(a, "sel_steps") == getv(a, "l2_steps") && getv(a, "sel_steps") == 8, "sel_ copies re-read for the new lane");
        setv(a, "sel_fill", 6);
        check(getv(a, "l2_fill") == 6, "sel_fill is mirrored to l2_fill at once");
        setv(a, "l2_gate", 80);
        check(getv(a, "sel_gate") == 80, "l2_gate is mirrored to sel_gate at once");
        run(a, 200);
        check(engine(a, "l2_fill") == 6 && engine(a, "l2_gate") == 80 && engine(a, "l1_fill") != 6, "and both reach lane 2 only");
        const int div0 = getv(a, "sel_div");
        setv(a, "sel_div_next", 1); run(a, 200);
        check(getv(a, "sel_div") == div0 + 1 && getv(a, "l2_div") == div0 + 1 && engine(a, "l2_div") == div0 + 1, "sel_div_next steps the lane's division");
        const int ch0 = (int)getv(a, "sel_ch");
        setv(a, "sel_ch_next", 1); run(a, 200);
        check(getv(a, "sel_ch") == ch0 + 1 && getv(a, "l2_ch") == ch0 + 1 && engine(a, "l2_ch") == ch0 + 1, "sel_ch_next steps the lane's MIDI channel");
    }

    /* randomise: new steps (4-15), fill, shift and loop for the picked lanes, nothing else */
    {
        int was[9];
        for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_steps", l); setv(a, k, 30 + (l & 1)); }
        run(a, 500);
        for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_steps", l); was[l] = getv(a, k); }
        setv(a, "rand_3", 1); run(a, 300);
        int others = 0; for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_steps", l); others += l != 3 && getv(a, k) != was[l]; }
        check(getv(a, "l3_steps") < 16 && engine(a, "l3_steps") == getv(a, "l3_steps") && !others, "rand_3 randomises lane 3 alone");
        check(getv(a, "l3_fill") >= 1 && getv(a, "l3_fill") <= getv(a, "l3_steps"), "the randomised fill fits the lane");
        setv(a, "rand_sel", 1); run(a, 300);   /* the selected lane is LANE 2 */
        check(getv(a, "l2_steps") < 16 && getv(a, "sel_steps") == getv(a, "l2_steps") && getv(a, "l1_steps") == was[1], "rand_sel randomises the selected lane alone");
        setv(a, "rand_all", 1); run(a, 300);
        int all = 1; for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_steps", l); all &= getv(a, k) < 16; }
        check(all && getv(a, "rand_all") == 0, "rand_all randomises every lane and drops back");
        setv(a, "l3_steps", 30); run(a, 500);
        setv(a, "rand_3", 1); run(a, 300);
        check(getv(a, "l3_steps") < 16, "a second press fires again");
    }

    /* all_drum */
    {
        setv(a, "l5_mode", 0); setv(a, "l2_mode", 0); run(a, 200);
        setv(a, "all_drum", 1); run(a, 300);
        int drums = 0; for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_mode", l); drums += getv(a, k) == 1 && engine(a, k) == 1; }
        check(drums == 8 && getv(a, "sel_mode") == 1, "all_drum puts every lane in DRUM mode");
    }

    /* state round trip into a fresh instance; a restore fires no engine verb */
    {
        setv(a, "l1_note", 57); setv(a, "l2_note", 64); setv(a, "l3_note", 71); setv(a, "sel", 3); setv(a, "l6_ch", 11);
        run(a, 500);
        void *c1 = 0;
        intptr_t n1 = a->d(a, 23, 0, 0, &c1, 0);
        char *saved = (char *)malloc((size_t)n1); memcpy(saved, c1, (size_t)n1);
        check(n1 > 4 && !memcmp(saved, "MPCP", 4), "the state is an MPCP chunk");
        AEffect *r = VSTPluginMain(host);
        r->d(r, 24, 0, n1, saved, 0);   /* before the engine is up, as the MPC does */
        waitUp(r); run(r, 500);
        int bad = 0;
        for (int i = 0; i < kNumParams; i++) {
            if (g_pspecs[i].flags & (kMomentary | kCell)) continue;
            if (fabsf(a->getP(a, i) - r->getP(r, i)) > 0.004f) { printf("     restore differs: %s %.3f -> %.3f\n", g_pspecs[i].key, a->getP(a, i), r->getP(r, i)); bad++; }
        }
        check(!bad, "state round trip (every lane value and the selected lane's copies)");
        check(engine(r, "l1_note") == 57 && engine(r, "l3_note") == 71 && engine(r, "sel") == 3 && engine(r, "l6_ch") == 11, "the restored engine holds the values");
        int before[8], same = 1;   /* a randomise would change the steps: a restore fires no verb */
        for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_steps", l); before[l - 1] = engine(a, k) * 100 + engine(a, (snprintf(k, sizeof k, "l%d_fill", l), k)); }
        a->d(a, 24, 0, n1, saved, 0); run(a, 500);
        for (int l = 1; l <= 8; l++) { char k[24]; snprintf(k, sizeof k, "l%d_steps", l); same &= before[l - 1] == engine(a, k) * 100 + engine(a, (snprintf(k, sizeof k, "l%d_fill", l), k)); }
        check(same, "a restore into a running instance fires no verb (randomise, bank load, bank save)");
        free(saved);
        r->d(r, 1, 0, 0, 0, 0);
    }

    /* a project saved by the 1.x build: text, with the old triggers and the selected lane's copies in it */
    {
        char legacy[] = "l1_steps=9;l2_note=50;l2_mode=0;l3_note=77;sel=4;sel_steps=30;sel_note=1;rand_go=0;preset_load=0;preset_save=0;l7_enable=1;l8_div=5;";
        AEffect *c = VSTPluginMain(host);
        c->d(c, 24, 0, (intptr_t)sizeof legacy, legacy, 0);
        waitUp(c); run(c, 500);
        check(getv(c, "l1_steps") == 9 && getv(c, "l3_note") == 77 && getv(c, "l7_enable") == 1 && !strcmp(disp(c, "l8_div"), "1"), "1.x chunk: lane values");
        check(getv(c, "l2_mode") == 0 && getv(c, "l2_note") == 50 && engine(c, "l2_note") == 50, "1.x chunk: the mode is set before the note");
        check(!strcmp(disp(c, "sel"), "LANE 5") && getv(c, "l5_steps") != 30 && getv(c, "sel_steps") == getv(c, "l5_steps"), "1.x chunk: sel_ copies are not applied");
        check(engine(c, "l1_steps") == 9 && engine(c, "l1_fill") == 4, "1.x chunk: the saved triggers fire nothing");
        c->d(c, 1, 0, 0, 0, 0);
    }

    /* no audio running (the host is not processing): a change still arrives, applied by the worker */
    {
        usleep(200000);
        setv(b, "l2_steps", 23); setv(b, "l6_note", 99);
        usleep(400000);
        check(engine(b, "l2_steps") == 23 && engine(b, "l6_note") == 99 && getv(b, "l2_steps") == 23, "values arrive with no audio running");
        run(b, 100);
        check(engine(b, "l2_steps") == 23, "and hold when audio resumes");
    }

    /* stop */
    g_ti.flags = kPpq | kTempo; a->pr(a, 0, out, 128);
    check(euclidier_dbg_clock(a, 1) == 1, "Stop is sent when the transport stops");

    a->d(a, 1, 0, 0, 0, 0);
    b->d(b, 1, 0, 0, 0, 0);
    timing();
    features();
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
