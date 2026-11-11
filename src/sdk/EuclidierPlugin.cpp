// Euclidier for MPC / Force: the MPC Plugin SDK front end (framework/mpc_plugin.h) with the sequencer inside the plugin.
//
// Euclidier makes no sound. It is a MIDI generator with eight Euclidean lanes (src/core/euclid_core.h: the 1.x standalone engine
// as a library). Up to 1.x the engine was a child process fed a MIDI clock over ALSA and asked for its values over a socket;
// now it is stepped by the audio thread itself:
//   * time: process() turns the host's song position into 24-PPQN ticks (vst/transport_grid.h: the ticks of this block, sent at
//     its start, cut at the loop end, re-anchored to the song position at Start, at every wrap or locate and once a bar) and
//     calls the core for each. Note lengths are counted in samples. No clock leaves the plugin, nothing waits on another process.
//   * notes: our ALSA sequencer port "<Name>" (a second instance "<Name> 2"), written from the audio thread at the start of the
//     block. The MPC lists it as a MIDI input and the user routes a track from it. (The MPC discards a plugin's VST MIDI
//     output, so this stays ALSA.) Notes played into the plugin's own track transpose the NOTE-mode lanes.
//   * settings: the audio thread owns the core. The worker thread sends it commands through a lock-free queue and reads the
//     values back from a snapshot the audio thread publishes after every block; neither ever waits for the other. If no audio
//     runs (the host is not processing), the worker applies the commands itself.
//
// Parameters: the 669 of the 1.x build in the same order, then the ones 2.0 appended (sel_ch_prev / sel_ch_next) (vst/params.lock -> tools/gen_sdk_params.py -> params_gen.h):
//   * lane values "l<N>_<x>" and the selected lane's copies "sel_<x>" (one engine value behind both: kept equal here);
//   * momentary verbs (randomise, all-drum, stepper arrows): fire on 0 -> 1, then drop back to 0;
//   * 540 display cells (lane rows "g<lane>_<cap>_<slot>", circle "c<cap>_<slot>"): the worker polls each lane's pattern and
//     play-head and sets them; the skin shows one picture per cell.
// The worker tells the host about every value the plugin or the engine changed (a randomise, a lane pick, the cells).
// Parameter 0 is lane 1's on/off switch (frozen order). The host nudges parameter 0 and puts it back on every prepare, so a
// change to it counts only once it has held for ~40 ms.
// State: the framework saves every parameter by id. On load only real lane values reach the engine (not the cells, the selected
// lane's copies or the verbs). A project saved by 1.x ("key=value;" text) is still read (effSetChunk is wrapped below).

#include "mpc_plugin.h"
#include "mpc_resources.h"

#include "params_gen.h"
#include "transport_grid.h"
#include "../core/euclid_core.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#ifndef EUCLIDIER_NAME
#define EUCLIDIER_NAME "Euclid"
#endif
#ifndef EUCLIDIER_UID
#define EUCLIDIER_UID 'E', 'q', 'D', 'r'
#endif
#define EUCLIDIER_FOURCC(...) MPC_FOURCC(__VA_ARGS__)   // expands EUCLIDIER_UID before MPC_FOURCC counts its arguments
#ifndef EUCLIDIER_VERSION
#define EUCLIDIER_VERSION 2000
#endif
#ifndef EUCLIDIER_SLUG
#define EUCLIDIER_SLUG "euclidier"
#endif
namespace {

FILE* g_log = nullptr;
#define ELOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

// One slot per live instance (lowest free), so names stay stable when an instance is removed and inserted again: the first is
// "Euclidier", the next "Euclidier 2", and the MPC keeps a port's routing.
std::atomic<unsigned> g_slots{0};

mpc::PluginInfo makeInfo()
{
    mpc::PluginInfo i;
    i.name = EUCLIDIER_NAME;
    i.vendor = "sd88me";
    i.uniqueId = EUCLIDIER_FOURCC(EUCLIDIER_UID);
    i.version = EUCLIDIER_VERSION;
    i.isInstrument = true;
    i.numInputs = 0;
    i.numOutputs = 2;
    return i;
}

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the display cells (names from vst/make_skin.py) ----
enum VKind : unsigned char { V_NONE = 0, V_GCELL, V_CCELL };
struct VInfo { VKind kind = V_NONE; int lane = 0, cap = 0, slot = 0; };
struct Tables
{
    VInfo v[kNumParams];
    std::vector<int> cells;                        // every display cell
    std::vector<int> gcaps, ccaps;                 // size-class capacities, ascending
    std::unordered_map<std::string, int> index;    // key -> parameter index
};
const Tables& tables()
{
    static const Tables& t = *([] {   // never destroyed: no static destructor at unload
        Tables* x = new Tables();
        auto addCap = [](std::vector<int>& v, int c) { if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c); };
        for (int i = 0; i < kNumParams; i++)
        {
            const char* k = g_pspecs[i].key;
            int a, b, c;
            x->index[k] = i;
            if (!(g_pspecs[i].flags & kCell)) continue;
            if (k[0] == 'g' && std::sscanf(k, "g%d_%d_%d", &a, &b, &c) == 3) { x->v[i] = {V_GCELL, a - 1, b, c}; addCap(x->gcaps, b); }
            else if (k[0] == 'c' && std::sscanf(k, "c%d_%d", &a, &b) == 2) { x->v[i] = {V_CCELL, 0, a, b}; addCap(x->ccaps, a); }
            x->cells.push_back(i);
        }
        std::sort(x->gcaps.begin(), x->gcaps.end());
        std::sort(x->ccaps.begin(), x->ccaps.end());
        return x;
    })();
    return t;
}
int classFor(const std::vector<int>& caps, int n)
{
    for (int c : caps) if (c >= n) return c;
    return caps.empty() ? 0 : caps.back();
}
// slot[0..cap): 0 hidden / 1 off / 2 on for `n` steps (bits[]) shown in `cap` slots. contiguous: a lane row, one cell per step
// from the left while n <= cap; otherwise the steps are spread round the circle (steps sharing a slot light it if any is on).
void fillSlots(const int* bits, int n, int cap, bool contiguous, int* slot)
{
    for (int s = 0; s < cap; s++) slot[s] = 0;
    if (n <= 0 || cap <= 0) return;
    if (contiguous && n <= cap) { for (int s = 0; s < n; s++) slot[s] = 1 + (bits[s] ? 1 : 0); return; }
    for (int s = 0; s < cap; s++) slot[s] = 1;
    for (int i = 0; i < n && i < 64; i++) if (bits[i]) slot[(i * cap) / n] = 2;
    if (n < cap)   // only the slots a step lands on exist
    {
        int used[64] = {0};
        for (int i = 0; i < n; i++) used[(i * cap) / n] = 1;
        for (int s = 0; s < cap; s++) if (!used[s]) slot[s] = 0;
    }
}
int playSlot(int play, int n, int cap, bool contiguous)
{
    if (play < 0 || play >= n) return -1;
    return contiguous && n <= cap ? play : (play * cap) / n;
}

