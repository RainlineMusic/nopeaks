#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "nopeaks/Dsp.h"
#include "nopeaks/Optimizer.h"
#include "nopeaks/SharedBus.h"
#include <atomic>
#include <mutex>
#include <vector>

#ifndef NOPEAKS_IS_MASTER
#define NOPEAKS_IS_MASTER 1
#endif

struct UiTrack {
    juce::String name;
    float priority = 0.5f, peakDb = -100.0f, toneReduction = 0.0f, sideReduction = 0.0f;
    bool protect = false, connected = false;
};
struct UiState {
    float inputPeak = 0, outputPeak = 0, gainReduction = 0;
    std::array<float, 12> bandDb{}, toneDb{}, sideDb{}, targetDb{};
    int connectedTracks = 0, trackCount = 0;
    bool masterConnected = false, routingValid = false, duplicateMaster = false;
    bool targetCaptured = false, targetCapturing = false, transportAvailable = false;
    juce::String connectionError;
    std::array<UiTrack, 64> tracks{};
};

class NoPeaksAudioProcessor final : public juce::AudioProcessor, private juce::Timer {
public:
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    explicit NoPeaksAudioProcessor(bool master = NOPEAKS_IS_MASTER != 0);
    ~NoPeaksAudioProcessor() override;
    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    bool isMaster() const noexcept { return master_; }
    UiState getUiState() const;
    void captureTarget() noexcept { captureRequested_.store(true); }
    void resetTarget() noexcept { targetCaptured_.store(false); resetRequested_.store(true); }
    void setTrackName(const juce::String&);
    juce::String getTrackName() const;
    juce::AudioProcessorValueTreeState parameters;

private:
    struct Connection {
        std::unique_ptr<nopeaks::SharedBus> bus;
        nopeaks::TrackHandle track;
        std::atomic<std::uint64_t> masterToken { 0 };
        int group = 1, session = 1;
        std::uint64_t generation = 0;
        ~Connection();
    };
    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();
    float parameter(const char*) const noexcept;
    void timerCallback() override;
    void refreshConnection();
    void resetTimeline(std::int64_t);
    void trackSample(float&, float&, std::int64_t, Connection*);
    void masterSample(float&, float&, std::int64_t, Connection*);
    void completeMasterFrame(Connection*);
    void coordinate(Connection*);
    void updateRouting(Connection*);
    bool getAlignedFrames(Connection*, std::int64_t, std::uint64_t);
    float commandGain(Connection*, nopeaks::TrackHandle, std::int64_t,
                      std::uint64_t, unsigned) const noexcept;
    static std::int64_t frameStart(std::int64_t) noexcept;
    void publishBands(const nopeaks::BandStatistics&);

    const bool master_;
    std::array<std::atomic<float>*,15> parameterValues_{};
    mutable std::mutex setupMutex_;
    mutable juce::CriticalSection nameLock_;
    juce::String trackName_ { "Track" };
    std::vector<std::unique_ptr<Connection>> connections_;
    std::atomic<Connection*> connection_ { nullptr };
    std::atomic<Connection*> audioHazard_ { nullptr };
    std::uint64_t connectionGeneration_ = 0, audioGeneration_ = 0;
    Connection* audioConnection_ = nullptr;
    nopeaks::BandAnalyzer analyzer_;
    nopeaks::BandStatistics smoothedStatistics_{};
    nopeaks::TonalProcessor tonal_;
    nopeaks::StereoDelay delay_;
    nopeaks::SamplePeakLimiter limiter_;
    nopeaks::TelemetryFrame frame_;
    std::array<nopeaks::TrackInfo, 64> trackInfos_{};
    std::array<nopeaks::TelemetryFrame, 64> workFrames_{};
    std::array<nopeaks::PeakTrackView, 64> peakViews_{};
    std::array<nopeaks::BandTrack, 64> bandTracks_{};
    std::array<float, 128> predictedLeft_{}, predictedRight_{};
    std::array<float, 12> slowTone_{}, slowSide_{};
    std::array<std::array<float,12>,64> widthState_{};
    std::array<std::uint64_t,64> widthTokens_{};
    std::array<float,12> masterMid_{}, masterSide_{}, lastAggregateDb_{};
    std::array<float,12> capturePower_{};
    unsigned captureFramesRemaining_ = 0;
    std::array<std::atomic<float>,12> uiBands_{}, uiLinkedBands_{}, uiTone_{}, uiSide_{}, targetDb_{};
    std::atomic<float> uiInput_{0}, uiOutput_{0}, uiGr_{0};
    std::atomic<bool> targetCaptured_{false}, captureRequested_{false}, resetRequested_{false}, targetCapturing_{false};
    std::atomic<bool> routingValid_{false}, transportAvailable_{false};
    double rate_ = 48000;
    int maximumBlock_ = 512;
    std::int64_t expectedPosition_ = 0, lastSolved_ = 0;
    std::uint64_t epoch_ = 1, solveEpoch_ = 0;
    std::uint64_t topology_ = 0;
    bool havePosition_ = false, haveSolved_ = false, synchronized_ = false;
    bool blockBypass_ = false, blockProtect_ = false;
    bool hostBypassed_ = false;
    unsigned routingGood_ = 0, routingMisses_ = 0;
    unsigned frameFill_ = 0;
    std::size_t trackCount_ = 0;
    float currentGain_ = 1, previousGain_ = 1, nextGain_ = 1;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoPeaksAudioProcessor)
};
