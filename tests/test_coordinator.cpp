#include "nopeaks/Dsp.h"
#include "nopeaks/Optimizer.h"
#include "nopeaks/SharedBus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
void coordinatorCheck(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct CoordinatorNamespace {
    std::uint64_t key = nopeaks::SharedBus::clockNanoseconds() ^ 0x636F6F7264696E61ULL;
    ~CoordinatorNamespace() {
#ifndef _WIN32
        char path[192] {};
        std::snprintf(path, sizeof(path), "/tmp/rainline-nopeaks-v1-%llu-%016llx-1.ipc",
                      static_cast<unsigned long long>(getuid()), static_cast<unsigned long long>(key));
        unlink(path);
#endif
    }
};

// Mirrors the shipped conservative 32-sample boundary envelope. Individual
// gains never exceed the scalar solution; signed cancellation can nevertheless
// make the sum larger, which is why the Master safety limiter remains necessary.
float scheduledGain(nopeaks::SharedBus& bus, nopeaks::TrackHandle track,
                    std::int64_t tag, std::uint64_t epoch, unsigned offset) {
    nopeaks::ControlFrame current, previous, next;
    const float g = bus.readControl(track, tag, epoch, current) ? current.attenuation : 1.0f;
    const float p = bus.readControl(track, tag - 128, epoch, previous) ? previous.attenuation : 1.0f;
    const float n = bus.readControl(track, tag + 128, epoch, next) ? next.attenuation : 1.0f;
    if (offset < 32 && p < g) return std::min(g, p + (g - p) * offset / 32.0f);
    if (offset >= 96 && n < g) return std::min(g, g + (n - g) * (offset - 96) / 32.0f);
    return g;
}

float source(std::size_t track, std::int64_t position) {
    const double n = static_cast<double>(position);
    const auto bed = 0.42 * std::sin(n * (0.027 + 0.003 * track) + 0.17 * track)
                   + 0.12 * std::sin(n * (0.093 + 0.007 * track));
    const auto pulse = position % 2048 >= 32 && position % 2048 < 48 ? 0.35 : 0.0;
    return static_cast<float>(bed + pulse);
}

void testDelayedFrameScheduling() {
    using namespace nopeaks;
    CoordinatorNamespace space;
    SharedBus master(1, space.key);
    coordinatorCheck(master.isOpen(), master.error().c_str());
    const auto owner = master.acquireMaster();
    coordinatorCheck(owner != 0, "coordinator test Master lease unavailable");
    constexpr std::size_t count = 3;
    std::array<std::unique_ptr<SharedBus>, count> trackBus;
    std::array<TrackHandle, count> handles;
    std::array<StereoDelay, count> delay;
    std::array<BandAnalyzer, count> analysis;
    std::array<TonalProcessor, count> tonal;
    // Epochs are instance-local capabilities, not a shared sequence number.
    const std::array<std::uint64_t, count> epochs {2, 17, 9001};
    for (std::size_t t = 0; t < count; ++t) {
        trackBus[t] = std::make_unique<SharedBus>(1, space.key);
        handles[t] = trackBus[t]->claimTrack();
        coordinatorCheck(static_cast<bool>(handles[t]), "coordinator test Track lease unavailable");
        delay[t].prepare(kTrackDelaySamples);
        analysis[t].prepare(48000.0);
        tonal[t].prepare(48000.0);
    }
    SamplePeakLimiter limiter;
    limiter.prepare(48000.0);
    limiter.setCeilingDb(-1.0f);
    constexpr float ceiling = 0.891250938f;
    std::mt19937 random(0x4e504b53);
    unsigned corrected = 0, trusted = 0;
    float incomingPeak = 0.0f, outputPeak = 0.0f;
    std::array<TelemetryFrame, count> received;
    std::array<PeakTrackView, count> views;
    std::array<float, 128> observedL {}, observedR {}, predictedL {}, predictedR {};
    for (std::int64_t block = 0; block < 300; ++block) {
        const auto tag = block * 128;
        observedL.fill(0.0f); observedR.fill(0.0f);
        std::array<std::size_t, count> order {0, 1, 2};
        std::shuffle(order.begin(), order.end(), random);
        // Random independent source callback order cannot alter a future-tagged
        // command because every source retains a full8192-sample audio delay.
        for (const auto t : order) {
            TelemetryFrame frame;
            frame.samplePosition = tag;
            frame.timelineEpoch = epochs[t];
            frame.priority = t == 0 ? 1.0f : (t == 1 ? 0.5f : 0.0f);
            frame.protectedTrack = t == 0;
            // Deliberately split each source block at unrelated boundaries.
            std::size_t n = 0;
            while (n < 128) {
                const auto length = std::min<std::size_t>(1 + random() % 31, 128 - n);
                for (std::size_t k = n; k < n + length; ++k) {
                    const float l = source(t, tag + static_cast<std::int64_t>(k));
                    const float r = 0.85f * l + 0.035f * std::sin(static_cast<float>(tag + k) * 0.071f);
                    analysis[t].processSample(l, r);
                    const auto wet = tonal[t].processSample(l, r);
                    frame.left[k] = wet.left; frame.right[k] = wet.right;
                    const auto delayed = delay[t].processSample(wet.left, wet.right);
                    const auto gain = scheduledGain(*trackBus[t], handles[t],
                        tag - static_cast<std::int64_t>(kTrackDelaySamples), epochs[t], static_cast<unsigned>(k));
                    observedL[k] += delayed.left * gain;
                    observedR[k] += delayed.right * gain;
                }
                n += length;
            }
            const auto statistics = analysis[t].statisticsAndReset();
            for (std::size_t b = 0; b < kBands; ++b) {
                frame.power[b] = (statistics.leftPower[b] + statistics.rightPower[b]) * 0.5f;
                frame.midPower[b] = statistics.midPower[b]; frame.sidePower[b] = statistics.sidePower[b];
                frame.cross[b] = statistics.crossPower[b];
            }
            coordinatorCheck(trackBus[t]->publish(handles[t], frame), "coordinator telemetry publication failed");
        }
        // Data transport and pure solver must use each source's own epoch.
        for (std::size_t t = 0; t < count; ++t) {
            coordinatorCheck(master.readFrame(handles[t], tag, epochs[t], received[t]),
                             "independent Track epochs could not be aligned by sample position");
            views[t] = {received[t].left.data(), received[t].right.data(), received[t].priority,
                        received[t].protectedTrack, 0.0f};
        }
        const auto solution = solvePeaks(views.data(), count, 128, {ceiling, 1.0f, 32});
        coordinatorCheck(solution.feasible, "coordinator feasible source-frame solution not found");
        coordinatorCheck(solution.gains[0] == 1.0f, "coordinator modified hard-protected Track");
        if (solution.gains[1] < 0.999f || solution.gains[2] < 0.999f) ++corrected;
        for (std::size_t t = 0; t < count; ++t) {
            ControlFrame command;
            command.samplePosition = tag; command.timelineEpoch = epochs[t];
            command.attenuation = solution.gains[t];
            coordinatorCheck(master.writeControl(owner, handles[t], command), "coordinator command scheduling failed");
        }
        if (tag >= static_cast<std::int64_t>(kTrackDelaySamples)) {
            predictedL.fill(0.0f); predictedR.fill(0.0f);
            const auto outputTag = tag - static_cast<std::int64_t>(kTrackDelaySamples);
            for (std::size_t t = 0; t < count; ++t) {
                TelemetryFrame historical;
                coordinatorCheck(master.readFrame(handles[t], outputTag, epochs[t], historical),
                                 "lookahead telemetry fell out of bounded history too early");
                for (unsigned n = 0; n < 128; ++n) {
                    const auto gain = scheduledGain(master, handles[t], outputTag, epochs[t], n);
                    predictedL[n] += historical.left[n] * gain;
                    predictedR[n] += historical.right[n] * gain;
                }
            }
            for (std::size_t n = 0; n < 128; ++n)
                coordinatorCheck(std::abs(predictedL[n] - observedL[n]) < 2.0e-6f &&
                                 std::abs(predictedR[n] - observedR[n]) < 2.0e-6f,
                                 "delayed output disagreed with frame-tagged/ramped control reconstruction");
            if (routingMatch(predictedL.data(), predictedR.data(), observedL.data(), observedR.data(), 128).trusted)
                ++trusted;
        }
        for (std::size_t n = 0; n < 128; ++n) {
            incomingPeak = std::max(incomingPeak, std::max(std::abs(observedL[n]), std::abs(observedR[n])));
            const auto output = limiter.processSample(observedL[n], observedR[n]);
            outputPeak = std::max(outputPeak, std::max(std::abs(output.left), std::abs(output.right)));
        }
        master.heartbeatMaster(owner);
    }
    coordinatorCheck(corrected > 20, "coordinator test did not exercise meaningful source peak correction");
    coordinatorCheck(trusted > 200, "validated delayed routing was not consistently recognized");
    coordinatorCheck(incomingPeak > 0.4f && outputPeak <= ceiling + 2.0e-5f,
                     "actual DSP safety ceiling failed after cooperative Track correction");
    TelemetryFrame historical;
    coordinatorCheck(!master.readFrame(handles[0], 0, epochs[0], historical),
                     "bounded coordinator history accepted a long-overwritten source frame");
    ControlFrame old;
    coordinatorCheck(!trackBus[0]->readControl(handles[0], 0, epochs[0], old) && old.attenuation == 1.0f,
                     "long-overwritten scheduled attenuation failed to unity");
    // A seek revisits the same sample address with a different Track epoch.
    TelemetryFrame seek;
    seek.samplePosition = 299 * 128;
    seek.timelineEpoch = epochs[1] + 1;
    seek.left.fill(0.1f); seek.right.fill(0.1f);
    coordinatorCheck(trackBus[1]->publish(handles[1], seek), "new seek epoch could not publish");
    coordinatorCheck(!master.readFrame(handles[1], seek.samplePosition, epochs[1], historical),
                     "seek read accepted the old timeline epoch");
    coordinatorCheck(!trackBus[1]->readControl(handles[1], seek.samplePosition, seek.timelineEpoch, old),
                     "old timeline attenuation leaked into a new seek epoch");
    for (std::size_t t = 0; t < count; ++t) trackBus[t]->releaseTrack(handles[t]);
    master.releaseMaster(owner);
}

void testRoutingTopologyErrors() {
    std::array<float, 128> a {}, b {}, correct {}, wrong {};
    for (std::size_t n = 0; n < 128; ++n) {
        a[n] = 0.5f * std::sin(0.17f * static_cast<float>(n));
        b[n] = 0.4f * std::cos(0.29f * static_cast<float>(n));
        correct[n] = a[n] + b[n];
    }
    coordinatorCheck(nopeaks::routingMatch(correct.data(), nullptr, correct.data(), nullptr, 128).trusted,
                     "correct multistem routing rejected");
    coordinatorCheck(!nopeaks::routingMatch(a.data(), nullptr, correct.data(), nullptr, 128).trusted,
                     "omitted audible Track was accepted by routing residual");
    for (std::size_t n = 0; n < 128; ++n) wrong[n] = 2.0f * a[n] + b[n];
    coordinatorCheck(!nopeaks::routingMatch(wrong.data(), nullptr, correct.data(), nullptr, 128).trusted,
                     "double-counted group/child topology was accepted");
    for (std::size_t n = 0; n < 128; ++n) wrong[n] = a[n] + 0.4f * b[n];
    coordinatorCheck(!nopeaks::routingMatch(correct.data(), nullptr, wrong.data(), nullptr, 128).trusted,
                     "downstream per-Track fader mismatch was accepted");
    for (std::size_t n = 0; n < 128; ++n) wrong[n] = 0.5f * correct[n];
    coordinatorCheck(!nopeaks::routingMatch(correct.data(), nullptr, wrong.data(), nullptr, 128).trusted,
                     "unmodeled aggregate post-insert gain was accepted");
}

void testAnalyzedBandDecisionsOnActualDsp() {
    using namespace nopeaks;
    std::array<BandAnalyzer, 2> analyzers;
    std::array<TonalProcessor, 2> processors;
    std::array<BandTrack, 2> tracks;
    for (std::size_t t = 0; t < 2; ++t) { analyzers[t].prepare(48000); processors[t].prepare(48000); }
    tracks[0].priority = 1.0f; tracks[0].protect = true;
    tracks[1].priority = 0.0f;
    for (unsigned n = 0; n < 48000; ++n) {
        const auto sample = 0.25f * std::sin(static_cast<float>(n) * 0.16362462f); //1250Hz
        for (auto& analyzer : analyzers) analyzer.processSample(sample, sample);
    }
    for (std::size_t t = 0; t < 2; ++t) {
        const auto s = analyzers[t].statisticsAndReset();
        for (std::size_t b = 0; b < kBands; ++b)
            tracks[t].power[b] = 0.5f * (s.leftPower[b] + s.rightPower[b]);
    }
    std::array<float, 12> delta {}, neutral {};
    delta[7] = -2.0f;
    const auto decision = solveTone(tracks.data(), 2, delta);
    coordinatorCheck(decision.gainsDb[0][7] == 0.0f && decision.gainsDb[1][7] < -3.0f,
                     "analyzed tonal priority allocation did not protect vocal-like source");
    processors[0].setTargets(decision.gainsDb[0], neutral);
    processors[1].setTargets(decision.gainsDb[1], neutral);
    double protectedError = 0.0, untreatedPower = 0.0, treatedPower = 0.0;
    for (unsigned n = 0; n < 96000; ++n) {
        const auto sample = 0.25f * std::sin(static_cast<float>(n) * 0.16362462f);
        const auto preserve = processors[0].processSample(sample, sample);
        const auto treat = processors[1].processSample(sample, sample);
        if (n > 48000) {
            protectedError += std::abs(preserve.left - sample);
            untreatedPower += static_cast<double>(sample) * sample;
            treatedPower += static_cast<double>(treat.left) * treat.left;
        }
    }
    coordinatorCheck(protectedError < 1.0e-5 && treatedPower < 0.65 * untreatedPower,
                     "actual tonal DSP did not realize priority-aware source cut");

    // Independently verify side decisions preserve the mono signal exactly.
    tracks[0].protect = false;
    for (auto& track : tracks) track.sidePower.fill(1.0f);
    std::array<float, 12> mid {}, side {};
    mid.fill(1.0f); side.fill(0.5f);
    side[7] = 3.0f;
    const auto width = solveWidth(tracks.data(), 2, mid, side, 0.0f, 12.0f);
    coordinatorCheck(width.gainsDb[0][7] > width.gainsDb[1][7], "analyzed width priority allocation reversed");
    TonalProcessor widthDsp;
    widthDsp.prepare(48000);
    widthDsp.setTargets(neutral, width.gainsDb[1]);
    double midError = 0.0, rawSide = 0.0, processedSide = 0.0;
    for (unsigned n = 0; n < 96000; ++n) {
        const float m = 0.1f * std::sin(0.03f * static_cast<float>(n));
        const float s = 0.3f * std::sin(0.16362462f * static_cast<float>(n));
        const auto output = widthDsp.processSample(m + s, m - s);
        if (n > 48000) {
            const double om = 0.5 * (output.left + output.right);
            const double os = 0.5 * (output.left - output.right);
            midError += std::abs(om - m);
            rawSide += static_cast<double>(s) * s; processedSide += os * os;
        }
    }
    coordinatorCheck(midError / 47999 < 1.0e-7 && processedSide < 0.5 * rawSide,
                     "actual side DSP failed mono preservation or side reduction");
}
} // namespace

void testCoordinator() {
    testDelayedFrameScheduling();
    testRoutingTopologyErrors();
    testAnalyzedBandDecisionsOnActualDsp();
}

#ifdef NOPEAKS_COORDINATOR_TEST_MAIN
int main() { testCoordinator(); }
#endif
