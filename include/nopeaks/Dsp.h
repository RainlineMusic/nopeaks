#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nopeaks {

inline constexpr std::size_t kDspBands = 12;
inline constexpr std::size_t kTrackDelaySamples = 8192;
inline constexpr std::size_t kSafetyLatencySamples = 256;
inline constexpr float kToneGainBudgetDb = 6.0f;
inline constexpr float kSideGainBudgetDb = 12.0f;
inline constexpr std::array<float, kDspBands> kBandCentresHz {
    50.0f, 80.0f, 125.0f, 200.0f, 315.0f, 500.0f,
    800.0f, 1250.0f, 2000.0f, 3150.0f, 5000.0f, 10000.0f
};

float sanitizeSample(float value) noexcept;
float dbToGain(float db) noexcept;
float gainToDb(float gain, float floorDb = -120.0f) noexcept;

struct StereoSample {
    float left = 0.0f;
    float right = 0.0f;
};

struct BandStatistics {
    std::array<float, kDspBands> leftPower {};
    std::array<float, kDspBands> rightPower {};
    std::array<float, kDspBands> midPower {};
    std::array<float, kDspBands> sidePower {};
    std::array<float, kDspBands> crossPower {};
    std::uint32_t samples = 0;
};

// RBJ direct-form-II-transposed biquad. All state belongs to the audio thread.
// Coefficients are calculated only during prepare or at bounded control steps.
class Biquad {
public:
    void reset() noexcept;
    void setIdentity() noexcept;
    void setBandpass(double sampleRate, double frequency, double q) noexcept;
    void setPeaking(double sampleRate, double frequency, double q, double gainDb) noexcept;
    float process(float value) noexcept;
private:
    void setCoefficients(double b0, double b1, double b2,
                         double a0, double a1, double a2) noexcept;
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

// Independent unity-peak bandpass analysis, Q=1.4. These powers are not a
// perfect-reconstruction decomposition, nor equal-bandwidth spectral density.
// Compare each band over time/across sources; do not sum them as audio energy.
class BandAnalyzer {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void processSample(float left, float right) noexcept;
    BandStatistics statisticsAndReset() noexcept;
    const std::array<float, kDspBands>& centreFrequencies() const noexcept { return centres_; }
private:
    std::array<Biquad, kDspBands> left_ {}, right_ {};
    std::array<float, kDspBands> centres_ = kBandCentresHz;
    std::array<double, kDspBands> leftPower_ {}, rightPower_ {}, midPower_ {}, sidePower_ {}, crossPower_ {};
    std::uint32_t samples_ = 0;
};

// Broad serial PEQ cuts, Q=0.9. Tone acts equally on L/R; side cuts act only
// on S=(L-R)/2. Targets accept up to 6 dB tone/18 dB side cut per section.
// After smoothing, summed absolute current gains are bounded to 6 dB tone/
// 12 dB side before coefficients update every 32 samples, including during
// band redistribution.
// These budgets bound the frozen filter response, not arbitrary waveform
// peaks during time-varying filtering. The Master safety limiter is separate.
class TonalProcessor {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setTargets(const std::array<float, kDspBands>& toneDb,
                    const std::array<float, kDspBands>& sideDb) noexcept;
    StereoSample processSample(float left, float right) noexcept;
    const std::array<float, kDspBands>& currentToneDb() const noexcept { return toneCurrent_; }
    const std::array<float, kDspBands>& currentSideDb() const noexcept { return sideCurrent_; }
private:
    void updateControl() noexcept;
    double sampleRate_ = 48000.0;
    std::array<Biquad, kDspBands> toneLeft_ {}, toneRight_ {}, side_ {};
    std::array<float, kDspBands> toneTarget_ {}, sideTarget_ {}, toneCurrent_ {}, sideCurrent_ {};
    std::array<bool, kDspBands> toneActive_ {}, sideActive_ {};
    std::uint32_t controlCounter_ = 0;
};

// prepare() may allocate; processSample/reset never allocate. Delay is exact
// and remains present when the plugin bypasses processing.
class StereoDelay {
public:
    void prepare(std::size_t delaySamples = kTrackDelaySamples);
    void reset() noexcept;
    StereoSample processSample(float left, float right) noexcept;
    std::size_t latencySamples() const noexcept { return buffer_.size(); }
private:
    std::vector<StereoSample> buffer_;
    std::size_t position_ = 0;
};

// Stereo-linked SAMPLE-PEAK limiter. Fixed 256-sample delay, 80 ms release,
// sliding-window anticipation. No true-peak/inter-sample ceiling is claimed.
class SamplePeakLimiter {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setCeilingDb(float db) noexcept;
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    StereoSample processSample(float left, float right) noexcept;
    float gainReductionDb() const noexcept { return -gainToDb(gain_); }
    float ceilingGain() const noexcept { return ceiling_; }
    static constexpr std::size_t latencySamples() noexcept { return kSafetyLatencySamples; }
private:
    static constexpr std::size_t kDequeSize = kSafetyLatencySamples + 2;
    std::array<StereoSample, kSafetyLatencySamples> delay_ {};
    std::array<float, kDequeSize> peaks_ {};
    std::array<std::uint64_t, kDequeSize> peakTimes_ {};
    std::size_t delayPosition_ = 0, queueHead_ = 0, queueTail_ = 0;
    std::uint64_t time_ = 0;
    float ceiling_ = 0.96605088f, gain_ = 1.0f;
    double release_ = 0.999739617;
    bool enabled_ = true;
};

struct MeterSnapshot {
    float leftPeakDb = -120.0f, rightPeakDb = -120.0f;
    float leftRmsDb = -120.0f, rightRmsDb = -120.0f;
};

// 300 ms exponentially-weighted RMS and 1.5 s decaying sample peaks.
class StereoMeter {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void processSample(float left, float right) noexcept;
    MeterSnapshot snapshot() const noexcept;
private:
    double rmsCoefficient_ = 0.99993056, peakCoefficient_ = 0.99998611;
    double leftPower_ = 0.0, rightPower_ = 0.0;
    float leftPeak_ = 0.0f, rightPeak_ = 0.0f;
};

} // namespace nopeaks
