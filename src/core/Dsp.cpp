#include "nopeaks/Dsp.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nopeaks {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::uint32_t kControlPeriod = 32;

double validRate(double sampleRate) noexcept {
    return std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 768000.0
        ? sampleRate : 48000.0;
}

double validFrequency(double frequency, double sampleRate) noexcept {
    return std::clamp(std::isfinite(frequency) ? frequency : 1000.0, 10.0, sampleRate * 0.45);
}

double cleanState(double value) noexcept {
    return std::isfinite(value) && std::abs(value) > 1.0e-30 ? value : 0.0;
}

float cutTarget(float value, float minimum) noexcept {
    return std::isfinite(value) ? std::clamp(value, minimum, 0.0f) : 0.0f;
}

float smoothGainDb(float current, float target, double sampleRate,
                   double attackSeconds, double releaseSeconds) noexcept {
    const auto seconds = target < current ? attackSeconds : releaseSeconds;
    const auto coefficient = std::exp(-double(kControlPeriod) / (seconds * sampleRate));
    const auto result = static_cast<float>(target + coefficient * (current - target));
    return std::abs(result - target) <= 1.0e-5f ? target : result;
}

void enforceGainBudget(std::array<float, kDspBands>& gains, float budgetDb) noexcept {
    double total = 0.0;
    for (const auto gain : gains) total += std::abs(double(gain));
    if (total > double(budgetDb)) {
        // A downward-rounded factor keeps the sum conservative after storing
        // each scaled value back to float, even when the source sum is huge.
        const auto factor = std::nextafter(static_cast<float>(double(budgetDb) / total), 0.0f);
        for (auto& gain : gains) gain *= factor;
    }
}
} // namespace

float sanitizeSample(float value) noexcept {
    if (!std::isfinite(value) || std::abs(value) < 1.0e-30f)
        return 0.0f;
    return std::clamp(value, -1.0e6f, 1.0e6f);
}

float dbToGain(float db) noexcept {
    if (!std::isfinite(db))
        return db < 0.0f ? 0.0f : 1.0f;
    return std::pow(10.0f, std::clamp(db, -160.0f, 120.0f) / 20.0f);
}

float gainToDb(float gain, float floorDb) noexcept {
    const auto floor = std::isfinite(floorDb) ? std::clamp(floorDb, -300.0f, 0.0f) : -120.0f;
    if (!std::isfinite(gain) || gain <= 0.0f)
        return floor;
    return std::max(floor, 20.0f * std::log10(gain));
}

void Biquad::reset() noexcept { z1_ = z2_ = 0.0; }

void Biquad::setIdentity() noexcept {
    b0_ = 1.0;
    b1_ = b2_ = a1_ = a2_ = 0.0;
    reset();
}

void Biquad::setCoefficients(double b0, double b1, double b2,
                             double a0, double a1, double a2) noexcept {
    if (!std::isfinite(a0) || std::abs(a0) < 1.0e-20) {
        setIdentity();
        return;
    }
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
}

void Biquad::setBandpass(double sampleRate, double frequency, double q) noexcept {
    sampleRate = validRate(sampleRate);
    const auto w = 2.0 * kPi * validFrequency(frequency, sampleRate) / sampleRate;
    const auto alpha = std::sin(w) / (2.0 * std::clamp(q, 0.1, 10.0));
    setCoefficients(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * std::cos(w), 1.0 - alpha);
}

void Biquad::setPeaking(double sampleRate, double frequency, double q, double gainDb) noexcept {
    if (!std::isfinite(gainDb) || std::abs(gainDb) < 1.0e-8) {
        setIdentity();
        return;
    }
    sampleRate = validRate(sampleRate);
    const auto w = 2.0 * kPi * validFrequency(frequency, sampleRate) / sampleRate;
    const auto alpha = std::sin(w) / (2.0 * std::clamp(q, 0.1, 10.0));
    const auto a = std::pow(10.0, std::clamp(gainDb, -36.0, 12.0) / 40.0);
    setCoefficients(1.0 + alpha * a, -2.0 * std::cos(w), 1.0 - alpha * a,
                    1.0 + alpha / a, -2.0 * std::cos(w), 1.0 - alpha / a);
}

float Biquad::process(float value) noexcept {
    const auto x = double(sanitizeSample(value));
    const auto y = b0_ * x + z1_;
    z1_ = cleanState(b1_ * x - a1_ * y + z2_);
    z2_ = cleanState(b2_ * x - a2_ * y);
    return sanitizeSample(static_cast<float>(y));
}

void BandAnalyzer::prepare(double sampleRate) noexcept {
    sampleRate = validRate(sampleRate);
    for (std::size_t band = 0; band < kDspBands; ++band) {
        centres_[band] = static_cast<float>(validFrequency(kBandCentresHz[band], sampleRate));
        left_[band].setBandpass(sampleRate, centres_[band], 1.4);
        right_[band].setBandpass(sampleRate, centres_[band], 1.4);
    }
    reset();
}

