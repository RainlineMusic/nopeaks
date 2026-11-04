#include "nopeaks/SharedBus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <new>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nopeaks {
namespace {
constexpr std::uint64_t kMagic = 0x4E504B5342555331ULL; // NPKSBUS1
constexpr std::uint32_t kProtocolVersion = 1;
constexpr unsigned kSnapshotAttempts = 3;

static_assert(std::is_trivially_copyable<TelemetryFrame>::value, "wire payload must be trivial");
static_assert(std::is_trivially_copyable<ControlFrame>::value, "wire payload must be trivial");

// Every payload word is atomic. A plain-data seqlock would have C++ data races,
// even if a changing sequence caused its reader to discard the result.
template <typename Payload>
struct alignas(64) AtomicSnapshot {
    static constexpr std::size_t wordCount = (sizeof(Payload) + 3) / 4;
    std::atomic<std::uint64_t> sequence{0};
    std::array<std::atomic<std::uint32_t>, wordCount> words;
    AtomicSnapshot() noexcept { for (auto& word : words) word.store(0, std::memory_order_relaxed); }

    template <typename Predicate>
    bool write(const Payload& value, Predicate stillOwns) noexcept {
        auto sequenceBefore = sequence.load(std::memory_order_acquire);
        if ((sequenceBefore & 1u) != 0 ||
            !sequence.compare_exchange_strong(sequenceBefore, sequenceBefore + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
            return false;
        if (!stillOwns()) {
            sequence.store(sequenceBefore + 2, std::memory_order_release);
            return false;
        }
        std::array<std::uint32_t, wordCount> copy{};
        std::memcpy(copy.data(), &value, sizeof(value));
        for (std::size_t i = 0; i < wordCount; ++i)
            words[i].store(copy[i], std::memory_order_relaxed);
        sequence.store(sequenceBefore + 2, std::memory_order_release);
        return stillOwns();
    }

    bool read(Payload& result) const noexcept {
        for (unsigned attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
            const auto before = sequence.load(std::memory_order_acquire);
            if (before == 0 || (before & 1u) != 0)
                continue;
            std::array<std::uint32_t, wordCount> copy{};
            for (std::size_t i = 0; i < wordCount; ++i)
                copy[i] = words[i].load(std::memory_order_relaxed);
            // Keep the word reads before the second version observation.
            std::atomic_thread_fence(std::memory_order_acq_rel);
            if (sequence.load(std::memory_order_acquire) == before) {
                std::memcpy(static_cast<void*>(&result), copy.data(), sizeof(result));
                return true;
            }
        }
        return false;
    }
};

struct TrackName { std::array<char, 48> text{}; };

struct alignas(64) TrackSlot {
    std::atomic<std::uint64_t> owner{0};
    std::atomic<std::uint64_t> heartbeat{0};
    std::atomic<std::int64_t> latestPosition{0};
    std::atomic<std::uint64_t> latestEpoch{0};
    std::atomic<bool> hasFrame{false};
    std::atomic<bool> hasControl{false};
    std::atomic<std::int64_t> latestControlPosition{0};
    std::atomic<std::uint64_t> latestControlEpoch{0};
    AtomicSnapshot<TrackName> name;
    std::array<AtomicSnapshot<TelemetryFrame>, kHistoryFrames> telemetry;
    std::array<AtomicSnapshot<ControlFrame>, kHistoryFrames> controls;
};

struct alignas(64) SharedState {
    std::uint64_t magic = 0;
    std::uint32_t version = kProtocolVersion;
    std::uint32_t frameBytes = sizeof(TelemetryFrame);
    std::uint32_t controlBytes = sizeof(ControlFrame);
    std::uint32_t stateBytes = sizeof(SharedState);
    std::atomic<std::uint64_t> masterOwner{0};
    std::atomic<std::uint64_t> masterHeartbeat{0};
    std::array<TrackSlot, kMaxTracks> tracks;
};

bool live(std::uint64_t heartbeat, std::uint64_t now) noexcept {
    return heartbeat != 0 && now >= heartbeat && now - heartbeat <= SharedBus::kLeaseNanoseconds;
}

std::size_t ringIndex(std::int64_t position) noexcept {
    // Floor division for DAW preroll positions; never cast a negative index.
    auto frame = position / static_cast<std::int64_t>(kFrameSize);
    if (position < 0 && position % static_cast<std::int64_t>(kFrameSize) != 0)
        --frame;
    auto index = frame % static_cast<std::int64_t>(kHistoryFrames);
    if (index < 0) index += static_cast<std::int64_t>(kHistoryFrames);
    return static_cast<std::size_t>(index);
}

std::uint64_t makeToken(const void* address) noexcept {
    static std::atomic<std::uint64_t> counter{1};
#ifdef _WIN32
    const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(getpid());
#endif
    auto x = SharedBus::clockNanoseconds() ^ (process << 32) ^
             static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address)) ^
             counter.fetch_add(1, std::memory_order_relaxed);
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x ^= x >> 31;
    // Timestamped tokens close the CAS-owner / heartbeat-initialization window:
    // a second constructor must not reclaim a just-claimed slot as stale.
    const auto born = (SharedBus::clockNanoseconds() / 1'000'000ULL) & 0xffffffffULL;
    return (born << 32) | ((x & 0xffffffffULL) | 1ULL);
}

bool newlyClaimed(std::uint64_t owner, std::uint64_t now) noexcept {
    const auto milliseconds = static_cast<std::uint32_t>(now / 1'000'000ULL);
    const auto born = static_cast<std::uint32_t>(owner >> 32);
    return owner != 0 && static_cast<std::uint32_t>(milliseconds - born) <=
                         SharedBus::kLeaseNanoseconds / 1'000'000ULL;
}

bool validHandle(TrackHandle handle) noexcept {
    return handle.index >= 0 && handle.index < static_cast<int>(kMaxTracks) && handle.token != 0;
}

// Keep a valid UTF-8 prefix rather than cutting a multibyte codepoint at the
// fixed48-byte wire boundary. Invalid input terminates the prefix, too.
void copyUtf8Name(TrackName& output, const char* input) noexcept {
    constexpr std::size_t capacity = 47;
    std::size_t position = 0;
    while (position < capacity && input[position] != 0) {
        const auto lead = static_cast<unsigned char>(input[position]);
        std::size_t length = 0;
        if (lead <= 0x7f) length = 1;
        else if (lead >= 0xc2 && lead <= 0xdf) length = 2;
        else if (lead >= 0xe0 && lead <= 0xef) length = 3;
        else if (lead >= 0xf0 && lead <= 0xf4) length = 4;
        else break;
        if (position + length > capacity) break;
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto byte = static_cast<unsigned char>(input[position + offset]);
            if (byte < 0x80 || byte > 0xbf) { valid = false; break; }
        }
        if (!valid) break;
        if (length >= 3) {
            const auto second = static_cast<unsigned char>(input[position + 1]);
            // Reject overlong encodings, UTF-16 surrogates and values > U+10FFFF.
            if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
                (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second > 0x8f)) break;
        }
        for (std::size_t offset = 0; offset < length; ++offset)
            output.text[position + offset] = input[position + offset];
        position += length;
    }
}

