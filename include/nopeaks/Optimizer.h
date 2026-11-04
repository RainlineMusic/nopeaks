#pragma once

#include <array>
#include <cstddef>

namespace nopeaks {

// These APIs are allocation-free, deterministic, and independent of the plug-in framework.
// Every calculation that combines stem powers is an explicit additive-power model,
// not a claim that correlated stem powers sum to the measured master power.
struct PeakTrackView {
    const float* left = nullptr;
    const float* right = nullptr; // nullptr means mono: use left for both channels
    float priority = 0.5f;       // 0..1: larger values cost more to change
    bool protect = false;       // hard constraint: gain is exactly one
    float minimumGain = 0.25f;
};

struct PeakOptions {
    float ceiling = 0.891250938f; // -1 dBFS, sample peak
    float amount = 1.0f;
    unsigned iterations = 32;    // hard-capped at 96
};

struct PeakResult {
    std::array<float, 64> gains {};
    float inputPeak = 0.0f;
    float predictedPeak = 0.0f;
    float residual = 0.0f;       // max(0, predictedPeak - ceiling), amplitude
    bool feasible = true;
    unsigned iterations = 0;
};

// Bounded dual-coordinate optimization of weighted squared gain change, with
// signed stereo sample constraints, hard protection and per-stem gain floors.
// count <= 64, samples <= 128; larger sizes are safely truncated.
PeakResult solvePeaks(const PeakTrackView* tracks, std::size_t count,
                      std::size_t samples, const PeakOptions& options = {}) noexcept;

struct BandTrack {
    std::array<float, 12> power {};
    std::array<float, 12> midPower {};
    std::array<float, 12> sidePower {};
    float priority = 0.5f;
    bool protect = false;
};

struct BandResult {
    std::array<std::array<float, 12>, 64> gainsDb {};
    std::array<float, 12> predictedPower {};
    std::array<float, 12> residualDb {};
};

// Cuts only. The requested delta is relative to the additive sum of stem band
// powers. A positive target requests no cut. No implicit spectral target exists.
BandResult solveTone(const BandTrack* tracks, std::size_t count,
                     const std::array<float, 12>& targetDeltaDb,
                     float maxCutDb = 6.0f) noexcept;

// Gated by measured master M/S power. Allocates side-power reduction using a
// stem-power-share approximation. The master must remeasure the actual result:
// coherent/cancelling side sources cannot be inferred from powers alone.
// At minimumCorrelation == 0, M >= S exactly means nonnegative L/R crosspower.
BandResult solveWidth(const BandTrack* tracks, std::size_t count,
                      const std::array<float, 12>& masterMidPower,
                      const std::array<float, 12>& masterSidePower,
                      float minimumCorrelation = 0.0f,
                      float maxCutDb = 18.0f) noexcept;

// Optional masking heuristic: only lower-priority active overlapping stems are
// cut, by at most maxCutDb. This is not a perceptual optimum or trained model.
BandResult conflictCuts(const BandTrack* tracks, std::size_t count,
                        float amount = 0.0f, float maxCutDb = 3.0f) noexcept;

struct RoutingMatch {
    float normalizedResidual = 1.0f; // RMS residual / RMS observation after fitting gain
    float gain = 0.0f;               // least-squares single aggregate gain
    bool trusted = false;           // requires active, non-silent data and low residual
};

// A necessary, never sufficient, routing check: fit one positive aggregate gain.
// A single silent/weak/constant frame cannot authorize cooperative correction.
// Parent must require consecutive checks, timeline alignment, and topology opt-in.
RoutingMatch routingMatch(const float* predictedLeft, const float* predictedRight,
                          const float* observedLeft, const float* observedRight,
                          std::size_t samples, float maximumResidual = 0.08f) noexcept;

} // namespace nopeaks