void BandAnalyzer::reset() noexcept {
    for (auto& filter : left_) filter.reset();
    for (auto& filter : right_) filter.reset();
    leftPower_.fill(0.0);
    rightPower_.fill(0.0);
    midPower_.fill(0.0);
    sidePower_.fill(0.0);
    crossPower_.fill(0.0);
    samples_ = 0;
}

void BandAnalyzer::processSample(float left, float right) noexcept {
    for (std::size_t band = 0; band < kDspBands; ++band) {
        const auto l = double(left_[band].process(left));
        const auto r = double(right_[band].process(right));
        const auto m = 0.5 * (l + r);
        const auto s = 0.5 * (l - r);
        leftPower_[band] += l * l;
        rightPower_[band] += r * r;
        midPower_[band] += m * m;
        sidePower_[band] += s * s;
        crossPower_[band] += l * r;
    }
    // The plugin reads each 128-sample frame. Saturation makes misuse bounded.
    if (samples_ < std::numeric_limits<std::uint32_t>::max()) ++samples_;
}

BandStatistics BandAnalyzer::statisticsAndReset() noexcept {
    BandStatistics result;
    result.samples = samples_;
    const auto inverseCount = samples_ != 0 ? 1.0 / double(samples_) : 0.0;
    for (std::size_t band = 0; band < kDspBands; ++band) {
        result.leftPower[band] = static_cast<float>(leftPower_[band] * inverseCount);
        result.rightPower[band] = static_cast<float>(rightPower_[band] * inverseCount);
        result.midPower[band] = static_cast<float>(midPower_[band] * inverseCount);
        result.sidePower[band] = static_cast<float>(sidePower_[band] * inverseCount);
        result.crossPower[band] = static_cast<float>(crossPower_[band] * inverseCount);
    }
    leftPower_.fill(0.0);
    rightPower_.fill(0.0);
    midPower_.fill(0.0);
    sidePower_.fill(0.0);
    crossPower_.fill(0.0);
    samples_ = 0;
    return result;
}

void TonalProcessor::prepare(double sampleRate) noexcept {
    sampleRate_ = validRate(sampleRate);
    reset();
}

void TonalProcessor::reset() noexcept {
    for (auto& filter : toneLeft_) filter.setIdentity();
    for (auto& filter : toneRight_) filter.setIdentity();
    for (auto& filter : side_) filter.setIdentity();
    toneTarget_.fill(0.0f);
    sideTarget_.fill(0.0f);
    toneCurrent_.fill(0.0f);
    sideCurrent_.fill(0.0f);
    toneActive_.fill(false);
    sideActive_.fill(false);
    controlCounter_ = 0;
}

void TonalProcessor::setTargets(const std::array<float, kDspBands>& toneDb,
                                const std::array<float, kDspBands>& sideDb) noexcept {
    for (std::size_t band = 0; band < kDspBands; ++band) {
        toneTarget_[band] = cutTarget(toneDb[band], -6.0f);
        sideTarget_[band] = cutTarget(sideDb[band], -18.0f);
    }
}

void TonalProcessor::updateControl() noexcept {
    for (std::size_t band = 0; band < kDspBands; ++band) {
        toneCurrent_[band] = smoothGainDb(toneCurrent_[band], toneTarget_[band], sampleRate_, 0.060, 0.350);
        sideCurrent_[band] = smoothGainDb(sideCurrent_[band], sideTarget_[band], sampleRate_, 0.025, 0.250);
    }
    // Independent attack/release rates can exceed an aggregate target budget
    // while moving a cut from one band to another. Project the whole current
    // vector before deriving any section's coefficients.
    enforceGainBudget(toneCurrent_, kToneGainBudgetDb);
    enforceGainBudget(sideCurrent_, kSideGainBudgetDb);
    for (std::size_t band = 0; band < kDspBands; ++band) {
        const bool toneActive = toneCurrent_[band] != 0.0f;
        const bool sideActive = sideCurrent_[band] != 0.0f;
        if (toneActive) {
            toneLeft_[band].setPeaking(sampleRate_, kBandCentresHz[band], 0.9, toneCurrent_[band]);
            toneRight_[band].setPeaking(sampleRate_, kBandCentresHz[band], 0.9, toneCurrent_[band]);
        } else if (toneActive_[band]) {
            toneLeft_[band].setIdentity();
            toneRight_[band].setIdentity();
        }
        if (sideActive) {
            side_[band].setPeaking(sampleRate_, kBandCentresHz[band], 0.9, sideCurrent_[band]);
        } else if (sideActive_[band]) {
            side_[band].setIdentity();
        }
        toneActive_[band] = toneActive;
        sideActive_[band] = sideActive;
    }
}

