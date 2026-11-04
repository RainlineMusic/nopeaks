#include "nopeaks/Dsp.h"
#include "nopeaks/Optimizer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double rate = 48000.0;
constexpr std::size_t frameSize = 128;

void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(std::string("Audio validation: ") + description);
}

float noise(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return float(std::int32_t(state)) / float(std::numeric_limits<std::int32_t>::max());
}

double energy(float value) { return double(value) * value; }

float correlation(const nopeaks::BandStatistics& statistics, std::size_t band) {
    const auto denominator = std::sqrt(double(statistics.leftPower[band]) * statistics.rightPower[band]);
    return denominator > 1.0e-20 ? float(statistics.crossPower[band] / denominator) : 0.0f;
}

nopeaks::BandTrack bandTrack(const nopeaks::BandStatistics& statistics, float priority, bool protect = false) {
    nopeaks::BandTrack result;
    for (std::size_t b = 0; b < nopeaks::kDspBands; ++b) {
        result.power[b] = 0.5f * (statistics.leftPower[b] + statistics.rightPower[b]);
        result.midPower[b] = statistics.midPower[b];
        result.sidePower[b] = statistics.sidePower[b];
    }
    result.priority = priority;
    result.protect = protect;
    return result;
}

// Bound the total possible serial-bell magnitude change. This is a test fixture
// constraint, not a claim that independent power allocation predicts the IIR
// renderer's exact response. Every measured result below uses the actual DSP.
void boundCorrection(std::array<float, 12>& gains, float budgetDb) {
    float sum = 0;
    for (const auto gain : gains) sum += std::abs(gain);
    if (sum > budgetDb)
        for (auto& gain : gains) gain *= budgetDb / sum;
}

void testSyntheticStemPeakRender() {
    using namespace nopeaks;
    constexpr std::size_t count = 48000;
    std::array<std::vector<float>, 3> left, right;
    for (std::size_t t = 0; t < 3; ++t) {
        left[t].resize(count);
        right[t].resize(count);
    }
    // Deterministic kick, harmonic vocal, and a lower-priority guitar-like
    // harmonic bed. This checks signal constraints, never musical preference.
    for (std::size_t n = 0; n < count; ++n) {
        const auto time = double(n) / rate;
        const auto kickTime = std::fmod(time, 0.25);
        const auto kick = kickTime < 0.15
            ? 0.95 * std::exp(-kickTime * 24.0) * std::sin(2.0 * pi * (60.0 * kickTime + 8.0 * kickTime * std::exp(-kickTime * 60.0)))
            : 0.0;
        const auto vocal = 0.25 * std::sin(2.0 * pi * 220.0 * time)
                         + 0.06 * std::sin(2.0 * pi * 440.0 * time);
        const auto guitar = 0.35 * std::sin(2.0 * pi * 110.0 * time + 0.4)
                          + 0.15 * std::sin(2.0 * pi * 330.0 * time + 0.7);
        left[0][n] = right[0][n] = float(kick);
        left[1][n] = right[1][n] = float(vocal);
        left[2][n] = float(guitar);
        right[2][n] = float(0.9 * guitar + 0.04 * std::sin(2.0 * pi * 550.0 * time));
    }
    PeakOptions options;
    options.ceiling = dbToGain(-1.0f);
    options.iterations = 16;
    std::size_t affectedFrames = 0, feasibleFrames = 0;
    float originalPeak = 0.0f, allocatedPeak = 0.0f, safetyPeak = 0.0f;
    SamplePeakLimiter safety;
    safety.prepare(rate);
    safety.setCeilingDb(-1.0f);
    for (std::size_t start = 0; start < count; start += frameSize) {
        std::array<PeakTrackView, 3> views;
        for (std::size_t t = 0; t < views.size(); ++t) {
            views[t].left = left[t].data() + start;
            views[t].right = right[t].data() + start;
            views[t].priority = t == 2 ? 0.15f : 0.9f;
            views[t].protect = t == 1;
        }
        const auto samples = std::min(frameSize, count - start);
        const auto result = solvePeaks(views.data(), views.size(), samples, options);
        check(result.gains[1] == 1.0f, "protected vocal gain must remain exactly one");
        if (result.inputPeak > options.ceiling) {
            ++affectedFrames;
            if (result.feasible) ++feasibleFrames;
        }
        float renderedPeak = 0.0f;
        for (std::size_t n = 0; n < samples; ++n) {
            float dryL = 0.0f, dryR = 0.0f, wetL = 0.0f, wetR = 0.0f;
            for (std::size_t t = 0; t < views.size(); ++t) {
                dryL += left[t][start + n]; dryR += right[t][start + n];
                wetL += result.gains[t] * left[t][start + n];
                wetR += result.gains[t] * right[t][start + n];
            }
            originalPeak = std::max(originalPeak, std::max(std::abs(dryL), std::abs(dryR)));
            renderedPeak = std::max(renderedPeak, std::max(std::abs(wetL), std::abs(wetR)));
            const auto safe = safety.processSample(wetL, wetR);
            safetyPeak = std::max(safetyPeak, std::max(std::abs(safe.left), std::abs(safe.right)));
        }
        allocatedPeak = std::max(allocatedPeak, renderedPeak);
        check(std::abs(renderedPeak - result.predictedPeak) < 1.0e-6f,
              "signed-sample prediction must match actual mixed samples");
    }
    for (std::size_t n = 0; n < kSafetyLatencySamples; ++n) {
        const auto safe = safety.processSample(0.0f, 0.0f);
        safetyPeak = std::max(safetyPeak, std::max(std::abs(safe.left), std::abs(safe.right)));
    }
    check(affectedFrames > 5, "peak fixture must actually exercise summation overshoots");
    check(feasibleFrames * 10 >= affectedFrames * 9, "most fixture constraints must be feasible");
    check(allocatedPeak < originalPeak - 0.01f, "allocation must reduce synthesized mix peak");
    check(safetyPeak <= options.ceiling, "rendered safety output must respect sample ceiling");
    std::cout << "Audio peak render: dry=" << gainToDb(originalPeak)
              << " dBFS, allocated=" << gainToDb(allocatedPeak)
              << " dBFS, safety=" << gainToDb(safetyPeak)
              << " dBFS, feasible=" << feasibleFrames << '/' << affectedFrames << " affected frames\n";
}