bool finiteTelemetry(const TelemetryFrame& frame) noexcept {
    if (frame.sampleCount == 0 || frame.sampleCount > kFrameSize ||
        !std::isfinite(frame.sampleRate) || frame.sampleRate < 8000.0 || frame.sampleRate > 768000.0 ||
        !std::isfinite(frame.priority) || frame.priority < 0.0f || frame.priority > 1.0f ||
        frame.timelineEpoch == 0)
        return false;
    for (std::size_t i = 0; i < frame.sampleCount; ++i)
        if (!std::isfinite(frame.left[i]) || !std::isfinite(frame.right[i])) return false;
    for (std::size_t i = 0; i < kBands; ++i)
        if (!std::isfinite(frame.power[i]) || !std::isfinite(frame.midPower[i]) ||
            !std::isfinite(frame.sidePower[i]) || !std::isfinite(frame.cross[i]) ||
            frame.power[i] < 0.0f || frame.midPower[i] < 0.0f || frame.sidePower[i] < 0.0f)
            return false;
    return true;
}
} // namespace

struct SharedBus::Impl {
    SharedState* state = nullptr;
    std::array<TrackHandle, kMaxTracks> ownedTracks{};
    std::uint64_t ownedMaster = 0;
#ifdef _WIN32
    HANDLE mapping = nullptr;
#else
    int descriptor = -1;
#endif
    ~Impl() {
#ifdef _WIN32
        if (state) UnmapViewOfFile(state);
        if (mapping) CloseHandle(mapping);
#else
        if (state) munmap(state, sizeof(SharedState));
        if (descriptor >= 0) close(descriptor);
#endif
    }
};

