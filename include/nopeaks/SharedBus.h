#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nopeaks {

inline constexpr std::size_t kBands = 12;
inline constexpr std::size_t kFrameSize = 128;
inline constexpr std::size_t kHistoryFrames = 128;
inline constexpr std::size_t kMaxTracks = 64;
inline constexpr std::size_t kBusTrackDelaySamples = 8192;

// A timeline discontinuity MUST increment timelineEpoch. Frames are identified by
// (slot token, timelineEpoch, samplePosition), never by ring index alone.
struct TelemetryFrame {
    std::int64_t samplePosition = 0;
    std::uint64_t timelineEpoch = 1;
    std::uint64_t slotToken = 0; // transport fills this field
    double sampleRate = 48000.0;
    float priority = 0.5f; // 0 = expendable, 1 = preserve
    bool protectedTrack = false;
    std::uint32_t sampleCount = static_cast<std::uint32_t>(kFrameSize);
    std::array<float, kFrameSize> left{};
    std::array<float, kFrameSize> right{};
    std::array<float, kBands> power{};
    std::array<float, kBands> midPower{};
    std::array<float, kBands> sidePower{};
    std::array<float, kBands> cross{};
};

struct ControlFrame {
    std::int64_t samplePosition = 0;
    std::uint64_t timelineEpoch = 1;
    std::uint64_t slotToken = 0; // transport fills this field
    std::uint64_t masterToken = 0; // transport fills this field
    float attenuation = 1.0f; // linear amplitude, 0..1
    std::array<float, kBands> toneDb{}; // -6..0 dB
    std::array<float, kBands> sideDb{}; // -18..0 dB
};

struct TrackHandle {
    int index = -1;
    std::uint64_t token = 0;
    explicit operator bool() const noexcept { return index >= 0 && token != 0; }
};

struct TrackInfo {
    TrackHandle handle{};
    std::uint64_t heartbeatNanoseconds = 0;
    std::int64_t latestSamplePosition = 0;
    std::uint64_t timelineEpoch = 0;
    bool hasFrame = false;
    std::array<char, 48> name{};
};

// All methods except construction/destruction, claimTrack/releaseTrack and
// acquireMaster/releaseMaster are bounded, allocation-free and lock-free. Those
// setup/teardown methods must run outside the audio callback. Cross-process
// operation requires lock-free native 32/64-bit atomics (checked when opening).
//
// namespaceKey is a manually shared session identifier: use a different key for
// another simultaneously open DAW project. Group 1..16 alone cannot identify a
// DAW project. This transport never discovers or joins projects automatically.
class SharedBus {
public:
    explicit SharedBus(int group = 1, std::uint64_t namespaceKey = 0) noexcept;
    ~SharedBus();
    SharedBus(const SharedBus&) = delete;
    SharedBus& operator=(const SharedBus&) = delete;

    bool isOpen() const noexcept;
    const std::string& error() const noexcept;
    int group() const noexcept;
    std::uint64_t namespaceKey() const noexcept;

    TrackHandle claimTrack() noexcept;
    void releaseTrack(TrackHandle handle) noexcept;
    bool heartbeatTrack(TrackHandle handle) noexcept;
    // Stores the longest valid UTF-8 prefix fitting47bytes plus a terminator.
    bool setTrackName(TrackHandle handle, const char* utf8Name) noexcept;
    bool publish(TrackHandle handle, const TelemetryFrame& frame) noexcept;
    bool readFrame(TrackHandle handle, std::int64_t samplePosition,
                   std::uint64_t timelineEpoch, TelemetryFrame& result) const noexcept;
    bool readLatest(TrackHandle handle, TelemetryFrame& result) const noexcept;
    std::size_t activeTracks(std::array<TrackInfo, kMaxTracks>& result) const noexcept;

    // One Master per (namespaceKey, group). A crashed owner may be replaced after
    // kLeaseNanoseconds. Every command is capability-bound to the live Master.
    std::uint64_t acquireMaster() noexcept;
    void releaseMaster(std::uint64_t token) noexcept;
    bool heartbeatMaster(std::uint64_t token) noexcept;
    bool hasMaster() const noexcept;
    std::uint64_t currentMasterToken() const noexcept;
    bool writeControl(std::uint64_t masterToken, TrackHandle track,
                      const ControlFrame& frame) noexcept;
    bool readLatestControl(TrackHandle track, ControlFrame& result) const noexcept;
    bool readControl(TrackHandle track, std::int64_t samplePosition,
                     std::uint64_t timelineEpoch, ControlFrame& result) const noexcept;

    static std::uint64_t clockNanoseconds() noexcept;
    static constexpr std::uint64_t kLeaseNanoseconds = 2'000'000'000ULL;
    static std::size_t mappedBytes() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    int group_ = 1;
    std::uint64_t namespaceKey_ = 0;
};

} // namespace nopeaks