void testPriorityToneWithActualFilters() {
    using namespace nopeaks;
    std::array<BandAnalyzer, 2> sources;
    for (auto& source : sources) source.prepare(rate);
    for (int n = 0; n < 48000; ++n) {
        const auto vocal = float(0.3 * std::sin(2.0 * pi * 490.0 * n / rate));
        const auto guitar = float(0.5 * std::sin(2.0 * pi * 520.0 * n / rate + 0.2));
        sources[0].processSample(vocal, vocal);
        sources[1].processSample(guitar, guitar);
    }
    std::array<BandTrack, 2> tracks {
        bandTrack(sources[0].statisticsAndReset(), 1.0f, true),
        bandTrack(sources[1].statisticsAndReset(), 0.1f)
    };
    std::array<float, 12> delta {};
    delta[5] = -1.0f;
    const auto solution = solveTone(tracks.data(), tracks.size(), delta, 3.0f);
    check(solution.gainsDb[0][5] == 0.0f && solution.gainsDb[1][5] < -0.2f,
          "tone allocation must cut the expendable source, preserve vocal");
    auto scaledTracks = tracks;
    for (auto& track : scaledTracks)
        for (auto& power : track.power) power *= 0.01f;
    const auto scaled = solveTone(scaledTracks.data(), scaledTracks.size(), delta, 3.0f);
    check(std::abs(scaled.gainsDb[1][5] - solution.gainsDb[1][5]) < 1.0e-5f,
          "tone allocation must not change when all input levels change equally");

    std::array<TonalProcessor, 2> filters;
    std::array<float, 12> neutral {};
    for (std::size_t t = 0; t < filters.size(); ++t) {
        filters[t].prepare(rate);
        filters[t].setTargets(solution.gainsDb[t], neutral);
    }
    BandAnalyzer before, after;
    before.prepare(rate); after.prepare(rate);
    for (int n = 0; n < 96000; ++n) {
        const auto vocal = float(0.3 * std::sin(2.0 * pi * 490.0 * n / rate));
        const auto guitar = float(0.5 * std::sin(2.0 * pi * 520.0 * n / rate + 0.2));
        const auto a = filters[0].processSample(vocal, vocal);
        const auto b = filters[1].processSample(guitar, guitar);
        check(a.left == vocal && a.right == vocal, "protected source actual waveform must be unchanged");
        before.processSample(vocal + guitar, vocal + guitar);
        after.processSample(a.left + b.left, a.right + b.right);
        if (n == 47999) { before.statisticsAndReset(); after.statisticsAndReset(); }
    }
    const auto dry = before.statisticsAndReset();
    const auto wet = after.statisticsAndReset();
    const auto measuredCut = 10.0 * std::log10(wet.leftPower[5] / dry.leftPower[5]);
    check(measuredCut < -0.7 && measuredCut > -1.4, "actual filtered mixed band must approach requested cut");
    std::cout << "Audio tone render: requested=-1 dB in 500 Hz band, actual=" << measuredCut
              << " dB, protected vocal exact\n";
}