SharedBus::SharedBus(int group, std::uint64_t namespaceKey) noexcept
    : group_(group), namespaceKey_(namespaceKey) {
    try {
        if (group < 1 || group > 16) { error_ = "Session group must be 1..16"; return; }
        std::atomic<std::uint32_t> a32{0};
        std::atomic<std::uint64_t> a64{0};
        std::atomic<std::int64_t> ai64{0};
        std::atomic<bool> ab{false};
        if (!a32.is_lock_free() || !a64.is_lock_free() || !ai64.is_lock_free() || !ab.is_lock_free()) {
            error_ = "Platform does not provide lock-free shared atomics"; return;
        }
        auto impl = std::make_unique<Impl>();
#ifdef _WIN32
        wchar_t name[160]{};
        wchar_t mutexName[176]{};
        std::swprintf(name, 160, L"Local\\RainlineNopeaks-v1-%016llx-%d", static_cast<unsigned long long>(namespaceKey), group);
        std::swprintf(mutexName, 176, L"%ls-init", name);
        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
        if (!mutex) { error_ = "Could not create shared-memory initialization mutex"; return; }
        const auto wait = WaitForSingleObject(mutex, 10000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex); error_ = "Shared-memory initialization timed out"; return;
        }
        const auto bytes = static_cast<std::uint64_t>(sizeof(SharedState));
        impl->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                          static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), name);
        if (impl->mapping)
            impl->state = static_cast<SharedState*>(MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (!impl->state) {
            ReleaseMutex(mutex); CloseHandle(mutex); error_ = "Could not map shared memory"; return;
        }
#else
        char path[192]{};
        std::snprintf(path, sizeof(path), "/tmp/rainline-nopeaks-v1-%llu-%016llx-%d.ipc",
                      static_cast<unsigned long long>(getuid()), static_cast<unsigned long long>(namespaceKey), group);
        int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        impl->descriptor = open(path, flags, 0600);
        if (impl->descriptor < 0) { error_ = "Could not open shared-memory backing file"; return; }
        if (flock(impl->descriptor, LOCK_EX) != 0) { error_ = "Could not lock shared-memory initialization"; return; }
        struct stat status{};
        if (fstat(impl->descriptor, &status) != 0 || status.st_uid != getuid() ||
            !S_ISREG(status.st_mode) || (status.st_mode & 0077) != 0) {
            flock(impl->descriptor, LOCK_UN); error_ = "Shared-memory backing file has unsafe ownership or permissions"; return;
        }
        if (status.st_size == 0) {
            if (ftruncate(impl->descriptor, static_cast<off_t>(sizeof(SharedState))) != 0) {
                flock(impl->descriptor, LOCK_UN); error_ = "Could not size shared memory"; return;
            }
        } else if (status.st_size != static_cast<off_t>(sizeof(SharedState))) {
            flock(impl->descriptor, LOCK_UN); error_ = "Shared-memory protocol size mismatch"; return;
        }
        void* mapped = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, impl->descriptor, 0);
        if (mapped == MAP_FAILED) {
            flock(impl->descriptor, LOCK_UN); error_ = "Could not map shared memory"; return;
        }
        impl->state = static_cast<SharedState*>(mapped);
#endif
        auto* state = impl->state;
        if (state->magic == 0) {
            new (state) SharedState();
            // Initialization is serialized by the native setup-only lock.
            state->magic = kMagic;
        }
        const bool protocolValid = state->magic == kMagic && state->version == kProtocolVersion &&
                                   state->frameBytes == sizeof(TelemetryFrame) &&
                                   state->controlBytes == sizeof(ControlFrame) &&
                                   state->stateBytes == sizeof(SharedState);