bool endsWith(const char* k, const char* tail)
{
    const size_t a = std::strlen(k), b = std::strlen(tail);
    return a >= b && !std::strcmp(k + a - b, tail);
}

} // namespace

class EuclidierPlugin : public mpc::Plugin
{
public:
    EuclidierPlugin() : mpc::Plugin(makeInfo())
    {
        const Tables& T = tables();
        for (int i = 0; i < kNumParams; ++i)
        {
            const PSpec& s = g_pspecs[i];
            mpc::Param p = (s.flags & kChoice)
                ? mpc::choiceParam(s.key, s.name, std::vector<std::string>(s.choices, s.choices + s.nchoices), (int)s.def)
                : mpc::intParam(s.key, s.name, (int)s.min, (int)s.max, (int)s.def);
            if (s.flags & (kCell | kStepper | kHidden | kTap)) p.automatable = false;   // display cells, stepper arrows, touch areas and the saved pattern halves stay out of the automation lists
            addParam(p);
        }
        const size_t n = (size_t)kNumParams;
        held_ = new std::atomic<bool>[n];
        release_ = new std::atomic<bool>[n];
        told_ = new std::atomic<float>[n];
        touched_ = new std::atomic<long long>[n];
        cell_ = new std::atomic<float>[n];
        for (size_t i = 0; i < n; ++i) { held_[i].store(false); release_[i].store(false); told_[i].store(get((int)i)); touched_[i].store(0); cell_[i].store(0.0f); }
        iSel_ = T.index.count("sel") ? T.index.at("sel") : -1;
        iView_ = T.index.count("view") ? T.index.at("view") : -1;

        if (!g_log) g_log = std::fopen("/tmp/euclidier_vst.log", "a");
#ifdef EUCLIDIER_TEST
        core_.seed(1);
#else
        core_.seed((uint32_t)nowMs() ^ (uint32_t)(uintptr_t)this);
#endif
        for (auto& d : allDelta_) d.store(0);
        applyDefaults();   // before any thread: the host reads real values from the first moment
        publish();
        for (int i = 0; i < kNumParams; i++)
        {
            int v;
            if (!(g_pspecs[i].flags & (kMomentary | kCell)) && coreGet(g_pspecs[i].key, v))
            {
                store(i, std::min(std::max((float)v, g_pspecs[i].min), g_pspecs[i].max));
                told_[i].store(get(i));
            }
        }
        alsaOpen();
        worker_ = std::thread(&EuclidierPlugin::workerMain, this);
    }

    ~EuclidierPlugin() override
    {
        { std::lock_guard<std::mutex> lk(qmu_); stop_ = true; }
        qcv_.notify_one();
        if (worker_.joinable()) worker_.join();
        core_.stop();   // release whatever still sounds
        flushOut();
        alsaClose();
        delete[] held_;
        delete[] release_;
        delete[] told_;
        delete[] touched_;
        delete[] cell_;
    }

    void prepare(double, int) override {}
    void reset() override {}

