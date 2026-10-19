#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace studiosp::audio {
// Fixed-size atomic model. Timeline, monitor suppression and held notes belong to the audio thread.
class PatternSequencer {
public:
    static constexpr int ticksPerQuarter = 9600, cycleTicks = ticksPerQuarter * 4, stepTicks = ticksPerQuarter / 4;
    struct HostPosition { bool valid = false, playing = false; double bpm = 120, ppq = 0; };
    struct Hit { uint32_t value = 0; uint16_t duration = 0; };
    struct State {
        double bpm = 120, swing = 0;
        int pattern = 0;
        bool quantize = true;
        std::array<std::array<uint16_t, 16>, 4> rows{};
        std::array<std::array<std::array<Hit, 16>, 16>, 4> hits{};
    };
    PatternSequencer() {
        for (auto& pattern : rows) for (auto& row : pattern) row = 0;
        for (auto& pattern : cells) for (auto& pad : pattern) for (auto& cell : pad) cell = 0;
        clearTimeline();
    }
    void setStep(int pattern, int pad, int step, bool enabled) {
        if (pattern < 0 || pattern >= 4 || pad < 0 || pad >= 16 || step < 0 || step >= 16) return;
        const auto bit = static_cast<uint16_t>(1u << step);
        if (enabled) rows[pattern][pad].fetch_or(bit);
        else rows[pattern][pad].fetch_and(static_cast<uint16_t>(~bit));
    }
    uint16_t row(int pattern, int pad) const { return rows[pattern][pad].load(); }
    static int velocity(Hit hit) { const int value = static_cast<int>(hit.value & 127u); return value == 0 ? 127 : value; }
    static bool freeTiming(Hit hit) { return (hit.value & freeFlag) != 0; }
    static int onsetTicks(const State& state, int pattern, int pad, int step) {
        return hitTick(state.hits[pattern][pad][step], step, state.swing);
    }
    static int lengthTicks(const State& state, int pattern, int pad, int step) {
        return hitLength(state.hits[pattern][pad][step], step, state.swing);
    }
    void setBpm(double value) { if (std::isfinite(value)) bpm = std::clamp(value, 40.0, 240.0); }
    void setSwing(double value) { if (std::isfinite(value)) swing = std::clamp(value, 0.0, 75.0); }
    double getBpm() const { return bpm.load(); }
    double getSwing() const { return swing.load(); }
    void setRecording(bool enabled) { recording = enabled; ++recordEpoch; }
    bool isRecording() const { return recording.load(); }
    void setQuantize(bool enabled) { quantize = enabled; }
    bool isQuantized() const { return quantize.load(); }
    void setHostSync(bool enabled) { setRecording(false); hostSync = enabled; ++restart; playing = enabled; }
    bool followsHost() const { return hostSync.load(); }
    bool isArmed() const { return playing.load(); }
    bool hostAvailable() const { return hostValid.load(); }
    double effectiveBpm() const { return followsHost() && hostAvailable() ? hostBpm.load() : getBpm(); }
    void requestPattern(int value) { requested = std::clamp(value, 0, 3); }
    int requestedPattern() const { return requested.load(); }
    int currentPattern() const { return active.load(); }
    int currentStep() const { return displayedStep.load(); }
    bool isPlaying() const { return playing.load() && (!followsHost() || hostPlaying.load()); }
    float eventVelocity(int pad) const { return lastVelocity[pad] / 127.0f; }
    void start() { ++restart; playing = true; }
    void stop() { playing = false; setRecording(false); }
    void reset() { apply(State{}); setHostSync(false); }
    State snapshot() const {
        State state; state.bpm = getBpm(); state.swing = getSwing(); state.pattern = requestedPattern(); state.quantize = isQuantized();
        for (int p = 0; p < 4; ++p) for (int pad = 0; pad < 16; ++pad) {
            state.rows[p][pad] = row(p, pad);
            for (int step = 0; step < 16; ++step) state.hits[p][pad][step] = unpack(cells[p][pad][step].load());
        }
        return state;
    }
    void apply(const State& state) {
        stop(); ++restart; setBpm(state.bpm); setSwing(state.swing); requestPattern(state.pattern); setQuantize(state.quantize);
        for (int p = 0; p < 4; ++p) for (int pad = 0; pad < 16; ++pad) {
            rows[p][pad] = 0;
            for (int step = 0; step < 16; ++step) cells[p][pad][step] = pack(state.hits[p][pad][step]);
            rows[p][pad] = state.rows[p][pad];
        }
    }
    void prepare() {
        running = false; hostPlaying = false; hostValid = false; blockHostSync = false;
        clockTick = 0; clearTimeline();
    }
    bool beginBlock(HostPosition position, int samples, double rate) {
        const bool sync = followsHost();
        position.valid = position.valid && std::isfinite(position.bpm) && position.bpm >= 1 && position.bpm <= 1000
            && std::isfinite(position.ppq) && std::abs(position.ppq) <= 1.0e10;
        hostValid = position.valid; if (position.valid) hostBpm = position.bpm;
        if (!sync && !blockHostSync) { ensureInternal(rate); return false; }
        const bool play = sync && playing.load() && position.valid && position.playing;
        const auto generation = restart.load();
        const bool changed = sync != blockHostSync || generation != seenRestart
            || (play && (!running || std::abs(position.ppq - expectedPpq) > 1.5 * position.bpm / (60 * rate)));
        const bool release = running && (!play || changed);
        if (changed || !play) { clearTimeline(); ++recordEpoch; }
        blockHostSync = sync; running = play; hostPlaying = play; seenRestart = generation;
        clockTick = position.valid ? position.ppq * ticksPerQuarter : 0;
        tickIncrement = position.valid ? position.bpm * ticksPerQuarter / (60 * rate) : 1;
        expectedPpq = position.ppq + samples * position.bpm / (60 * rate);
        if (changed && play) { active = requested.load(); lastEvent = static_cast<int64_t>(std::ceil(clockTick - 1e-7)) - 1; }
        if (!sync) { running = false; ensureInternal(rate); }
        return release;
    }
    // Call after rendering to the incoming MIDI event's exact offset. Monitoring is handled by AudioEngine.
    void recordMidi(int pad, bool down, float value = 1) {
        if (pad < 0 || pad >= 16 || !isRecording() || !running || !playing.load()) return;
        const auto epoch = recordEpoch.load();
        auto& held = heldNotes[pad];
        if (!down) {
            if (held.epoch != epoch || held.pattern < 0) return;
            auto expected = held.cell;
            auto hit = unpack(expected);
            hit.duration = static_cast<uint16_t>(std::clamp(std::llround(clockTick - held.start), 1LL, static_cast<long long>(cycleTicks)));
            cells[held.pattern][pad][held.step].compare_exchange_strong(expected, pack(hit));
            held.pattern = -1; return;
        }
        if (!std::isfinite(value) || value <= 0) return;
        const auto cycle = static_cast<int64_t>(std::floor(clockTick / cycleTicks));
        const double phase = clockTick - cycle * cycleTicks;
        if (phase < tickIncrement + 1e-7) active = requested.load();
        const int pattern = active.load(); const bool snap = isQuantized(); const double amount = getSwing();
        int selectedStep = 0; double best = cycleTicks;
        for (int step = 0; step < 16; ++step) {
            const double target = snap ? static_cast<double>(boundary(step, amount)) : step * stepTicks;
            const double delta = std::remainder(phase - target, static_cast<double>(cycleTicks));
            if (std::abs(delta) < std::abs(best)) { best = delta; selectedStep = step; }
        }
        Hit hit;
        const auto offset = static_cast<int16_t>(snap ? 0 : std::llround(best));
        hit.value = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(value * 127)), 1, 127))
            | (static_cast<uint32_t>(static_cast<uint16_t>(offset)) << 7) | (snap ? 0u : freeFlag);
        const auto cell = pack(hit); cells[pattern][pad][selectedStep] = cell;
        setStep(pattern, pad, selectedStep, true); suppressedCycle[pattern][pad][selectedStep] = cycle;
        gateEnd[pad] = noEvent; activeMask &= ~(1u << pad);
        held = {pattern, selectedStep, epoch, cell, clockTick};
    }
    template<class Render, class Trigger>
    void process(int count, double rate, Render render, Trigger trigger) {
        if (count <= 0) return;
        if (!blockHostSync) {
            if (!playing.load()) {
                if (activeMask) trigger(0, 0, activeMask);
                running = false; clearTimeline(); render(0, count); return;
            }
            ensureInternal(rate);
        }
        if (!running) { render(0, count); return; }
        int offset = 0;
        const double amount = getSwing();
        while (offset < count) {
            const auto lower = std::max(lastEvent + 1, static_cast<int64_t>(std::ceil(clockTick - tickIncrement + 1e-7)));
            auto boundaryIndex = static_cast<int64_t>(std::floor(static_cast<double>(lower) / stepTicks));
            while (boundary(boundaryIndex, amount) < lower) ++boundaryIndex;
            const auto boundaryTime = boundary(boundaryIndex, amount);
            int64_t next = boundaryTime;
            for (int pad = 0; pad < 16; ++pad) if (gateEnd[pad] != noEvent && gateEnd[pad] >= lower) next = std::min(next, gateEnd[pad]);
            const int pattern = active.load();
            for (int pad = 0; pad < 16; ++pad) {
                const auto mask = row(pattern, pad);
                for (int step = 0; step < 16; ++step) if (mask & (1u << step)) {
                    const int phase = hitTick(unpack(cells[pattern][pad][step].load()), step, amount);
                    auto cycle = floorDiv(lower - phase + cycleTicks - 1, cycleTicks);
                    if (cycle == suppressedCycle[pattern][pad][step]) ++cycle;
                    next = std::min(next, cycle * cycleTicks + phase);
                }
            }
            const int distance = static_cast<int>(std::max(0.0, std::ceil((next - clockTick) / tickIncrement - 1e-7)));
            const int span = std::min(count - offset, distance);
            if (span > 0) { render(offset, span); offset += span; clockTick += span * tickIncrement; }
            if (offset == count) break;
            unsigned up = 0, down = 0;
            for (int pad = 0; pad < 16; ++pad) if (gateEnd[pad] != noEvent && gateEnd[pad] <= next) {
                up |= 1u << pad; gateEnd[pad] = noEvent;
            }
            if (next == boundaryTime) {
                const int step = static_cast<int>((boundaryIndex % 16 + 16) % 16);
                if (step == 0) active = requested.load();
                displayedStep = step;
            }
            const int playingPattern = active.load(); const auto cycle = floorDiv(next, cycleTicks);
            const int phase = static_cast<int>(next - cycle * cycleTicks);
            for (int pad = 0; pad < 16; ++pad) {
                const auto mask = row(playingPattern, pad);
                for (int step = 0; step < 16; ++step) if (mask & (1u << step)) {
                    const auto hit = unpack(cells[playingPattern][pad][step].load());
                    if (hitTick(hit, step, amount) == phase && suppressedCycle[playingPattern][pad][step] != cycle) {
                        down |= 1u << pad; lastVelocity[pad] = velocity(hit);
                        gateEnd[pad] = next + hitLength(hit, step, amount);
                    }
                }
            }
            activeMask = (activeMask & ~up) | down;
            trigger(offset, down, up); lastEvent = next;
        }
    }
