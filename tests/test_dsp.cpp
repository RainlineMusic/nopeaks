#include "nopeaks/Dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double pi = 3.141592653589793238462643383279502884;

void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(std::string("DSP: ") + description);
}

void near(double actual, double expected, double tolerance, const char* description) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string("DSP: ") + description
            + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
}

float randomSample(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return float(std::int32_t(state)) / float(std::numeric_limits<std::int32_t>::max());
}

void testUtilitiesAndNeutral() {
    using namespace nopeaks;
    near(dbToGain(-6.0205999f), 0.5, 1.0e-6, "dB conversion");
    near(gainToDb(0.5f), -6.0205999, 1.0e-5, "gain conversion");
    near(gainToDb(0.0f), -120.0, 0.0, "silence dB floor");
    check(sanitizeSample(std::numeric_limits<float>::quiet_NaN()) == 0.0f, "NaN sanitation");
    check(sanitizeSample(std::numeric_limits<float>::infinity()) == 0.0f, "infinity sanitation");
    check(sanitizeSample(1.0e-38f) == 0.0f, "denormal sanitation");
    for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        TonalProcessor processor;
        processor.prepare(rate);
        std::uint32_t rng = 0x57a3a012;
        for (int n = 0; n < 20000; ++n) {
            const auto left = randomSample(rng);
            const auto right = randomSample(rng);
            const auto output = processor.processSample(left, right);
            check(output.left == left && output.right == right, "neutral tone/side must preserve exact L/R samples");
        }
    }
}

void testExactDelay() {
    using namespace nopeaks;
    for (const std::size_t latency : {std::size_t(0), std::size_t(1), std::size_t(256), kTrackDelaySamples}) {
        StereoDelay delay;
        delay.prepare(latency);
        check(delay.latencySamples() == latency, "declared delay");
        for (std::size_t n = 0; n < latency + 100; ++n) {
            const auto value = n == 0 ? 0.75f : 0.0f;
            const auto output = delay.processSample(value, -value);
            const auto expected = n == latency ? 0.75f : 0.0f;
            check(output.left == expected && output.right == -expected, "delay impulse must have exact latency");
        }
        delay.reset();
        for (std::size_t n = 0; n <= latency; ++n) {
            const auto output = delay.processSample(0.0f, 0.0f);
            check(output.left == 0.0f && output.right == 0.0f, "delay reset removes buffered audio");
        }
    }
}

void testAnalysis() {
    using namespace nopeaks;
    BandAnalyzer one, half;
    one.prepare(48000.0);
    half.prepare(48000.0);
    for (int n = 0; n < 48000; ++n) {
        const auto value = float(0.8 * std::sin(2.0 * pi * 500.0 * n / 48000.0));
        one.processSample(value, value);
        half.processSample(value * 0.5f, value * 0.5f);
    }
    const auto a = one.statisticsAndReset();
    const auto b = half.statisticsAndReset();
    check(a.samples == 48000, "analysis frame count");
    check(one.statisticsAndReset().samples == 0, "reading statistics resets count");
    for (std::size_t band = 0; band < kDspBands; ++band) {
        near(a.leftPower[band], a.rightPower[band], 1.0e-12, "mono equal channel powers");
        near(a.midPower[band], a.leftPower[band], 1.0e-12, "mono M power");
        near(a.sidePower[band], 0.0, 0.0, "mono has no S power");
        near(a.crossPower[band], a.leftPower[band], 1.0e-12, "mono positive cross power");
        near(b.leftPower[band], a.leftPower[band] * 0.25, 1.0e-8, "analysis power scales quadratically with gain");
    }
    near(a.leftPower[5], 0.32, 0.002, "unity-peak bandpass at 500 Hz");
    one.reset();
    for (int n = 0; n < 48000; ++n) {
        const auto value = float(0.8 * std::sin(2.0 * pi * 500.0 * n / 48000.0));
        one.processSample(value, -value);
    }
    const auto anti = one.statisticsAndReset();
    for (std::size_t band = 0; band < kDspBands; ++band) {
        near(anti.midPower[band], 0.0, 0.0, "anti-phase has no M power");
        near(anti.sidePower[band], anti.leftPower[band], 1.0e-12, "anti-phase S power");
        near(anti.crossPower[band], -anti.leftPower[band], 1.0e-12, "anti-phase cross power");
        near(anti.leftPower[band] + anti.rightPower[band],
             2.0 * (anti.midPower[band] + anti.sidePower[band]), 1.0e-8, "M/S energy identity");
    }
    for (const double rate : {8000.0, 44100.0, 96000.0, 192000.0}) {
        one.prepare(rate);
        for (const auto center : one.centreFrequencies())
            check(center <= rate * 0.45 + 1.0e-3, "analysis centers below Nyquist guard");
    }
}