#ifdef _WIN32
        ReleaseMutex(mutex); CloseHandle(mutex);
#else
        flock(impl->descriptor, LOCK_UN);
#endif
        if (!protocolValid) { error_ = "Shared-memory protocol version mismatch"; return; }
        impl_ = std::move(impl);
    } catch (...) { error_ = "Shared-memory allocation failed"; }
}

SharedBus::~SharedBus() {
    if (!impl_) return;
    for (auto handle : impl_->ownedTracks) if (handle) releaseTrack(handle);
    if (impl_->ownedMaster != 0) releaseMaster(impl_->ownedMaster);
}
bool SharedBus::isOpen() const noexcept { return impl_ && impl_->state; }
const std::string& SharedBus::error() const noexcept { return error_; }
int SharedBus::group() const noexcept { return group_; }
std::uint64_t SharedBus::namespaceKey() const noexcept { return namespaceKey_; }
std::uint64_t SharedBus::clockNanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::size_t SharedBus::mappedBytes() noexcept { return sizeof(SharedState); }

TrackHandle SharedBus::claimTrack() noexcept {
    if (!isOpen()) return {};
    const auto now = clockNanoseconds();
    for (std::size_t i = 0; i < kMaxTracks; ++i) {
        auto& slot = impl_->state->tracks[i];
        auto oldOwner = slot.owner.load(std::memory_order_acquire);
        if (oldOwner != 0 && (live(slot.heartbeat.load(std::memory_order_acquire), now) || newlyClaimed(oldOwner, now))) continue;
        const auto token = makeToken(&slot);
        if (slot.owner.compare_exchange_strong(oldOwner, token, std::memory_order_acq_rel)) {
            slot.hasFrame.store(false, std::memory_order_release);
            slot.hasControl.store(false, std::memory_order_release);
            slot.name.write(TrackName{}, [&slot, token] { return slot.owner.load(std::memory_order_acquire) == token; });
            slot.heartbeat.store(now, std::memory_order_release);
            TrackHandle handle{static_cast<int>(i), token};
            impl_->ownedTracks[i] = handle;
            return handle;
        }
    }
    return {};
}
void SharedBus::releaseTrack(TrackHandle handle) noexcept {
    if (!isOpen() || !validHandle(handle)) return;
    auto expected = handle.token;
    impl_->state->tracks[static_cast<std::size_t>(handle.index)].owner.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel);
    if (impl_->ownedTracks[static_cast<std::size_t>(handle.index)].token == handle.token)
        impl_->ownedTracks[static_cast<std::size_t>(handle.index)] = {};
}
bool SharedBus::heartbeatTrack(TrackHandle handle) noexcept {
    if (!isOpen() || !validHandle(handle)) return false;
    auto& slot = impl_->state->tracks[static_cast<std::size_t>(handle.index)];
    if (slot.owner.load(std::memory_order_acquire) != handle.token) return false;
    slot.heartbeat.store(clockNanoseconds(), std::memory_order_release);
    return slot.owner.load(std::memory_order_acquire) == handle.token;
}
bool SharedBus::setTrackName(TrackHandle handle, const char* utf8Name) noexcept {
    if (!isOpen() || !validHandle(handle) || utf8Name == nullptr) return false;
    auto& slot = impl_->state->tracks[static_cast<std::size_t>(handle.index)];
    if (slot.owner.load(std::memory_order_acquire) != handle.token) return false;
    TrackName name;
    copyUtf8Name(name, utf8Name);
    return slot.name.write(name, [&slot, handle] { return slot.owner.load(std::memory_order_acquire) == handle.token; });
}