void testWidthAndAntiPhaseLimits() {
    using namespace nopeaks;
    BandAnalyzer vocalAnalysis, guitarAnalysis, masterAnalysis;
    vocalAnalysis.prepare(rate); guitarAnalysis.prepare(rate); masterAnalysis.prepare(rate);
    for (int n = 0; n < 48000; ++n) {
        const auto m = float(0.2 * std::sin(2.0 * pi * 1250.0 * n / rate));
        const auto s = float(0.3 * std::cos(2.0 * pi * 1250.0 * n / rate));
        vocalAnalysis.processSample(m, m);
        guitarAnalysis.processSample(s, -s);
        masterAnalysis.processSample(m + s, m - s);
    }
    const auto vocal = vocalAnalysis.statisticsAndReset();
    const auto guitar = guitarAnalysis.statisticsAndReset();
    const auto master = masterAnalysis.statisticsAndReset();
    std::array<BandTrack, 2> tracks {bandTrack(vocal, 1.0f, true), bandTrack(guitar, 0.1f)};
    const auto solution = solveWidth(tracks.data(), tracks.size(), master.midPower, master.sidePower, 0.0f, 18.0f);
    auto side = solution.gainsDb[1];
    boundCorrection(side, 6.0f);
    std::array<float, 12> neutral {};
    TonalProcessor filter;
    filter.prepare(rate); filter.setTargets(neutral, side);
    BandAnalyzer outputAnalysis;
    outputAnalysis.prepare(rate);
    for (int n = 0; n < 96000; ++n) {
        const auto m = float(0.2 * std::sin(2.0 * pi * 1250.0 * n / rate));
        const auto s = float(0.3 * std::cos(2.0 * pi * 1250.0 * n / rate));
        const auto filtered = filter.processSample(s, -s);
        check(std::abs(filtered.left + filtered.right) < 1.0e-7f, "side-only stem must stay zero in mono sum");
        outputAnalysis.processSample(m + filtered.left, m + filtered.right);
        if (n == 47999) outputAnalysis.statisticsAndReset();
    }
    const auto wet = outputAnalysis.statisticsAndReset();
    const auto beforeCorrelation = correlation(master, 7);
    const auto afterCorrelation = correlation(wet, 7);
    check(beforeCorrelation < -0.3f, "width fixture must actually be negatively correlated");
    check(afterCorrelation > beforeCorrelation + 0.1f, "actual source-side filtering must improve master cross-correlation");
    check(wet.sidePower[7] < master.sidePower[7], "actual side energy must decrease");
    check(std::abs(wet.midPower[7] - master.midPower[7]) < master.midPower[7] * 0.01f,
          "actual master mid energy must be preserved");

    // Pure anti-phase is mathematically unfixable by any finite side gain:
    // L=-R remains true. The optimizer must expose nonzero residual instead of
    // declaring that a finite cut guarantees nonnegative correlation.
    const auto pure = bandTrack(guitar, 0.0f);
    const auto impossible = solveWidth(&pure, 1, guitar.midPower, guitar.sidePower, 0.0f, 18.0f);
    check(impossible.residualDb[7] > 1.0f, "pure anti-phase solution must expose residual risk");
    TonalProcessor pureFilter;
    pureFilter.prepare(rate);
    neutral.fill(0.0f);
    auto pureSide = neutral;
    pureSide[7] = -12.0f;
    pureFilter.setTargets(neutral, pureSide);
    BandAnalyzer pureAfter;
    pureAfter.prepare(rate);
    for (int n = 0; n < 96000; ++n) {
        const auto s = float(0.3 * std::cos(2.0 * pi * 1250.0 * n / rate));
        const auto sample = pureFilter.processSample(s, -s);
        pureAfter.processSample(sample.left, sample.right);
        if (n == 47999) pureAfter.statisticsAndReset();
    }
    const auto pureOutput = pureAfter.statisticsAndReset();
    check(correlation(pureOutput, 7) < -0.9999f, "finite side cut cannot cure pure anti-phase correlation");
    check(pureOutput.sidePower[7] < guitar.sidePower[7] * 0.08f, "finite side cut still reduces anti-phase energy");
    std::cout << "Audio width render: correlation=" << beforeCorrelation << " -> " << afterCorrelation
              << "; pure anti-phase remains=" << correlation(pureOutput, 7) << " with residual reported\n";
}