void testTonalAndSide() {
    using namespace nopeaks;
    std::array<float, kDspBands> neutral {}, tone {}, side {};
    tone[7] = -6.0f;
    side[7] = -12.0f;
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        TonalProcessor eq, width, mono;
        eq.prepare(rate);
        width.prepare(rate);
        mono.prepare(rate);
        eq.setTargets(tone, neutral);
        width.setTargets(neutral, side);
        mono.setTargets(neutral, side);
        double inputPower = 0.0, tonePower = 0.0, sidePower = 0.0;
        const auto count = int(rate * 1.5);
        for (int n = 0; n < count; ++n) {
            const auto source = float(0.5 * std::sin(2.0 * pi * 1250.0 * n / rate));
            const auto mid = float(0.1 * std::sin(2.0 * pi * 173.0 * n / rate));
            const auto t = eq.processSample(source, source);
            const auto w = width.processSample(mid + source, mid - source);
            const auto m = mono.processSample(mid, mid);
            check(m.left == mid && m.right == mid, "side EQ must leave mono exactly unchanged");
            near(0.5 * (double(w.left) + w.right), mid, 1.0e-7, "side EQ preserves M on stereo input");
            if (n > count - int(rate * 0.25)) {
                inputPower += double(source) * source;
                tonePower += double(t.left) * t.left;
                const auto s = 0.5 * (double(w.left) - w.right);
                sidePower += s * s;
            }
        }
        near(10.0 * std::log10(tonePower / inputPower), -6.0, 0.03, "actual tone PEQ center gain");
        near(10.0 * std::log10(sidePower / inputPower), -12.0, 0.03, "actual side PEQ center gain");
        near(eq.currentToneDb()[7], -6.0, 1.0e-4, "tone smoother reaches target");
        near(width.currentSideDb()[7], -12.0, 1.0e-4, "side smoother reaches target");
    }

    TonalProcessor stress;
    stress.prepare(48000.0);
    std::uint32_t rng = 0x726519;
    for (int n = 0; n < 48000; ++n) {
        if (n % 128 == 0) {
            for (std::size_t band = 0; band < kDspBands; ++band) {
                tone[band] = (n / 128) % 2 ? -100.0f : 20.0f;
                side[band] = (n / 128) % 3 ? -100.0f : std::numeric_limits<float>::quiet_NaN();
            }
            stress.setTargets(tone, side);
        }
        const auto output = stress.processSample(randomSample(rng), randomSample(rng));
        check(std::isfinite(output.left) && std::isfinite(output.right), "rapid modulation remains finite");
        check(std::abs(output.left) < 10.0f && std::abs(output.right) < 10.0f, "rapid modulation remains bounded");
    }
    for (const auto value : stress.currentToneDb()) check(value <= 0.0f && value >= -6.0f, "tone cut clamp");
    for (const auto value : stress.currentSideDb()) check(value <= 0.0f && value >= -18.0f, "side cut clamp");
}

void testAggregateControlBudgets() {
    using namespace nopeaks;
    for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0}) {
        TonalProcessor processor;
        processor.prepare(rate);
        std::array<float, kDspBands> tone {}, side {};
        const auto duration = int(rate * 2.0);
        const auto switchPeriod = std::max(128, int(rate * 0.08));
        for (int n = 0; n < duration; ++n) {
            if (n % switchPeriod == 0) {
                tone.fill(0.0f); side.fill(0.0f);
                const auto phase = (n / switchPeriod) % 3;
                if (phase == 2) {
                    // Untrusted/excessive commands must also stay bounded.
                    tone.fill(-6.0f); side.fill(-18.0f);
                } else {
                    for (std::size_t band = std::size_t(phase); band < kDspBands; band += 2) {
                        tone[band] = -1.0f;
                        side[band] = -2.0f;
                    }
                }
                processor.setTargets(tone, side);
            }
            const auto source = float(0.3 * std::sin(2.0 * pi * 315.0 * n / rate));
            const auto output = processor.processSample(source, -source * 0.7f);
            check(std::isfinite(output.left) && std::isfinite(output.right), "budgeted band redistribution stays finite");
            double toneSum = 0.0, sideSum = 0.0;
            for (const auto gain : processor.currentToneDb()) toneSum += std::abs(double(gain));
            for (const auto gain : processor.currentSideDb()) sideSum += std::abs(double(gain));
            check(toneSum <= double(kToneGainBudgetDb) + 1.0e-6, "current tone gain budget through unequal attack/release");
            check(sideSum <= double(kSideGainBudgetDb) + 2.0e-6, "current side gain budget through unequal attack/release");
        }
    }
}