bool SharedBus::publish(TrackHandle handle, const TelemetryFrame& frame) noexcept {
    if (!isOpen() || !validHandle(handle) || !finiteTelemetry(frame)) return false;
    auto& slot = impl_->state->tracks[static_cast<std::size_t>(handle.index)];
    if (slot.owner.load(std::memory_order_acquire) != handle.token) return false;
    TelemetryFrame copy = frame;
    copy.slotToken = handle.token;
    const auto stillOwns = [&slot, handle] { return slot.owner.load(std::memory_order_acquire) == handle.token; };
    if (!slot.telemetry[ringIndex(frame.samplePosition)].write(copy, stillOwns)) return false;
    if (!stillOwns()) return false;
    slot.latestPosition.store(frame.samplePosition, std::memory_order_relaxed);
    slot.latestEpoch.store(frame.timelineEpoch, std::memory_order_relaxed);
    slot.hasFrame.store(true, std::memory_order_release);
    slot.heartbeat.store(clockNanoseconds(), std::memory_order_release);
    return stillOwns();
}
bool SharedBus::readFrame(TrackHandle handle, std::int64_t position, std::uint64_t epoch,
                          TelemetryFrame& result) const noexcept {
    if (!isOpen() || !validHandle(handle)) return false;
    const auto& slot = impl_->state->tracks[static_cast<std::size_t>(handle.index)];
    if (slot.owner.load(std::memory_order_acquire) != handle.token ||
        !live(slot.heartbeat.load(std::memory_order_acquire), clockNanoseconds())) return false;
    TelemetryFrame copy;
    if (!slot.telemetry[ringIndex(position)].read(copy) || copy.samplePosition != position ||
        copy.timelineEpoch != epoch || copy.slotToken != handle.token ||
        slot.owner.load(std::memory_order_acquire) != handle.token) return false;
    result = copy;
    return true;
}
bool SharedBus::readLatest(TrackHandle handle, TelemetryFrame& result) const noexcept {
    if (!isOpen() || !validHandle(handle)) return false;
    const auto& slot = impl_->state->tracks[static_cast<std::size_t>(handle.index)];
    if (!slot.hasFrame.load(std::memory_order_acquire)) return false;
    const auto epoch = slot.latestEpoch.load(std::memory_order_acquire);
    const auto position = slot.latestPosition.load(std::memory_order_acquire);
    return readFrame(handle, position, epoch, result);
}
std::size_t SharedBus::activeTracks(std::array<TrackInfo, kMaxTracks>& result) const noexcept {
    if (!isOpen()) return 0;
    std::size_t count = 0;
    const auto now = clockNanoseconds();
    for (std::size_t i = 0; i < kMaxTracks; ++i) {
        const auto& slot = impl_->state->tracks[i];
        const auto token = slot.owner.load(std::memory_order_acquire);
        const auto heartbeat = slot.heartbeat.load(std::memory_order_acquire);
        if (token == 0 || !live(heartbeat, now)) continue;
        TrackInfo info;
        info.handle = {static_cast<int>(i), token};
        info.heartbeatNanoseconds = heartbeat;
        info.hasFrame = slot.hasFrame.load(std::memory_order_acquire);
        info.latestSamplePosition = slot.latestPosition.load(std::memory_order_acquire);
        info.timelineEpoch = slot.latestEpoch.load(std::memory_order_acquire);
        TrackName name;
        if (slot.name.read(name)) info.name = name.text;
        if (slot.owner.load(std::memory_order_acquire) == token) result[count++] = info;
    }
    return count;
}