private:
    static constexpr uint32_t freeFlag = 1u << 23;
    static constexpr int64_t noEvent = std::numeric_limits<int64_t>::min() / 2;
    static int64_t floorDiv(int64_t value, int64_t divisor) { const auto q = value / divisor, r = value % divisor; return q - (r < 0 ? 1 : 0); }
    static int64_t boundary(int64_t step, double amount) {
        return step * stepTicks + ((step & 1) ? static_cast<int64_t>(std::llround(stepTicks * amount / 100.0)) : 0);
    }
    static int hitTick(Hit hit, int step, double amount) {
        const int offset = static_cast<int16_t>((hit.value >> 7) & 0xffffu);
        const int tick = freeTiming(hit) ? step * stepTicks + offset : static_cast<int>(boundary(step, amount));
        return (tick % cycleTicks + cycleTicks) % cycleTicks;
    }
    static int hitLength(Hit hit, int step, double amount) {
        return hit.duration != 0 ? hit.duration : freeTiming(hit) ? stepTicks : static_cast<int>(boundary(step + 1, amount) - boundary(step, amount));
    }
    static uint64_t pack(Hit hit) { return hit.value | (static_cast<uint64_t>(hit.duration) << 32); }
    static Hit unpack(uint64_t cell) { return {static_cast<uint32_t>(cell), static_cast<uint16_t>(cell >> 32)}; }
    void clearTimeline() {
        lastEvent = -1; displayedStep = -1; activeMask = 0; gateEnd.fill(noEvent);
        for (auto& pattern : suppressedCycle) for (auto& pad : pattern) pad.fill(noEvent);
        for (auto& held : heldNotes) held.pattern = -1;
    }
    void ensureInternal(double rate) {
        const auto generation = restart.load();
        if (playing.load() && (!running || generation != seenRestart)) {
            clearTimeline(); clockTick = 0; active = requested.load(); running = true; seenRestart = generation;
        }
        tickIncrement = getBpm() * ticksPerQuarter / (60 * rate);
    }
    std::array<std::array<std::atomic<uint16_t>, 16>, 4> rows;
    std::array<std::array<std::array<std::atomic<uint64_t>, 16>, 16>, 4> cells;
    std::atomic<double> bpm{120}, swing{0}, hostBpm{120};
    std::atomic<int> requested{0}, active{0}, displayedStep{-1};
    std::atomic<bool> playing{false}, recording{false}, quantize{true}, hostSync{false}, hostValid{false}, hostPlaying{false};
    std::atomic<unsigned> restart{0}, recordEpoch{0};
    unsigned seenRestart = 0, activeMask = 0;
    double clockTick = 0, tickIncrement = 1, expectedPpq = 0;
    int64_t lastEvent = -1;
    bool running = false, blockHostSync = false;
    std::array<int64_t, 16> gateEnd{};
    std::array<int, 16> lastVelocity{};
    std::array<std::array<std::array<int64_t, 16>, 16>, 4> suppressedCycle{};
    struct Held { int pattern = -1, step = 0; unsigned epoch = 0; uint64_t cell = 0; double start = 0; };
    std::array<Held, 16> heldNotes{};
};
}