void testLimiter() {
    using namespace nopeaks;
    for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        SamplePeakLimiter limiter;
        limiter.prepare(rate);
        limiter.setCeilingDb(-1.0f);
        const auto ceiling = limiter.ceilingGain();
        for (std::size_t n = 0; n <= kSafetyLatencySamples; ++n) {
            const auto output = limiter.processSample(n == 0 ? 2.0f : 0.0f, n == 0 ? -4.0f : 0.0f);
            if (n < kSafetyLatencySamples)
                check(output.left == 0.0f && output.right == 0.0f, "limiter exact lookahead latency");
            else {
                near(output.left, ceiling * 0.5, 2.0e-7, "limiter stereo-linked left gain");
                near(output.right, -ceiling, 2.0e-7, "limiter stereo-linked right gain");
            }
        }
        limiter.reset();
        std::uint32_t rng = 0xc2c8f098;
        for (int n = 0; n < 20000; ++n) {
            const auto output = limiter.processSample(8.0f * randomSample(rng), 8.0f * randomSample(rng));
            check(std::abs(output.left) <= ceiling && std::abs(output.right) <= ceiling,
                  "sample ceiling guaranteed on stress signal");
        }
        limiter.reset();
        limiter.setEnabled(false);
        std::array<StereoSample, kSafetyLatencySamples + 1000> source {};
        for (auto& value : source) value = {2.0f * randomSample(rng), 2.0f * randomSample(rng)};
        for (std::size_t n = 0; n < source.size(); ++n) {
            const auto output = limiter.processSample(source[n].left, source[n].right);
            const auto expected = n >= kSafetyLatencySamples ? source[n - kSafetyLatencySamples] : StereoSample {};
            check(output.left == expected.left && output.right == expected.right,
                  "limiter bypass keeps exact delay and unity gain");
        }
    }

    std::array<std::vector<StereoSample>, 2> results;
    std::vector<StereoSample> source(12000);
    std::uint32_t rng = 0x14a671;
    for (auto& sample : source) sample = {2.0f * randomSample(rng), 2.0f * randomSample(rng)};
    for (std::size_t pass = 0; pass < results.size(); ++pass) {
        SamplePeakLimiter limiter;
        limiter.prepare(48000.0);
        const auto blockSize = pass == 0 ? std::size_t(1) : std::size_t(1024);
        results[pass].resize(source.size());
        for (std::size_t begin = 0; begin < source.size(); begin += blockSize)
            for (auto n = begin; n < std::min(source.size(), begin + blockSize); ++n)
                results[pass][n] = limiter.processSample(source[n].left, source[n].right);
    }
    for (std::size_t n = 0; n < source.size(); ++n)
        check(results[0][n].left == results[1][n].left && results[0][n].right == results[1][n].right,
              "limiter independent of host block partition");
}

void testNonFiniteAndMeters() {
    using namespace nopeaks;
    TonalProcessor tone;
    SamplePeakLimiter limiter;
    BandAnalyzer analyzer;
    StereoMeter meter;
    tone.prepare(48000.0);
    limiter.prepare(48000.0);
    analyzer.prepare(48000.0);
    meter.prepare(48000.0);
    for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(), 1.0e-38f}) {
        const auto a = tone.processSample(value, value);
        const auto b = limiter.processSample(value, value);
        analyzer.processSample(value, value);
        meter.processSample(value, value);
        check(std::isfinite(a.left) && std::isfinite(a.right) && std::isfinite(b.left) && std::isfinite(b.right),
              "non-finite input cannot escape audio path");
    }
    for (const auto value : analyzer.statisticsAndReset().leftPower)
        check(std::isfinite(value), "non-finite input cannot escape analysis");
    meter.reset();
    for (int n = 0; n < 240000; ++n) {
        const auto value = float(0.5 * std::sin(2.0 * pi * 1000.0 * n / 48000.0));
        meter.processSample(value, value);
    }
    const auto meters = meter.snapshot();
    near(meters.leftPeakDb, -6.0206, 0.02, "sample peak meter");
    near(meters.leftRmsDb, -9.0309, 0.03, "RMS meter on sine");
    near(meters.leftRmsDb, meters.rightRmsDb, 0.0, "stereo RMS symmetry");
}
} // namespace

void testDsp() {
    testUtilitiesAndNeutral();
    testExactDelay();
    testAnalysis();
    testTonalAndSide();
    testAggregateControlBudgets();
    testLimiter();
    testNonFiniteAndMeters();
}

#ifdef NOPEAKS_DSP_TEST_MAIN
#include <iostream>
int main() {
    try {
        testDsp();
        std::cout << "DSP tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