    // ---- the audio callback: commands in, the block's ticks from the song position, notes out, snapshot. No audio. ----
    void process(mpc::ProcessContext& ctx) override
    {
        const mpc::Transport& t = ctx.transport;
        const bool playing = t.valid && t.playing;
        playing_.store(playing);
        // The worker holds the core only when no audio has run for a while (see syncCore). If it does right now, leave this block:
        // the song-position grid sends its ticks with the next one.
        if (busy_.exchange(true)) return;
        drain();
        core_.beginBlock(sample_, ctx.numFrames, t.valid && t.tempoValid ? t.bpm : 120.0, ctx.sampleRate);
        for (int i = 0; i < ctx.numMidiEvents; i++)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.size >= 3 && (e.data[0] & 0xF0) == 0x90) core_.noteIn(e.data[1], e.data[2]);
        }
        Ticks sink{this};
        const bool cyc = t.valid && t.cycling && t.cycleEndPpq > t.cycleStartPpq;
        grid_.block(sink, playing, t.valid ? t.ppqPos : 0.0, t.valid ? t.bpm : 120.0, ctx.sampleRate, ctx.numFrames,
                    cyc ? t.cycleStartPpq : 0.0, cyc ? t.cycleEndPpq : 0.0);
        flushOut();
        publish();
        sample_ += ctx.numFrames;
        lastBlockMs_.store(nowMs(), std::memory_order_relaxed);
        busy_.store(false);
    }

    // ---- parameters: any thread. Nothing here touches the core; requests are queued for the worker. ----
    void parameterChanged(int index, float plain) override
    {
        if (index < 0 || index >= kNumParams) return;
        const PSpec& s = g_pspecs[index];
        if (s.flags & kCell)   // display only: the plugin sets these. A write from outside is undone, except a project load, whose
        {                      // cells are the ones its lanes will draw again
            if (loading_.load()) return;
            store(index, cell_[index].load());
            return;
        }
        if (s.flags & kMomentary)
        {
            if (loading_.load()) return;   // a verb is an action, not state: a restore never fires one
            if (plain < 0.5f) { held_[index].store(false); return; }
            if (held_[index].exchange(true)) return;
            fire(index);
            release_[index].store(true);
            return;
        }
        if (loading_.load())
        {
            if (s.flags & kSelMirror) return;   // a copy of the selected lane's value: applying it would write to whatever lane is selected
            told_[index].store(plain);
            queueSet(s.key, std::to_string((int)std::lround(plain)), false);
            return;
        }
        const float was = told_[index].load();
        told_[index].store(plain);           // the host already has this value
        touched_[index].store(nowMs());
        if ((s.flags & kSelMirror) && view_.load() == 8) { allView(index, plain - was); return; }
        if (index == 0) { p0Val_.store((int)std::lround(plain)); p0At_.store(nowMs()); return; }   // debounced in the worker
        apply(index, plain);
    }

    bool formatValue(int index, float, std::string& out) override
    {
        if (index < 0 || index >= kNumParams || !(g_pspecs[index].flags & kMomentary)) return false;
        out = "";
        return true;
    }

    // ---- state ----
    // A project saved by the 1.x build: "key=value;..." in the engine's own units. Called from the wrapped effSetChunk.
    bool loadLegacy(const char* data, size_t size)
    {
        if (!data || size == 0) return false;
        const std::string s(data, strnlen(data, size));
        if (s.find('=') == std::string::npos) return false;
        const Tables& T = tables();
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            const std::string tok = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = tok.substr(0, eq);
            const auto it = T.index.find(key);
            if (it == T.index.end()) continue;
            const PSpec& p = g_pspecs[it->second];
            if (p.flags & (kMomentary | kCell | kSelMirror)) continue;   // old chunks hold "rand_go=0;preset_load=0;..." and the sel_ copies
            const float v = std::min(std::max((float)std::atof(tok.c_str() + eq + 1), p.min), p.max);
            store(it->second, v);
            queueSet(p.key, std::to_string((int)std::lround(v)), false);
            if (!std::strcmp(p.key, "sel") && iView_ >= 0)   // 1.x had no ALL: the lane keys follow the saved lane
            {
                store(iView_, v);
                queueSet("view", std::to_string((int)std::lround(v)), false);
            }
        }
        finishLoad();
        return true;
    }

    void beginLoad() { loading_.store(true); }
    void finishLoad()
    {
        loading_.store(false);
        p0At_.store(0);
        { std::lock_guard<std::mutex> lk(qmu_); wantRefresh_ = true; }   // read back what the engine really holds (it clamps, and fills the sel_ copies)
        qcv_.notify_one();
    }