void testMeasuredAggregateCutBound() {
    using namespace nopeaks;
    std::array<float, 12> tone, neutral {};
    tone.fill(-1.5f);
    boundCorrection(tone, 3.0f);
    float sum = 0.0f;
    for (const auto gain : tone) sum += std::abs(gain);
    check(sum <= 3.00001f, "fixture total cut budget");
    double largestCut = 0.0;
    for (const auto frequency : {50.0, 80.0, 125.0, 200.0, 315.0, 500.0, 800.0,
                                 1250.0, 2000.0, 3150.0, 5000.0, 10000.0}) {
        TonalProcessor filter;
        filter.prepare(rate); filter.setTargets(tone, neutral);
        double inputPower = 0.0, outputPower = 0.0;
        for (int n = 0; n < 72000; ++n) {
            const auto source = float(0.5 * std::sin(2.0 * pi * frequency * n / rate));
            const auto output = filter.processSample(source, source);
            if (n >= 48000) { inputPower += energy(source); outputPower += energy(output.left); }
        }
        const auto cut = -10.0 * std::log10(outputPower / inputPower);
        check(cut >= -0.001 && cut <= 3.001, "actual serial-bell response must stay inside aggregate cut budget");
        largestCut = std::max(largestCut, cut);
    }
    std::cout << "Audio tone response: 3 dB aggregate coefficient budget, largest measured cut=" << largestCut << " dB\n";
}

void benchmarkOptimizerAndAnalysis() {
    using namespace nopeaks;
    constexpr std::size_t tracks = 64;
    constexpr std::size_t frames = 64;
    std::array<std::array<float, frameSize>, tracks> left {}, right {};
    std::array<BandAnalyzer, tracks> analysis;
    std::array<PeakTrackView, tracks> views;
    std::array<BandTrack, tracks> bands;
    std::uint32_t rng = 0x743618;
    for (std::size_t t = 0; t < tracks; ++t) {
        analysis[t].prepare(rate);
        views[t].left = left[t].data(); views[t].right = right[t].data();
        views[t].priority = float(t) / float(tracks - 1);
        views[t].protect = t == 0;
        bands[t].priority = views[t].priority;
        bands[t].protect = views[t].protect;
    }
    std::array<float, 12> delta, mid, side;
    delta.fill(-1.0f); mid.fill(0.03f); side.fill(0.05f);
    PeakOptions options;
    options.iterations = 16;
    double checksum = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t t = 0; t < tracks; ++t) {
            for (std::size_t n = 0; n < frameSize; ++n) {
                left[t][n] = 0.12f * noise(rng);
                right[t][n] = 0.12f * noise(rng);
                analysis[t].processSample(left[t][n], right[t][n]);
            }
            const auto statistics = analysis[t].statisticsAndReset();
            bands[t].power = statistics.leftPower;
            bands[t].midPower = statistics.midPower;
            bands[t].sidePower = statistics.sidePower;
        }
        const auto peaks = solvePeaks(views.data(), tracks, frameSize, options);
        const auto tone = solveTone(bands.data(), tracks, delta, 3.0f);
        const auto width = solveWidth(bands.data(), tracks, mid, side, 0.0f, 12.0f);
        const auto conflicts = conflictCuts(bands.data(), tracks, 0.1f, 1.5f);
        checksum += peaks.predictedPeak + tone.predictedPower[5] + width.predictedPower[5] + conflicts.predictedPower[5];
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto audioDuration = double(frames * frameSize) / rate;
    check(std::isfinite(checksum) && checksum > 0.0, "full optimizer benchmark must produce usable numbers");
    // Timing is evidence, never a portable pass/fail realtime performance gate.
    std::cout << "Audio throughput: 64-track analysis + peak/tone/width/conflict solvers, "
              << audioDuration << " s audio in " << elapsed << " s wall time ("
              << elapsed / audioDuration << " single-thread realtime ratio); excludes JUCE/IPC/UI\n";
}
} // namespace

void testAudioQuality() {
    testSyntheticStemPeakRender();
    testPriorityToneWithActualFilters();
    testWidthAndAntiPhaseLimits();
    testMeasuredAggregateCutBound();
    benchmarkOptimizerAndAnalysis();
}

#ifdef NOPEAKS_AUDIO_QUALITY_TEST_MAIN
int main() {
    try {
        testAudioQuality();
        std::cout << "Audio render tests passed (synthetic signal constraints; no listening-quality claim)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
