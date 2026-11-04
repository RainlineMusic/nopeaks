#include "nopeaks/Optimizer.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1.0e-5f) {
    return std::abs(a - b) <= tolerance;
}
}

void testOptimizer() {
    using namespace nopeaks;
    std::array<float, 128> a {}, b {}, r {};
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = 0.8f * std::sin(static_cast<float>(i) * 0.2f);
        b[i] = a[i];
        r[i] = 0.5f * std::cos(static_cast<float>(i) * 0.13f);
    }
    std::array<PeakTrackView, 3> tracks {};
    tracks[0] = {a.data(), nullptr, 1.0f, false, 0.1f};
    tracks[1] = {b.data(), nullptr, 0.0f, false, 0.1f};
    PeakOptions options;
    options.ceiling = 1.0f;
    auto peak = solvePeaks(tracks.data(), 2, 128, options);
    check(peak.feasible, "peak solver must meet feasible coherent-sum ceiling");
    check(peak.predictedPeak <= 1.000003f, "predicted sample peak must be bounded");
    check(peak.gains[0] > peak.gains[1] + 0.1f, "higher priority must receive less peak reduction");
    check(peak.gains[0] <= 1.0f && peak.gains[1] >= 0.1f, "peak gains obey bounds");

    tracks[0].protect = true;
    peak = solvePeaks(tracks.data(), 2, 128, options);
    check(near(peak.gains[0], 1.0f), "hard-protected peak stem cannot change");
    check(peak.feasible, "free stem must resolve protected-stem feasible peak");

    // A negative contributor helps the sum. Reducing it would increase the peak.
    a.fill(2.0f); b.fill(-0.5f);
    tracks[0] = {a.data(), nullptr, 0.0f, false, 0.0f};
    tracks[1] = {b.data(), nullptr, 0.0f, false, 0.0f};
    peak = solvePeaks(tracks.data(), 2, 128, options);
    check(peak.feasible && near(peak.gains[0], 0.75f), "signed peak attribution must retain cancellation");
    check(near(peak.gains[1], 1.0f), "canceling source must not be attenuated");
    a.fill(0.5f); b.fill(-0.5f);
    peak = solvePeaks(tracks.data(), 2, 128, options);
    check(near(peak.gains[0], 1.0f) && near(peak.gains[1], 1.0f), "silent coherent cancellation must be no-op");

    a.fill(2.0f); b.fill(0.5f);
    tracks[0].protect = true;
    peak = solvePeaks(tracks.data(), 2, 128, options);
    check(!peak.feasible && peak.residual > 0.99f, "protected peak above ceiling must report infeasibility");
    check(near(peak.gains[0], 1.0f), "infeasible solve cannot break hard protection");
    tracks[0].protect = false;
    options.amount = 0.0f;
    peak = solvePeaks(tracks.data(), 2, 128, options);
    check(near(peak.gains[0], 1.0f) && near(peak.gains[1], 1.0f), "amount zero preserves original");
    check(!peak.feasible, "partial/zero amount must report remaining peak honestly");
    options.amount = 1.0f;
    a.fill(0.0f); b.fill(0.0f); r.fill(2.0f);
    tracks[0] = {a.data(), r.data(), 0.5f, false, 0.0f};
    peak = solvePeaks(tracks.data(), 1, 128, options);
    check(peak.feasible && near(peak.gains[0], 0.5f), "peak constraint must include right channel");
    a[0] = std::numeric_limits<float>::quiet_NaN();
    a[1] = std::numeric_limits<float>::infinity();
    peak = solvePeaks(tracks.data(), 1, 128, options);
    check(std::isfinite(peak.predictedPeak) && std::isfinite(peak.gains[0]), "nonfinite telemetry must not poison solver");
    peak = solvePeaks(nullptr, 64, 128, options);
    check(peak.feasible && near(peak.gains[0], 1.0f), "missing telemetry yields unity");

    std::array<BandTrack, 3> bands {};
    bands[0].priority = 1.0f;
    bands[1].priority = 0.0f;
    bands[0].power.fill(1.0f); bands[1].power.fill(1.0f);
    bands[0].sidePower.fill(2.0f); bands[1].sidePower.fill(2.0f);
    std::array<float, 12> target {}, mid {}, side {};
    target.fill(-3.0f);
    auto tone = solveTone(bands.data(), 2, target);
    check(tone.gainsDb[0][0] > tone.gainsDb[1][0], "tonal cuts must prefer low-priority stem");
    check(tone.gainsDb[1][0] >= -6.00001f, "tonal gain floor must be respected");
    check(tone.residualDb[0] < 0.0001f, "feasible additive tonal target must be reached");
    check(near(tone.predictedPower[0], 2.0f * std::pow(10.0f, -0.3f)), "tonal power uses ten-log, not twenty-log");
    const auto referenceCut = tone.gainsDb[0][0];
    bands[0].power.fill(1000.0f); bands[1].power.fill(1000.0f);
    tone = solveTone(bands.data(), 2, target);
    check(near(tone.gainsDb[0][0], referenceCut), "tonal allocation must be level invariant");
    bands[0].protect = true;
    target.fill(-6.0f);
    tone = solveTone(bands.data(), 2, target);
    check(near(tone.gainsDb[0][0], 0.0f) && tone.residualDb[0] > 0.1f,
          "protected tonal target infeasibility must be visible");
    target.fill(3.0f);
    tone = solveTone(bands.data(), 2, target);
    check(near(tone.gainsDb[0][0], 0.0f) && near(tone.gainsDb[1][0], 0.0f), "cut-only solver must not boost");

    bands[0].protect = false;
    mid.fill(1.0f); side.fill(4.0f);
    auto width = solveWidth(bands.data(), 2, mid, side);
    check(width.gainsDb[0][0] > width.gainsDb[1][0], "side cuts must prefer low priority");
    check(width.predictedPower[0] <= 1.00001f && width.residualDb[0] < 0.0001f,
          "width allocation must reach M>=S in its declared additive model");
    check(width.gainsDb[1][0] >= -18.00001f, "side gain floor must be respected");
    bands[0].protect = true;
    width = solveWidth(bands.data(), 2, mid, side);
    check(near(width.gainsDb[0][0], 0.0f) && width.residualDb[0] > 0.0f,
          "width protection must remain hard when target cannot be achieved");
    mid.fill(0.0f); side.fill(4.0f);
    width = solveWidth(bands.data(), 2, mid, side);
    check(width.predictedPower[0] > 0.0f && width.residualDb[0] > 60.0f && std::isfinite(width.residualDb[0]),
          "pure anti-phase source must report finite nonzero residual when side cannot reach zero");
    mid.fill(1.0f); side.fill(0.5f);
    width = solveWidth(bands.data(), 2, mid, side);
    check(near(width.gainsDb[0][0], 0.0f) && near(width.gainsDb[1][0], 0.0f),
          "nonnegative measured master correlation must not trigger side cuts");
    side.fill(0.0f); mid.fill(0.0f);
    width = solveWidth(bands.data(), 2, mid, side);
    check(near(width.gainsDb[1][0], 0.0f), "silent master must not trigger width control");

    bands[0].protect = false;
    bands[0].power.fill(1.0f); bands[1].power.fill(1.0f);
    auto conflicts = conflictCuts(bands.data(), 2, 1.0f);
    check(near(conflicts.gainsDb[0][0], 0.0f) && conflicts.gainsDb[1][0] < 0.0f,
          "masking heuristic must preserve higher-priority source");
    bands[1].priority = 1.0f;
    conflicts = conflictCuts(bands.data(), 2, 1.0f);
    check(near(conflicts.gainsDb[1][0], 0.0f), "equal priority cannot invent a masking winner");
    conflicts = conflictCuts(bands.data(), 2);
    check(near(conflicts.gainsDb[0][0], 0.0f), "masking heuristic default is off");

    for (std::size_t i = 0; i < 128; ++i) {
        a[i] = 0.3f * std::sin(static_cast<float>(i) * 0.2f);
        b[i] = a[i];
        r[i] = 0.3f * std::cos(static_cast<float>(i) * 0.37f);
    }
    auto routing = routingMatch(a.data(), nullptr, b.data(), nullptr, 128);
    check(routing.trusted && routing.normalizedResidual < 1.0e-6f && near(routing.gain, 1.0f),
          "matching active waveforms must pass routing residual test");
    routing = routingMatch(a.data(), nullptr, r.data(), nullptr, 128);
    check(!routing.trusted, "unrelated signals must fail routing check");
    for (std::size_t i = 0; i < 128; ++i) b[i] = a[i] * 2.0f;
    routing = routingMatch(a.data(), nullptr, b.data(), nullptr, 128);
    check(!routing.trusted && near(routing.gain, 2.0f), "unexpected downstream gain must fail routing check");
    a.fill(0.0f); b.fill(0.0f);
    check(!routingMatch(a.data(), nullptr, b.data(), nullptr, 128).trusted, "silence is not routing evidence");
    a.fill(0.3f); b.fill(0.3f);
    check(!routingMatch(a.data(), nullptr, b.data(), nullptr, 128).trusted, "constant frame is weak routing evidence");
    r.fill(-0.3f);
    check(!routingMatch(a.data(), r.data(), b.data(), r.data(), 128).trusted,
          "opposite-channel DC is also weak routing evidence");
}

#ifdef NOPEAKS_OPTIMIZER_TEST_MAIN
int main() { testOptimizer(); }
#endif