std::uint64_t SharedBus::acquireMaster() noexcept {
    if (!isOpen()) return 0;
    auto& state = *impl_->state;
    auto owner = state.masterOwner.load(std::memory_order_acquire);
    const auto now = clockNanoseconds();
    if (owner != 0 && (live(state.masterHeartbeat.load(std::memory_order_acquire), now) || newlyClaimed(owner, now))) return 0;
    const auto token = makeToken(&state.masterOwner);
    if (!state.masterOwner.compare_exchange_strong(owner, token, std::memory_order_acq_rel)) return 0;
    state.masterHeartbeat.store(now, std::memory_order_release);
    impl_->ownedMaster = token;
    return token;
}
void SharedBus::releaseMaster(std::uint64_t token) noexcept {
    if (!isOpen() || token == 0) return;
    auto expected = token;
    impl_->state->masterOwner.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    if (impl_->ownedMaster == token) impl_->ownedMaster = 0;
}
bool SharedBus::heartbeatMaster(std::uint64_t token) noexcept {
    if (!isOpen() || token == 0) return false;
    auto& state = *impl_->state;
    if (state.masterOwner.load(std::memory_order_acquire) != token) return false;
    state.masterHeartbeat.store(clockNanoseconds(), std::memory_order_release);
    return state.masterOwner.load(std::memory_order_acquire) == token;
}
std::uint64_t SharedBus::currentMasterToken() const noexcept {
    if (!isOpen()) return 0;
    const auto& state = *impl_->state;
    const auto owner = state.masterOwner.load(std::memory_order_acquire);
    if (owner == 0 || !live(state.masterHeartbeat.load(std::memory_order_acquire), clockNanoseconds())) return 0;
    return state.masterOwner.load(std::memory_order_acquire) == owner ? owner : 0;
}
bool SharedBus::hasMaster() const noexcept { return currentMasterToken() != 0; }
bool SharedBus::writeControl(std::uint64_t token, TrackHandle track, const ControlFrame& frame) noexcept {
    if (!isOpen() || !validHandle(track) || token == 0 || frame.timelineEpoch == 0 ||
        !std::isfinite(frame.attenuation) || currentMasterToken() != token) return false;
    auto& slot = impl_->state->tracks[static_cast<std::size_t>(track.index)];
    if (slot.owner.load(std::memory_order_acquire) != track.token) return false;
    ControlFrame copy = frame;
    copy.slotToken = track.token;
    copy.masterToken = token;
    copy.attenuation = std::clamp(copy.attenuation, 0.0f, 1.0f);
    for (std::size_t b = 0; b < kBands; ++b) {
        if (!std::isfinite(copy.toneDb[b]) || !std::isfinite(copy.sideDb[b])) return false;
        copy.toneDb[b] = std::clamp(copy.toneDb[b], -6.0f, 0.0f);
        copy.sideDb[b] = std::clamp(copy.sideDb[b], -18.0f, 0.0f);
    }
    const bool wrote = slot.controls[ringIndex(frame.samplePosition)].write(copy, [this, &slot, track, token] {
        return impl_->state->masterOwner.load(std::memory_order_acquire) == token &&
               slot.owner.load(std::memory_order_acquire) == track.token;
    });
    if (wrote) {
        slot.latestControlPosition.store(frame.samplePosition, std::memory_order_relaxed);
        slot.latestControlEpoch.store(frame.timelineEpoch, std::memory_order_relaxed);
        slot.hasControl.store(true, std::memory_order_release);
    }
    return wrote;
}
bool SharedBus::readLatestControl(TrackHandle track, ControlFrame& result) const noexcept {
    result = {};
    if (!isOpen() || !validHandle(track)) return false;
    const auto& slot = impl_->state->tracks[static_cast<std::size_t>(track.index)];
    if (!slot.hasControl.load(std::memory_order_acquire)) return false;
    const auto epoch = slot.latestControlEpoch.load(std::memory_order_acquire);
    const auto position = slot.latestControlPosition.load(std::memory_order_acquire);
    return readControl(track, position, epoch, result);
}
bool SharedBus::readControl(TrackHandle track, std::int64_t position, std::uint64_t epoch,
                            ControlFrame& result) const noexcept {
    // Missing, expired, mismatched or racing data means unity / no EQ. Always
    // initialize result so callers cannot accidentally reuse an earlier command.
    result = {};
    result.samplePosition = position;
    result.timelineEpoch = epoch;
    if (!isOpen() || !validHandle(track)) return false;
    const auto token = currentMasterToken();
    if (token == 0) return false;
    const auto& slot = impl_->state->tracks[static_cast<std::size_t>(track.index)];
    if (slot.owner.load(std::memory_order_acquire) != track.token) return false;
    ControlFrame copy;
    if (!slot.controls[ringIndex(position)].read(copy) || copy.samplePosition != position ||
        copy.timelineEpoch != epoch || copy.slotToken != track.token || copy.masterToken != token ||
        currentMasterToken() != token || slot.owner.load(std::memory_order_acquire) != track.token)
        return false;
    result = copy;
    return true;
}
} // namespace nopeaks