private:
    // Set a parameter's value without calling parameterChanged. told_ is left alone, so the worker reports the change to the host.
    void store(int i, float plain) { values_[(size_t)i]->store(param(i).toNormalised(plain), std::memory_order_relaxed); }

    void queueSet(const std::string& key, const std::string& val, bool refresh)
    {
        std::lock_guard<std::mutex> lk(qmu_);
        pending_[key] = val;
        if (refresh) wantRefresh_ = true;
        qcv_.notify_one();
    }
    void queueScript(const std::vector<std::pair<std::string, std::string>>& steps)
    {
        std::lock_guard<std::mutex> lk(qmu_);
        script_.insert(script_.end(), steps.begin(), steps.end());
        wantRefresh_ = true;
        qcv_.notify_one();
    }

    int selectedLane() const { return iSel_ >= 0 ? getInt(iSel_) : 0; }

    // Keep the selected lane's "sel_<x>" and "l<N>_<x>" equal: the engine has one value for both.
    void mirror(int i, float plain)
    {
        const char* k = g_pspecs[i].key;
        const int sel = selectedLane();
        std::string other;
        if (g_pspecs[i].flags & kSelMirror) other = "l" + std::to_string(sel + 1) + "_" + (k + 4);
        else if (k[0] == 'l' && k[1] >= '1' && k[1] <= '8' && k[2] == '_' && k[1] - '1' == sel) other = std::string("sel_") + (k + 3);
        else return;
        const auto it = tables().index.find(other);
        if (it != tables().index.end()) store(it->second, plain);
    }

    // A value change from the host (or a stepper arrow): to the engine, and to its twin.
    void apply(int index, float plain)
    {
        const PSpec& s = g_pspecs[index];
        if (index == iView_)   // a lane key, or ALL: the lane keys also pick the lane the sel_ controls act on
        {
            if ((int)plain < 8 && iSel_ >= 0) store(iSel_, plain);
            queueSet("view", std::to_string((int)std::lround(plain)), true);
            return;
        }
        if (index == iSel_ && iView_ >= 0 && view_.load() < 8) store(iView_, plain);
        mirror(index, plain);
        // a new lane re-reads the sel_ copies; a note/drum switch makes the engine pick that mode's note, so re-read that too
        queueSet(s.key, std::to_string((int)std::lround(plain)), index == iSel_ || endsWith(s.key, "_mode"));
    }

    // ---- ALL view: FILL, VELOCITY, HUMANIZE and GATE move every lane by the same amount; nothing else is available ----
    static bool allParam(int p) { return p == euclid::P_FILL || p == euclid::P_GATE || p == euclid::P_VEL || p == euclid::P_VELH; }
    static void allRange(int p, int& lo, int& hi)
    {
        lo = 0; hi = 127;
        if (p == euclid::P_FILL) { lo = 1; hi = 32; }
        else if (p == euclid::P_GATE) { lo = 10; hi = 95; }
        else if (p == euclid::P_VELH) hi = 50;
    }
    void allView(int index, float delta)
    {
        int lane, param;
        laneKey(g_pspecs[index].key, 0, lane, param);
        if (allParam(param))
        {
            allDelta_[param].fetch_add((int)std::lround(delta));
            std::lock_guard<std::mutex> lk(qmu_);
            wantRefresh_ = true;
            qcv_.notify_one();
            return;
        }
        // not available here: put the control back to the lane it shows
        int v;
        if (coreGet(g_pspecs[index].key, v)) store(index, (float)v);
    }
    // A touch area over the step displays was pressed ("tg<lane>_<slot>": one of 32 slices of a lane row; "ct_<k>": one of 32 sectors of
    // the circle). Flip the step that is drawn there, if any.
    void tapArea(const char* key)
    {
        int lane = -1, step = -1;
        if (key[0] == 't')
        {
            int l, slot;
            if (std::sscanf(key, "tg%d_%d", &l, &slot) != 2) return;
            lane = l - 1;
            const int n = snapP_[lane][euclid::P_STEPS].load(std::memory_order_relaxed);
            const int cap = classFor(tables().gcaps, n);   // the row's cells are drawn in this size class: its cell under this slice
            if (cap > 0 && (slot * cap) / 32 < n) step = (slot * cap) / 32;
        }
        else
        {
            int k;
            if (std::sscanf(key, "ct_%d", &k) != 1) return;
            lane = circleLane_.load();
            const int n = snapP_[lane][euclid::P_STEPS].load(std::memory_order_relaxed);
            const int cap = classFor(tables().ccaps, n);
            for (int i = 0; i < n && cap > 0; i++)   // step i is drawn in slot (i*cap)/n, nearest to sector round(slot*32/cap)
                if ((int)std::floor(((i * cap) / n) * 32.0 / cap + 0.5) % 32 == k) { step = i; break; }
        }
        if (lane < 0 || lane > 7 || step < 0) return;
        std::lock_guard<std::mutex> lk(qmu_);
        taps_.push_back({lane, step});
        wantRefresh_ = true;
        qcv_.notify_one();
    }

    // A momentary went 0 -> 1.
    void fire(int index)
    {
        const PSpec& s = g_pspecs[index];
        const Tables& T = tables();
        if (s.flags & kTap) { tapArea(s.key); return; }
        if (s.flags & kStepper)   // an arrow: move another parameter by one
        {
            const int t = s.stepTarget;
            const int nv = std::min((int)g_pspecs[t].max, std::max((int)g_pspecs[t].min, getInt(t) + s.stepDelta));
            store(t, (float)nv);
            touched_[t].store(nowMs());
            apply(t, (float)nv);
            return;
        }
        if (s.flags & kTrigger)   // the engine's own verbs; a bank load or a randomise changes other values
        {
            queueSet(s.key, "1", std::strcmp(s.key, "preset_save") != 0);
            return;
        }
        if (!std::strcmp(s.key, "all_drum"))   // every lane to DRUM mode; the engine then moves each note to its drum note
        {
            for (int l = 1; l <= 8; l++)
            {
                const auto it = T.index.find("l" + std::to_string(l) + "_mode");
                if (it == T.index.end()) continue;
                store(it->second, 1.0f);
                mirror(it->second, 1.0f);
                queueSet(g_pspecs[it->second].key, "1", false);
                // each lane to its own drum note, 36 up to 43 (also a lane that was already in DRUM mode)
                const auto nt = T.index.find("l" + std::to_string(l) + "_note");
                if (nt == T.index.end()) continue;
                store(nt->second, (float)(35 + l));
                mirror(nt->second, (float)(35 + l));
                queueSet(g_pspecs[nt->second].key, std::to_string(35 + l), l == 8);   // one re-read after the last
            }
            return;
        }
        if (!std::strncmp(s.key, "rand_", 5))
        {
            // randomise one lane (rand_1..rand_8, rand_sel = the lane being edited) or all (rand_all): the engine's rand_go acts on
            // the rand_l<N> flags (none set = all), so set the flags, fire, clear them: an ordered script
            int lane = 0;
            if (s.key[5] == 's') lane = selectedLane() + 1;
            else if (s.key[5] >= '1' && s.key[5] <= '8') lane = s.key[5] - '0';
            std::vector<std::pair<std::string, std::string>> st;
            for (int l = 1; l <= 8; l++) st.push_back({"rand_l" + std::to_string(l), l == lane ? "1" : "0"});
            st.push_back({"rand_go", "1"});
            for (int l = 1; l <= 8; l++) st.push_back({"rand_l" + std::to_string(l), "0"});
            queueScript(st);
        }
    }

    // ---- the core: owned by whoever holds busy_ (the audio thread, every block) ----
    struct Cmd { uint8_t op, lane, param; int32_t value; };
    enum { C_SET = 1, C_RAND, C_PATCH, C_TOGGLE, C_PAT };
    static const unsigned kRing = 1024;

    // owner: apply what the worker queued
    void drain()
    {
        unsigned tail = tail_.load(std::memory_order_relaxed);
        const unsigned head = head_.load(std::memory_order_acquire);
        for (; tail != head; tail++)
        {
            const Cmd c = ring_[tail % kRing];
            if (c.op == C_SET) core_.set(c.lane, c.param, c.value);
            else if (c.op == C_RAND) core_.randomize(c.lane);
            else if (c.op == C_PATCH) core_.applyPatch(patchIn_);
            else if (c.op == C_TOGGLE) core_.toggle(c.lane, c.param);
            else if (c.op == C_PAT) core_.setPattern(c.lane, (uint32_t)c.value);
        }
        tail_.store(tail, std::memory_order_release);
    }
    // owner: the values the worker (and through it the host and the skin) reads
    void publish()
    {
        for (int l = 0; l < 8; l++)
        {
            for (int p = 0; p < euclid::P_COUNT; p++) snapP_[l][p].store(core_.get(l, p), std::memory_order_relaxed);
            snapSeq_[l].store(core_.lane[l].seq, std::memory_order_relaxed);
            snapStep_[l].store(core_.lane[l].step, std::memory_order_relaxed);
            snapCustom_[l].store(core_.lane[l].custom ? 1 : 0, std::memory_order_relaxed);
        }
        done_.store(tail_.load(std::memory_order_relaxed), std::memory_order_release);
    }
    // owner: the core's MIDI to our ALSA port, in order (note-offs of this block, then its note-ons)
    void flushOut()
    {
        for (int k = 0; k < core_.outN; k++) alsaSend(core_.out[k]);
        core_.outN = 0;
    }

    // worker: queue a command; a full queue waits for the owner
    void push(uint8_t op, int lane, int param, int value)
    {
        if (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire) >= kRing - 1) syncCore();
        const unsigned head = head_.load(std::memory_order_relaxed);
        ring_[head % kRing] = {op, (uint8_t)lane, (uint8_t)param, (int32_t)value};
        head_.store(head + 1, std::memory_order_release);
    }
    // worker: wait until everything queued is applied and published. The audio thread does that within a block; if no block has
    // run for a while (the host is not processing audio), do it here.
    void syncCore()
    {
        const unsigned want = head_.load(std::memory_order_relaxed);
        for (int k = 0; k < 400 && (int)(done_.load(std::memory_order_acquire) - want) < 0; k++)
        {
            if (nowMs() - lastBlockMs_.load(std::memory_order_relaxed) > 60 && !busy_.exchange(true))
            {
                drain();
                flushOut();
                publish();
                busy_.store(false);
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    static bool laneKey(const char* key, int sel, int& lane, int& param)   // "l<N>_<x>" or "sel_<x>"
    {
        const char* x;
        if (!std::strncmp(key, "sel_", 4)) { lane = sel; x = key + 4; }
        else if (key[0] == 'l' && key[1] >= '1' && key[1] <= '8' && key[2] == '_') { lane = key[1] - '1'; x = key + 3; }
        else return false;
        for (int p = 0; p < euclid::P_COUNT; p++) if (!std::strcmp(x, euclid::laneParamName(p))) { param = p; return true; }
        return false;
    }

    // What the engine's control socket answered to "GET key" (docs/ENGINE.md), from the snapshot. Any thread.
    bool coreGet(const char* key, int& v) const
    {
        const int sel = sel_.load();
        if (!std::strcmp(key, "sel")) { v = sel; return true; }
        if (!std::strcmp(key, "preset")) { v = slotNo_.load(); return true; }
        if (!std::strcmp(key, "rand_lane")) { v = randLane_.load(); return true; }
        if (!std::strncmp(key, "rand_l", 6) && key[6] >= '1' && key[6] <= '8' && !key[7]) { v = (randMask_.load() >> (key[6] - '1')) & 1; return true; }
        if (!std::strcmp(key, "view")) { v = view_.load(); return true; }
        if (key[0] == 'm' && key[1] >= '1' && key[1] <= '8' && key[2] == '_')   // a lane's hand-edited pattern: flag and two halves
        {
            const int l = key[1] - '1';
            const bool on = snapCustom_[l].load(std::memory_order_relaxed) != 0;
            const uint64_t bits = snapSeq_[l].load(std::memory_order_relaxed);
            v = !std::strcmp(key + 3, "on") ? (on ? 1 : 0) : !std::strcmp(key + 3, "a") ? (on ? (int)(bits & 0xFFFF) : 0) : (on ? (int)((bits >> 16) & 0xFFFF) : 0);
            return true;
        }
        int lane, param;
        if (!laneKey(key, sel, lane, param)) return false;
        if (key[0] == 's' && view_.load() == 8 && allParam(param))   // ALL view: the controls show the mean of the eight lanes
        {
            int sum = 0;
            for (int l = 0; l < 8; l++) sum += snapP_[l][param].load(std::memory_order_relaxed);
            v = (sum + 4) / 8;
            return true;
        }
        v = snapP_[lane][param].load(std::memory_order_relaxed);
        return true;
    }

    // "SET key value". Worker only.
    void coreSet(const std::string& key, int v)
    {
        const char* k = key.c_str();
        if (key == "sel") { sel_.store(euclid::limit(v, 0, 7)); if (view_.load() < 8) view_.store(sel_.load()); return; }
        if (key == "view") { view_.store(euclid::limit(v, 0, 8)); if (v < 8) sel_.store(euclid::limit(v, 0, 7)); return; }
        if (k[0] == 'm' && k[1] >= '1' && k[1] <= '8' && k[2] == '_')   // a saved hand-edited pattern: the halves first, then the flag applies it
        {
            const int l = k[1] - '1';
            if (!std::strcmp(k + 3, "a")) patLo_[l] = v & 0xFFFF;
            else if (!std::strcmp(k + 3, "b")) patHi_[l] = v & 0xFFFF;
            else if (v) push(C_PAT, l, 0, (int32_t)((uint32_t)patLo_[l] | ((uint32_t)patHi_[l] << 16)));
            return;
        }
        if (key == "preset") { slotNo_.store(euclid::limit(v, 0, 127)); return; }
        if (key == "rand_lane") { randLane_.store(euclid::limit(v, 0, 10)); return; }
        if (!std::strncmp(k, "rand_l", 6) && k[6] >= '1' && k[6] <= '8' && !k[7])
        {
            const unsigned bit = 1u << (k[6] - '1');
            randMask_.store(v ? randMask_.load() | bit : randMask_.load() & ~bit);
            return;
        }
        if (key == "rand_go")   // the picked lanes; none picked = all
        {
            const unsigned m = randMask_.load() ? randMask_.load() : 0xFFu;
            for (int l = 0; l < 8; l++) if (m & (1u << l)) push(C_RAND, l, 0, 0);
            return;
        }
        if (key == "preset_load") { loadPreset(); return; }
        if (key == "preset_save") { savePreset(); return; }
        int lane, param;
        if (laneKey(k, sel_.load(), lane, param)) push(C_SET, lane, param, euclid::limit(v, 0, 127));
    }

    // ---- presets: the engine's bank file, 128 slots of one EUPATCH, kept in the plugin folder ----
    static std::string bankPath()
    {
        const std::string dir = mpc::detail::computePluginFolder();
        return dir.empty() ? std::string() : dir + "/euclidierBANK.bin";
    }
    EUPATCH snapshotPatch() const
    {
        EUPATCH E;
        for (int i = 0; i < 8; i++)
        {
            lanePatch& p = E.lane[i];
            auto g = [&](int x) { return (unsigned char)snapP_[i][x].load(std::memory_order_relaxed); };
            p.enabled = g(euclid::P_ENABLE) != 0;
            p.steps = g(euclid::P_STEPS); p.pulses = g(euclid::P_FILL); p.shift = g(euclid::P_SHIFT); p.loop = g(euclid::P_LOOP);
            p.gate = g(euclid::P_GATE); p.div = g(euclid::P_DIV); p.note = g(euclid::P_NOTE); p.BV = g(euclid::P_VEL); p.VA = g(euclid::P_VELH);
            p.type = g(euclid::P_MODE) ? 2 : 1;
            p.ch = g(euclid::P_CH);
        }
        return E;
    }
    void loadPreset()
    {
        const std::string path = bankPath();
        FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
        if (!f) return;
        EUPATCH E;
        const bool ok = std::fseek(f, (long)(sizeof(EUPATCH) * (size_t)slotNo_.load()), SEEK_SET) == 0 && std::fread(&E, sizeof E, 1, f) == 1;
        std::fclose(f);
        if (!ok) return;
        syncCore();        // nothing queued may still be reading the last patch
        patchIn_ = E;
        push(C_PATCH, 0, 0, 0);
        syncCore();
    }
    void savePreset()
    {
        const std::string path = bankPath();
        if (path.empty()) return;
        syncCore();
        const EUPATCH E = snapshotPatch();
        FILE* f = std::fopen(path.c_str(), "rb+");
        if (!f)   // a new bank: every slot starts as this patch, as the engine's did
        {
            f = std::fopen(path.c_str(), "wb+");
            if (!f) return;
            for (int i = 0; i < 128; i++) std::fwrite(&E, sizeof E, 1, f);
        }
        if (std::fseek(f, (long)(sizeof(EUPATCH) * (size_t)slotNo_.load()), SEEK_SET) == 0) std::fwrite(&E, sizeof E, 1, f);
        std::fclose(f);
    }

    // ---- worker ----
    // A fresh plugin: every lane in DRUM mode (each lane then has its own General MIDI drum note), lanes 1-4 on with four standard
    // Euclidean patterns, lanes 5-8 off. A saved project's state is applied on top. Constructor only: the core has no other user yet.
    void applyDefaults()
    {
        static const int pat[4][2] = {{16, 4}, {8, 3}, {12, 5}, {16, 5}};   // four on the floor, tresillo, E(5,12), bossa-ish
        for (int l = 0; l < 8; l++)
        {
            core_.set(l, euclid::P_MODE, 1);
            core_.set(l, euclid::P_ENABLE, l < 4 ? 1 : 0);
            if (l >= 4) continue;
            core_.set(l, euclid::P_STEPS, pat[l][0]);
            core_.set(l, euclid::P_FILL, pat[l][1]);
            core_.set(l, euclid::P_SHIFT, 0);
        }
        core_.outN = 0;
    }

    // Read every real value back from the core. A value the host changed a moment ago (a finger still on it, or a request not
    // yet sent) is left alone and read on a later pass.
    void refreshAll()
    {
        bool again = false;
        for (int i = 0; i < kNumParams; i++)
        {
            const PSpec& s = g_pspecs[i];
            if (s.flags & (kMomentary | kCell)) continue;
            if (nowMs() - touched_[i].load() < 300 || (i == 0 && p0At_.load())) { again = true; continue; }
            int cv;
            if (!coreGet(s.key, cv)) continue;
            const float v = std::min(std::max((float)cv, s.min), s.max);
            if (std::lround(v) != std::lround(get(i))) store(i, v);
        }
        if (again) { std::lock_guard<std::mutex> lk(qmu_); wantRefresh_ = true; }
    }

    struct LaneView { int steps = 0, play = -1, enabled = 0, selected = 0; int bits[64] = {0}; };
    // Poll every lane's pattern and set the display cells: 0 hidden, 1 off, 2 on, +2 while the play-head is on it.
    void pollViews()
    {
        const Tables& T = tables();
        for (int n = 0; n < 8; n++)
        {
            LaneView& L = lv_[n];
            L.steps = euclid::limit(snapP_[n][euclid::P_STEPS].load(std::memory_order_relaxed), 2, 64);
            const uint64_t seq = snapSeq_[n].load(std::memory_order_relaxed);
            for (int i = 0; i < 64; i++) L.bits[i] = i < L.steps && ((seq >> i) & 1);
            L.play = snapStep_[n].load(std::memory_order_relaxed);
            L.enabled = snapP_[n][euclid::P_ENABLE].load(std::memory_order_relaxed);
            L.selected = n == sel_.load();
        }
        const bool playing = playing_.load();
        int sel = sel_.load();
        if (view_.load() == 8)   // ALL: the circle shows the longest pattern (of the lanes that are on, if any)
        {
            int best = -1;
            for (int pass = 0; pass < 2 && best < 0; pass++)
                for (int i = 0; i < 8; i++)
                    if ((pass == 1 || lv_[i].enabled) && (best < 0 || lv_[i].steps > lv_[best].steps)) best = i;
            sel = best < 0 ? 0 : best;
        }
        circleLane_.store(sel);
        int gcls[8], gslot[8][64], cslot[64];
        const int ccls = classFor(T.ccaps, lv_[sel].steps);
        for (int n = 0; n < 8; n++)
        {
            gcls[n] = classFor(T.gcaps, lv_[n].steps);
            fillSlots(lv_[n].bits, lv_[n].steps, gcls[n], true, gslot[n]);
            const int ps = playing && lv_[n].enabled ? playSlot(lv_[n].play, lv_[n].steps, gcls[n], true) : -1;
            if (ps >= 0 && gslot[n][ps] && gcls[n] <= 16) gslot[n][ps] += 2;   // the 32-step row has no play-head state
        }
        fillSlots(lv_[sel].bits, lv_[sel].steps, ccls, false, cslot);
        const int cps = playing && lv_[sel].enabled ? playSlot(lv_[sel].play, lv_[sel].steps, ccls, false) : -1;
        if (cps >= 0 && cslot[cps]) cslot[cps] += 2;
        for (int i : T.cells)
        {
            const VInfo& v = T.v[i];
            int val = 0;
            if (v.kind == V_GCELL) val = v.cap == gcls[v.lane] ? gslot[v.lane][v.slot] : 0;
            else if (v.kind == V_CCELL) val = v.cap == ccls ? cslot[v.slot] : 0;
            const float cv = std::min((float)val, g_pspecs[i].max);
            cell_[i].store(cv);
            store(i, cv);
        }
    }

    // Tell the host what changed behind its back: cells first (a lane resize touches ~100), then real values, a bounded number per pass.
    void pushChanges(int budget)
    {
        const Tables& T = tables();
        auto push = [&](int i) {
            const float v = get(i);
            if (v == told_[i].load()) return;
            told_[i].store(v);
            notifyHostParameterChanged(i);
            --budget;
        };
        for (size_t k = 0; k < T.cells.size() && budget > 0; k++) push(T.cells[k]);
        for (int i = 0; i < kNumParams && budget > 0; i++)
            if (!(g_pspecs[i].flags & (kCell | kMomentary))) push(i);
    }

    void workerMain()
    {
        { std::lock_guard<std::mutex> lk(qmu_); wantRefresh_ = true; }
        for (int tick = 0;; tick++)
        {
            {
                std::unique_lock<std::mutex> lk(qmu_);
                qcv_.wait_for(lk, std::chrono::milliseconds(30), [this] { return stop_ || !pending_.empty() || !script_.empty() || wantRefresh_; });
                if (stop_) return;
            }
            pass(tick, 96);
        }
    }

    // One worker pass.
    void pass(int tick, int budget)
    {
        std::vector<std::pair<std::string, std::string>> todo, steps;
        bool refresh;
        {
            std::lock_guard<std::mutex> lk(qmu_);
            todo.assign(pending_.begin(), pending_.end());
            pending_.clear();
            steps.swap(script_);
            refresh = wantRefresh_;
            wantRefresh_ = false;
        }
        // lane 1's switch (parameter 0) counts once it has held ~40 ms
        const long long p0 = p0At_.load();
        if (p0 && nowMs() - p0 >= 40)
        {
            p0At_.store(0);
            const int v = p0Val_.load();
            mirror(0, (float)v);
            todo.push_back({g_pspecs[0].key, std::to_string(v)});
        }
        // The SETs are not independent: `sel` picks the lane the sel_ keys write to, and a lane's mode makes the core choose that
        // mode's note, which would overwrite a note set before it. So: sel, then modes, then everything else.
        // A hand-edited pattern comes last: steps, fill and shift build the pattern again.
        auto prio = [](const std::string& k) {
            if (k == "sel" || k == "view") return 0;
            if (endsWith(k.c_str(), "_mode")) return 1;
            if (k[0] == 'm' && k.size() >= 4 && k[2] == '_') return k[3] == 'o' ? 4 : 3;
            return 2;
        };
        std::stable_sort(todo.begin(), todo.end(), [&](const auto& x, const auto& y) { return prio(x.first) < prio(y.first); });
        for (auto& kv : todo) coreSet(kv.first, std::atoi(kv.second.c_str()));
        for (auto& kv : steps) coreSet(kv.first, std::atoi(kv.second.c_str()));
        int dl[euclid::P_COUNT] = {0};
        bool anyDelta = false;
        for (int q = 0; q < euclid::P_COUNT; q++) if (allParam(q) && (dl[q] = allDelta_[q].exchange(0)) != 0) anyDelta = true;
        std::vector<std::pair<int, int>> taps;
        { std::lock_guard<std::mutex> lk(qmu_); taps.swap(taps_); }
        if (anyDelta)   // ALL view: every lane by the same amount, from the values it has now
        {
            syncCore();
            for (int q = 0; q < euclid::P_COUNT; q++)
            {
                if (!dl[q]) continue;
                int lo, hi;
                allRange(q, lo, hi);
                for (int l = 0; l < 8; l++) push(C_SET, l, q, euclid::limit(snapP_[l][q].load(std::memory_order_relaxed) + dl[q], lo, hi));
            }
        }
        for (auto& tp : taps) push(C_TOGGLE, tp.first, tp.second, 0);
        if (!todo.empty() || !steps.empty() || refresh || anyDelta || !taps.empty()) syncCore();   // what is read next is what those left behind
        if (refresh) refreshAll();
        if (!loading_.load()) pollViews();   // not from half a project
        for (int i = 0; i < kNumParams; ++i)
            if ((g_pspecs[i].flags & kMomentary) && release_[i].exchange(false))
            {
                held_[i].store(false);
                store(i, 0.0f);
                told_[i].store(0.0f);
                notifyHostParameterChanged(i);
            }
        pushChanges(budget);
    }

    // ---- ALSA sequencer: our client and its one port, which the MPC lists as a MIDI input ----
    void alsaOpen()
    {
        unsigned cur = g_slots.load();
        for (;;)
        {
            slot_ = 0;
            while (slot_ < 31 && (cur & (1u << slot_))) ++slot_;
            if (g_slots.compare_exchange_weak(cur, cur | (1u << slot_))) break;
        }
        if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { seq_ = nullptr; return; }
        char name[48];
        if (slot_ == 0) std::snprintf(name, sizeof name, "%s", EUCLIDIER_NAME);
        else std::snprintf(name, sizeof name, "%s %d", EUCLIDIER_NAME, slot_ + 1);
        snd_seq_set_client_name(seq_, name);
        port_ = snd_seq_create_simple_port(seq_, EUCLIDIER_NAME, SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        ELOG("[euclidier] %s: ALSA client %d port %d\n", name, snd_seq_client_id(seq_), port_);
    }
    void alsaClose()
    {
        if (seq_) snd_seq_close(seq_);
        seq_ = nullptr;
        if (slot_ >= 0) g_slots.fetch_and(~(1u << slot_));
        slot_ = -1;
    }
    // owner: one message to whoever is subscribed, now (no queue: the block's notes go out at its start)
    void alsaSend(const euclid::MidiOut& m)
    {
#ifdef EUCLIDIER_TEST
        if (capN_ < kCap) cap_[capN_++] = {sample_, core_.tick, m.status, m.d1, m.d2};
#endif
        if (!seq_ || port_ < 0) return;
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        const int ch = m.status & 15;
        switch (m.status & 0xF0)
        {
        case 0x90: snd_seq_ev_set_noteon(&ev, ch, m.d1, m.d2); break;
        case 0x80: snd_seq_ev_set_noteoff(&ev, ch, m.d1, m.d2); break;
        case 0xB0: snd_seq_ev_set_controller(&ev, ch, m.d1, m.d2); break;
        default: return;
        }
        snd_seq_ev_set_source(&ev, port_);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq_, &ev);
    }
    struct Ticks   // what vst/transport_grid.h calls for the block's clock
    {
        EuclidierPlugin* w;
        void start() { w->count(0); w->core_.start(); }
        void stop() { w->count(1); w->core_.stop(); }
        void songpos(long s16) { w->count(2); w->core_.songpos(s16); }
        void pulse() { w->count(3); w->core_.pulse(); }
    };
    void count(int what) { (void)what;
#ifdef EUCLIDIER_TEST
        ++clock_[what];
#endif
    }

public:
#ifdef EUCLIDIER_TEST   // offline test hooks (tests/host_test.cpp)
    struct Cap { int64_t sample; long long tick; uint8_t status, d1, d2; };   // every message sent: the block's first sample, the tick after it
    static const int kCap = 1 << 16;
    Cap* cap_ = new Cap[kCap];
    int capN_ = 0;
    long clock_[4] = {0, 0, 0, 0};
    long dbgClock(int what) const { return clock_[what]; }
    int dbgGet(const char* key) const { int v; return coreGet(key, v) ? v : -1; }
    int dbgTake(Cap* dst, int max) { const int n = std::min(max, capN_); std::memcpy(dst, cap_, sizeof(Cap) * (size_t)n); capN_ = 0; return n; }
#endif

private:
    int iSel_ = -1, iView_ = -1;
    TransportGrid grid_;

    euclid::Core core_;                                  // owner only
    int64_t sample_ = 0;                                 // samples processed: the core's time
    std::atomic<bool> busy_{false};                      // who owns the core right now
    std::atomic<long long> lastBlockMs_{0};
    Cmd ring_[kRing];                                    // worker -> owner
    std::atomic<unsigned> head_{0}, tail_{0}, done_{0};  // queued, applied, applied and published
    EUPATCH patchIn_;                                    // the preset a C_PATCH applies (one in flight)
    std::atomic<int> snapP_[8][euclid::P_COUNT];         // owner -> everyone: lane values,
    std::atomic<uint64_t> snapSeq_[8];                   // patterns,
    std::atomic<int> snapStep_[8];                       // play-heads
    std::atomic<int> snapCustom_[8];                     // 1 = the pattern was edited by hand
    std::atomic<int> view_{0}, circleLane_{0};           // 0-7 a lane, 8 = ALL; the lane the circle draws
    std::atomic<int> allDelta_[euclid::P_COUNT];         // ALL view: what the knobs moved, not yet applied
    int patLo_[8] = {0}, patHi_[8] = {0};                // worker: a saved pattern while its halves arrive
    std::vector<std::pair<int, int>> taps_;              // lane, step: taps on cells, for the worker (qmu_)
    std::atomic<int> sel_{0}, slotNo_{0}, randLane_{0};  // the selected lane, the preset slot (worker's own values)
    std::atomic<unsigned> randMask_{0};

    std::thread worker_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::unordered_map<std::string, std::string> pending_;         // key -> value, coalesced (rapid Q-Link moves collapse)
    std::vector<std::pair<std::string, std::string>> script_;      // ordered SETs (randomise: flags, go, clear), run after pending_
    bool wantRefresh_ = false, stop_ = false;

    std::atomic<bool> loading_{false}, playing_{false};
    std::atomic<long long> p0At_{0};
    std::atomic<int> p0Val_{0};
    std::atomic<bool>* held_ = nullptr;
    std::atomic<bool>* release_ = nullptr;
    std::atomic<float>* told_ = nullptr;          // the value the host last knew, per parameter
    std::atomic<long long>* touched_ = nullptr;   // when the host last changed it (ms)
    std::atomic<float>* cell_ = nullptr;          // a display cell's value as the plugin last set it
    LaneView lv_[8];                              // worker only

    snd_seq_t* seq_ = nullptr;
    int port_ = -1, slot_ = -1;
};

// The same entry as MPC_PLUGIN_ENTRY, plus a wrapped effSetChunk: an "MPCP" chunk loads with only real lane values reaching the
// engine; a text chunk from the 1.x build is read as before.
namespace {
VstIntPtr (*g_origDispatcher)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) = nullptr;

VstIntPtr euclidierDispatcher(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    if (opcode == effSetChunk && e->object && ptr)
    {
        EuclidierPlugin* p = static_cast<EuclidierPlugin*>(e->object);
        if (value >= 4 && std::memcmp(ptr, "MPCP", 4) == 0)
        {
            p->beginLoad();
            const VstIntPtr r = g_origDispatcher(e, opcode, index, value, ptr, opt);
            p->finishLoad();
            return r;
        }
        return p->loadLegacy(static_cast<const char*>(ptr), (size_t)value) ? 1 : 0;
    }
    return g_origDispatcher(e, opcode, index, value, ptr, opt);
}
} // namespace

#ifdef EUCLIDIER_TEST
extern "C" MPC_EXPORT int euclidier_dbg_get(AEffect* e, const char* key) { return static_cast<EuclidierPlugin*>(e->object)->dbgGet(key); }
extern "C" MPC_EXPORT int euclidier_dbg_take(AEffect* e, void* dst, int max) { return static_cast<EuclidierPlugin*>(e->object)->dbgTake(static_cast<EuclidierPlugin::Cap*>(dst), max); }
extern "C" MPC_EXPORT long euclidier_dbg_clock(AEffect* e, int what) { return static_cast<EuclidierPlugin*>(e->object)->dbgClock(what); }
#endif

extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)
{
    if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) return nullptr;   // host is VST1 only
    AEffect* e = mpc::detail::createEffect(new EuclidierPlugin(), host);
    if (e && !g_origDispatcher) g_origDispatcher = e->dispatcher;
    if (e) e->dispatcher = euclidierDispatcher;
    return e;
}
