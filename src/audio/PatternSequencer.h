#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace studiosp::audio {
// Fixed atomic model; scheduling state below is owned exclusively by the audio thread.
class PatternSequencer {
public:
    struct HostPosition { bool valid = false, playing = false; double bpm = 120, ppq = 0; };
    struct State {
        double bpm = 120, swing = 0;
        int pattern = 0;
        std::array<std::array<uint16_t, 16>, 4> rows{};
    };
    PatternSequencer() { for (auto& pattern : rows) for (auto& row : pattern) row.store(0); }
    void setStep(int pattern, int pad, int step, bool enabled) {
        if (pattern < 0 || pattern >= 4 || pad < 0 || pad >= 16 || step < 0 || step >= 16) return;
        const auto bit = static_cast<uint16_t>(1u << step);
        if (enabled) rows[pattern][pad].fetch_or(bit);
        else rows[pattern][pad].fetch_and(static_cast<uint16_t>(~bit));
    }
    uint16_t row(int pattern, int pad) const { return rows[pattern][pad].load(); }
    void setBpm(double value) { if (std::isfinite(value)) bpm = std::clamp(value, 40.0, 240.0); }
    void setSwing(double value) { if (std::isfinite(value)) swing = std::clamp(value, 0.0, 75.0); }
    double getBpm() const { return bpm.load(); }
    double getSwing() const { return swing.load(); }
    void setHostSync(bool enabled) { hostSync = enabled; ++restart; playing = enabled; }
    bool followsHost() const { return hostSync.load(); }
    bool isArmed() const { return playing.load(); }
    bool hostAvailable() const { return hostValid.load(); }
    double effectiveBpm() const { return followsHost() && hostAvailable() ? hostBpm.load() : getBpm(); }
    void requestPattern(int value) { requested = std::clamp(value, 0, 3); }
    int requestedPattern() const { return requested.load(); }
    int currentPattern() const { return active.load(); }
    int currentStep() const { return displayedStep.load(); }
    bool isPlaying() const { return playing.load() && (!followsHost() || hostPlaying.load()); }
    void start() { ++restart; playing = true; }
    void stop() { playing = false; }
    void reset() { apply(State{}); setHostSync(false); }
    State snapshot() const {
        State state; state.bpm = getBpm(); state.swing = getSwing(); state.pattern = requestedPattern();
        for (int p = 0; p < 4; ++p) for (int pad = 0; pad < 16; ++pad) state.rows[p][pad] = row(p, pad);
        return state;
    }
    void apply(const State& state) {
        stop(); ++restart;
        setBpm(state.bpm); setSwing(state.swing); requestPattern(state.pattern);
        for (int p = 0; p < 4; ++p) for (int pad = 0; pad < 16; ++pad) rows[p][pad] = state.rows[p][pad];
    }
    void prepare() {
        running = false; countdown = 0; nextStep = 0; previousMask = 0; displayedStep = -1;
        hostRunning = false; hostPlaying = false; hostValid = false; lastHostStep = noHostStep;
    }

    // Called once per audio block; process() may be split again by incoming MIDI.
    // Returning true asks the engine to release voices on stop, seek or clock change.
    bool beginBlock(HostPosition position, int samples, double rate) {
        const bool sync = followsHost();
        position.valid = position.valid && std::isfinite(position.bpm) && position.bpm >= 1 && position.bpm <= 1000
            && std::isfinite(position.ppq) && std::abs(position.ppq) <= 1.0e10;
        hostValid = position.valid; if (position.valid) hostBpm = position.bpm;
        if (!sync && !blockHostSync) return false;
        const bool play = sync && playing.load() && position.valid && position.playing;
        const auto generation = restart.load();
        const bool discontinuity = sync != blockHostSync || generation != hostRestart
            || (play && (!hostRunning || std::abs(position.ppq - expectedPpq) > 1.5 * position.bpm / (60 * rate)));
        const bool release = (hostRunning && (!play || discontinuity)) || (running && sync);
        if (discontinuity) { lastHostStep = noHostStep; previousMask = 0; displayedStep = -1; }
        if (!play) { lastHostStep = noHostStep; displayedStep = -1; }
        blockHostSync = sync; hostRunning = play; hostPlaying = play; hostRestart = generation;
        hostPpq = position.ppq; hostIncrement = position.bpm / (60 * rate);
        expectedPpq = position.ppq + samples * hostIncrement;
        if (sync) running = false;
        return release;
    }