StereoSample TonalProcessor::processSample(float left, float right) noexcept {
    if (controlCounter_ == 0) updateControl();
    controlCounter_ = (controlCounter_ + 1) % kControlPeriod;
    left = sanitizeSample(left);
    right = sanitizeSample(right);
    for (std::size_t band = 0; band < kDspBands; ++band) {
        if (toneActive_[band]) {
            left = toneLeft_[band].process(left);
            right = toneRight_[band].process(right);
        }
    }
    const float originalSide = 0.5f * (left - right);
    float processedSide = originalSide;
    bool hasSideProcessing = false;
    for (std::size_t band = 0; band < kDspBands; ++band) {
        if (sideActive_[band]) {
            processedSide = side_[band].process(processedSide);
            hasSideProcessing = true;
        }
    }
    // Preserve the original floating point L/R path when side EQ is neutral.
    if (hasSideProcessing) {
        const auto difference = processedSide - originalSide;
        left += difference;
        right -= difference;
    }
    return {sanitizeSample(left), sanitizeSample(right)};
}

void StereoDelay::prepare(std::size_t delaySamples) {
    buffer_.assign(delaySamples, StereoSample {});
    position_ = 0;
}

void StereoDelay::reset() noexcept {
    std::fill(buffer_.begin(), buffer_.end(), StereoSample {});
    position_ = 0;
}

StereoSample StereoDelay::processSample(float left, float right) noexcept {
    const StereoSample input {sanitizeSample(left), sanitizeSample(right)};
    if (buffer_.empty()) return input;
    const auto output = buffer_[position_];
    buffer_[position_] = input;
    if (++position_ == buffer_.size()) position_ = 0;
    return output;
}

void SamplePeakLimiter::prepare(double sampleRate) noexcept {
    release_ = std::exp(-1.0 / (0.080 * validRate(sampleRate)));
    reset();
}

void SamplePeakLimiter::reset() noexcept {
    delay_.fill(StereoSample {});
    peaks_.fill(0.0f);
    peakTimes_.fill(0);
    delayPosition_ = queueHead_ = queueTail_ = 0;
    time_ = 0;
    gain_ = 1.0f;
}

void SamplePeakLimiter::setCeilingDb(float db) noexcept {
    ceiling_ = dbToGain(std::isfinite(db) ? std::clamp(db, -30.0f, 0.0f) : -0.3f);
}

StereoSample SamplePeakLimiter::processSample(float left, float right) noexcept {
    left = sanitizeSample(left);
    right = sanitizeSample(right);
    const auto peak = std::max(std::abs(left), std::abs(right));
    const auto oldest = time_ >= kSafetyLatencySamples ? time_ - kSafetyLatencySamples : 0;
    while (queueHead_ != queueTail_ && peakTimes_[queueHead_] < oldest)
        queueHead_ = (queueHead_ + 1) % kDequeSize;
    while (queueHead_ != queueTail_) {
        const auto previous = (queueTail_ + kDequeSize - 1) % kDequeSize;
        if (peaks_[previous] > peak) break;
        queueTail_ = previous;
    }
    peaks_[queueTail_] = peak;
    peakTimes_[queueTail_] = time_;
    queueTail_ = (queueTail_ + 1) % kDequeSize;
    const auto windowPeak = peaks_[queueHead_];
    const auto required = windowPeak > ceiling_
        ? std::nextafter(ceiling_ / windowPeak, 0.0f) : 1.0f;
    gain_ = enabled_ ? std::min(required, static_cast<float>(1.0 - release_ * (1.0 - gain_))) : 1.0f;
    auto output = delay_[delayPosition_];
    delay_[delayPosition_] = {left, right};
    if (++delayPosition_ == delay_.size()) delayPosition_ = 0;
    ++time_;
    output.left *= gain_;
    output.right *= gain_;
    // Rounding guard; the anticipation gain already bounds both channels.
    if (enabled_) {
        output.left = std::clamp(output.left, -ceiling_, ceiling_);
        output.right = std::clamp(output.right, -ceiling_, ceiling_);
    }
    return output;
}

void StereoMeter::prepare(double sampleRate) noexcept {
    sampleRate = validRate(sampleRate);
    rmsCoefficient_ = std::exp(-1.0 / (0.300 * sampleRate));
    peakCoefficient_ = std::exp(-1.0 / (1.500 * sampleRate));
    reset();
}

void StereoMeter::reset() noexcept {
    leftPower_ = rightPower_ = 0.0;
    leftPeak_ = rightPeak_ = 0.0f;
}

void StereoMeter::processSample(float left, float right) noexcept {
    left = sanitizeSample(left);
    right = sanitizeSample(right);
    leftPower_ = rmsCoefficient_ * leftPower_ + (1.0 - rmsCoefficient_) * double(left) * left;
    rightPower_ = rmsCoefficient_ * rightPower_ + (1.0 - rmsCoefficient_) * double(right) * right;
    leftPeak_ = std::max(std::abs(left), static_cast<float>(peakCoefficient_ * leftPeak_));
    rightPeak_ = std::max(std::abs(right), static_cast<float>(peakCoefficient_ * rightPeak_));
}

MeterSnapshot StereoMeter::snapshot() const noexcept {
    return {gainToDb(leftPeak_), gainToDb(rightPeak_),
            gainToDb(static_cast<float>(std::sqrt(std::max(0.0, leftPower_)))),
            gainToDb(static_cast<float>(std::sqrt(std::max(0.0, rightPower_))))};
}

} // namespace nopeaks