    // Emit events at exact offsets, rendering only the spans between them. No allocation or lock.
    template<class Render, class Trigger>
    void process(int count, double sampleRate, Render render, Trigger trigger) {
        if (count <= 0) return;
        if (blockHostSync) { processHost(count, render, trigger); return; }
        const bool play = playing.load(); const auto generation = restart.load();
        if (!play) {
            if (running && previousMask) trigger(0, 0, previousMask);
            prepare(); render(0, count); return;
        }
        if (!running || generation != seenRestart) {
            if (running && previousMask) trigger(0, 0, previousMask);
            countdown = 0; nextStep = 0; previousMask = 0; running = true; seenRestart = generation;
        }
        int offset = 0;
        while (offset < count) {
            if (countdown <= 0) {
                if (nextStep == 0) active = requested.load();
                unsigned mask = 0; const int pattern = active.load();
                for (int pad = 0; pad < 16; ++pad) if (row(pattern, pad) & (1u << nextStep)) mask |= 1u << pad;
                trigger(offset, mask, previousMask); previousMask = mask; displayedStep = nextStep;
                const double sixteenth = sampleRate * 15.0 / bpm.load();
                // Latch swing for each pair so changing it cannot change the pair's duration.
                if ((nextStep & 1) == 0) pairSwing = swing.load() / 100.0;
                countdown += sixteenth * (1.0 + ((nextStep & 1) ? -pairSwing : pairSwing));
                nextStep = (nextStep + 1) % 16;
            }
            const int span = std::min(count - offset, std::max(1, static_cast<int>(std::ceil(countdown))));
            render(offset, span); offset += span; countdown -= span;
        }
    }
private:
    std::array<std::array<std::atomic<uint16_t>, 16>, 4> rows;
    std::atomic<double> bpm{120}, swing{0};
    std::atomic<int> requested{0}, active{0}, displayedStep{-1};
    std::atomic<bool> playing{false};
    std::atomic<unsigned> restart{0};
    unsigned seenRestart = 0, previousMask = 0;
    double countdown = 0, pairSwing = 0;
    int nextStep = 0;
    bool running = false;
    std::atomic<bool> hostSync{false}, hostValid{false}, hostPlaying{false};
    std::atomic<double> hostBpm{120};
    bool blockHostSync = false, hostRunning = false;
    unsigned hostRestart = 0;
    double hostPpq = 0, hostIncrement = 0, expectedPpq = 0;
    static constexpr int64_t noHostStep = INT64_MIN;
    int64_t lastHostStep = noHostStep;
    template<class Render, class Trigger>
    void processHost(int count, Render render, Trigger trigger) {
        if (!hostRunning) { render(0, count); return; }
        const double amount = swing.load() / 100.0;
        int offset = 0;
        while (offset < count) {
            const double ppq = hostPpq + offset * hostIncrement;
            const auto pair = static_cast<int64_t>(std::floor(ppq / 0.5));
            const double oddBeat = pair * 0.5 + 0.25 * (1 + amount);
            int64_t event = pair * 2 + (ppq > oddBeat + 1e-9 ? 2 : 1);
            double beat = event % 2 == 0 ? (pair + 1) * 0.5 : oddBeat;
            if (std::abs(ppq - pair * 0.5) < 1e-9) { event = pair * 2; beat = pair * 0.5; }
            // Ceil prevents an event sounding before the host's musical boundary.
            const int distance = std::max(0, static_cast<int>(std::ceil((beat - ppq) / hostIncrement - 1e-7)));
            const int span = std::min(count - offset, distance);
            if (span > 0) { render(offset, span); offset += span; }
            if (offset >= count) break;
            if (event != lastHostStep) {
                const int step = static_cast<int>((event % 16 + 16) % 16);
                if (step == 0 || lastHostStep == noHostStep) active = requested.load();
                unsigned mask = 0; const int pattern = active.load();
                for (int pad = 0; pad < 16; ++pad) if (row(pattern, pad) & (1u << step)) mask |= 1u << pad;
                trigger(offset, mask, previousMask); previousMask = mask; displayedStep = step; lastHostStep = event;
            }
            // Move past this event before computing the next one, including exact block boundaries.
            render(offset, 1); ++offset;
        }
        hostPpq += count * hostIncrement;
    }
};
}
