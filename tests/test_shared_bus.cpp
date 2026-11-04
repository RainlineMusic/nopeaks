#include "nopeaks/SharedBus.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct TestNamespace {
    std::uint64_t key = nopeaks::SharedBus::clockNanoseconds() ^ 0x1234567812345678ULL;
    ~TestNamespace() {
#ifndef _WIN32
        char path[192]{};
        std::snprintf(path, sizeof(path), "/tmp/rainline-nopeaks-v1-%llu-%016llx-1.ipc",
                      static_cast<unsigned long long>(getuid()), static_cast<unsigned long long>(key));
        unlink(path);
#endif
    }
};

void testRoundTripAndOwnership(std::uint64_t key) {
    using namespace nopeaks;
    SharedBus track(1, key), master(1, key), duplicateMaster(1, key);
    require(track.isOpen(), track.error().c_str());
    require(master.isOpen(), master.error().c_str());
    require(SharedBus::mappedBytes() > kMaxTracks * kHistoryFrames * sizeof(TelemetryFrame), "mapped ring storage missing");
    const auto handle = track.claimTrack();
    require(static_cast<bool>(handle), "track registration failed");
    require(track.setTrackName(handle, "Lead vocal"), "track name write failed");
    const auto masterToken = master.acquireMaster();
    require(masterToken != 0, "master registration failed");
    require(duplicateMaster.acquireMaster() == 0, "two active masters were accepted");

    TelemetryFrame frame;
    frame.samplePosition = -256;
    frame.timelineEpoch = 7;
    frame.priority = 0.9f;
    frame.protectedTrack = true;
    frame.left.fill(0.25f);
    frame.right.fill(-0.5f);
    frame.power.fill(0.125f);
    frame.midPower.fill(0.1f);
    frame.sidePower.fill(0.025f);
    frame.cross.fill(-0.2f);
    require(track.publish(handle, frame), "telemetry publication failed");
    TelemetryFrame received;
    require(master.readFrame(handle, -256, 7, received), "cross-instance telemetry read failed");
    require(received.slotToken == handle.token && received.left[127] == 0.25f && received.right[0] == -0.5f &&
            received.protectedTrack && received.priority == 0.9f, "telemetry payload changed");
    require(!master.readFrame(handle, -256, 8, received), "wrong timeline epoch accepted");
    require(!master.readFrame(handle, -128, 7, received), "wrong timestamp accepted");
    require(master.readLatest(handle, received) && received.samplePosition == -256, "latest telemetry failed");
    std::array<TrackInfo, kMaxTracks> info{};
    require(master.activeTracks(info) == 1 && std::string(info[0].name.data()) == "Lead vocal", "track metadata failed");

    // Cyrillic and emoji must never be split at the47-byte wire boundary.
    const std::string cyrillicLetter = "\xD0\x96"; // U+0416, Ж
    const std::string emoji = "\xF0\x9F\x8E\xB8"; // U+1F3B8, guitar
    std::string russianName;
    for (int i = 0; i < 22; ++i) russianName += cyrillicLetter;
    require(track.setTrackName(handle, (russianName + emoji + russianName).c_str()),
            "long Russian/emoji name write failed");
    require(master.activeTracks(info) == 1 && std::string(info[0].name.data()) == russianName,
            "name truncation split a four-byte emoji");
    russianName.resize(42); //21 complete Cyrillic letters, plus emoji =46bytes
    const auto completePrefix = russianName + emoji;
    require(track.setTrackName(handle, (completePrefix + cyrillicLetter).c_str()), "UTF-8 name write failed");
    require(master.activeTracks(info) == 1 && std::string(info[0].name.data()) == completePrefix,
            "name truncation split a Cyrillic codepoint or dropped a fitting emoji");
    require(track.setTrackName(handle, "Valid\xED\xA0\x80"), "invalid UTF-8 prefix handling failed");
    require(master.activeTracks(info) == 1 && std::string(info[0].name.data()) == "Valid",
            "invalid UTF-8 surrogate encoding was retained");

    ControlFrame control;
    control.samplePosition = -256;
    control.timelineEpoch = 7;
    control.attenuation = 0.75f;
    control.toneDb.fill(-2.0f);
    control.sideDb.fill(-4.0f);
    require(!master.writeControl(masterToken + 1, handle, control), "invalid master capability accepted");
    require(master.writeControl(masterToken, handle, control), "control publication failed");
    ControlFrame applied;
    require(track.readControl(handle, -256, 7, applied) && applied.attenuation == 0.75f &&
            applied.toneDb[11] == -2.0f && applied.sideDb[0] == -4.0f, "control payload changed");
    require(track.readLatestControl(handle, applied) && applied.masterToken == masterToken, "latest control read failed");
    require(!track.readControl(handle, -256, 8, applied) && applied.attenuation == 1.0f && applied.toneDb[0] == 0.0f,
            "epoch mismatch did not fail to unity");

    // Ring wrap must invalidate the old timestamp, even when it reuses a cell.
    frame.samplePosition += static_cast<std::int64_t>(kFrameSize * kHistoryFrames);
    require(track.publish(handle, frame), "wrapped telemetry publication failed");
    require(!master.readFrame(handle, -256, 7, received), "overwritten ring frame accepted");
    require(master.readFrame(handle, frame.samplePosition, 7, received), "new wrapped frame missing");

    frame.left[1] = std::numeric_limits<float>::quiet_NaN();
    require(!track.publish(handle, frame), "nonfinite telemetry accepted");
    control.attenuation = std::numeric_limits<float>::infinity();
    require(!master.writeControl(masterToken, handle, control), "nonfinite gain command accepted");
    control.attenuation = 9.0f;
    control.toneDb.fill(-99.0f);
    control.sideDb.fill(-99.0f);
    require(master.writeControl(masterToken, handle, control), "bounded control publication failed");
    require(track.readLatestControl(handle, applied) && applied.attenuation == 1.0f &&
            applied.toneDb[0] == -6.0f && applied.sideDb[0] == -18.0f, "control limits not enforced");

    master.releaseMaster(masterToken);
    require(!track.readLatestControl(handle, applied) && applied.attenuation == 1.0f, "released master command remained active");
    const auto replacementMaster = duplicateMaster.acquireMaster();
    require(replacementMaster != 0 && replacementMaster != masterToken, "replacement master failed");
    require(!track.readControl(handle, -256, 7, applied), "previous master's command survived replacement");
    duplicateMaster.releaseMaster(replacementMaster);
    track.releaseTrack(handle);
    require(!master.readLatest(handle, received), "released track remained readable");
    const auto replacementTrack = track.claimTrack();
    require(static_cast<bool>(replacementTrack) && replacementTrack.token != handle.token, "replacement track failed");
    frame.left[1] = 0.0f;
    require(!track.publish(handle, frame), "stale track capability accepted");
    track.releaseTrack(replacementTrack);
}

void testCapacityAndIsolation(std::uint64_t key) {
    using namespace nopeaks;
    SharedBus writer(1, key), reader(1, key);
    std::array<TrackHandle, kMaxTracks> handles{};
    for (auto& handle : handles) {
        handle = writer.claimTrack();
        require(static_cast<bool>(handle), "64-track capacity was not available");
    }
    require(!writer.claimTrack(), "transport exceeded its fixed64-track capacity");
    std::array<TrackInfo, kMaxTracks> active{};
    require(reader.activeTracks(active) == kMaxTracks, "active-track enumeration lost registrations");
    const auto old = handles[17];
    writer.releaseTrack(old);
    handles[17] = writer.claimTrack();
    require(handles[17].index == old.index && handles[17].token != old.token,
            "released slot was not safely reusable");
    for (auto handle : handles) writer.releaseTrack(handle);
    require(reader.activeTracks(active) == 0, "released slots remained active");

    TestNamespace isolatedSpace;
    SharedBus isolated(1, isolatedSpace.key);
    require(isolated.isOpen(), "isolated project transport failed to open");
    const auto isolatedTrack = isolated.claimTrack();
    require(static_cast<bool>(isolatedTrack) && reader.activeTracks(active) == 0,
            "distinct ProjectIDs leaked track registration");
    isolated.releaseTrack(isolatedTrack);
}

void testConcurrentSnapshots(std::uint64_t key) {
    using namespace nopeaks;
    SharedBus writer(1, key), reader(1, key);
    const auto handle = writer.claimTrack();
    require(static_cast<bool>(handle), "concurrent test track registration failed");
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        TelemetryFrame frame;
        for (std::int64_t f = 1; f <= 2500; ++f) {
            frame.samplePosition = f * static_cast<std::int64_t>(kFrameSize);
            const auto value = static_cast<float>(f);
            frame.left.fill(value);
            frame.right.fill(-value);
            frame.power.fill(value * 2.0f);
            writer.publish(handle, frame);
        }
        finished.store(true, std::memory_order_release);
    });
    bool torn = false;
    std::size_t readCount = 0;
    while (!finished.load(std::memory_order_acquire)) {
        TelemetryFrame frame;
        if (!reader.readLatest(handle, frame)) continue;
        ++readCount;
        const auto value = static_cast<float>(frame.samplePosition / static_cast<std::int64_t>(kFrameSize));
        for (std::size_t s = 0; s < kFrameSize; ++s)
            if (frame.left[s] != value || frame.right[s] != -value) torn = true;
        for (auto power : frame.power) if (power != value * 2.0f) torn = true;
    }
    producer.join();
    TelemetryFrame final;
    require(reader.readLatest(handle, final), "final concurrent snapshot missing");
    require(!torn, "atomic payload snapshot was torn");
    require(readCount > 0 || final.left[0] == 2500.0f, "snapshot test produced no reads");
    writer.releaseTrack(handle);
}

#ifndef _WIN32
void testSeparateProcess(std::uint64_t key) {
    using namespace nopeaks;
    SharedBus master(1, key);
    const auto token = master.acquireMaster();
    require(token != 0, "process-test master registration failed");
    int childToParent[2]{}, parentToChild[2]{};
    require(pipe(childToParent) == 0 && pipe(parentToChild) == 0, "process-test pipe creation failed");
    const auto child = fork();
    require(child >= 0, "process-test fork failed");
    if (child == 0) {
        close(childToParent[0]); close(parentToChild[1]);
        SharedBus track(1, key);
        const auto handle = track.claimTrack();
        TelemetryFrame frame;
        frame.samplePosition = 4096;
        frame.timelineEpoch = 19;
        frame.left.fill(0.3125f);
        frame.right.fill(-0.125f);
        int result = !track.isOpen() || !handle || !track.publish(handle, frame) ? 1 : 0;
        if (write(childToParent[1], &handle, sizeof(handle)) != static_cast<ssize_t>(sizeof(handle))) result = 2;
        char proceed = 0;
        if (read(parentToChild[0], &proceed, 1) != 1) result = 3;
        ControlFrame control;
        if (!track.readControl(handle, 4096, 19, control) || control.attenuation != 0.625f ||
            control.sideDb[3] != -3.0f) result = 4;
        track.releaseTrack(handle);
        close(childToParent[1]); close(parentToChild[0]);
        _exit(result);
    }
    close(childToParent[1]); close(parentToChild[0]);
    TrackHandle handle;
    const auto bytes = read(childToParent[0], &handle, sizeof(handle));
    TelemetryFrame frame;
    bool success = bytes == static_cast<ssize_t>(sizeof(handle)) &&
                   master.readFrame(handle, 4096, 19, frame) && frame.left[127] == 0.3125f;
    ControlFrame control;
    control.samplePosition = 4096;
    control.timelineEpoch = 19;
    control.attenuation = 0.625f;
    control.sideDb[3] = -3.0f;
    success = master.writeControl(token, handle, control) && success;
    const char proceed = 1;
    success = write(parentToChild[1], &proceed, 1) == 1 && success;
    close(childToParent[0]); close(parentToChild[1]);
    int status = 0;
    const auto waited = waitpid(child, &status, 0);
    master.releaseMaster(token);
    require(success && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "separate-process telemetry/control round trip failed");
}
#endif

void testLeaseExpiry(std::uint64_t key) {
    using namespace nopeaks;
    SharedBus first(1, key), second(1, key);
    const auto track = first.claimTrack();
    const auto master = first.acquireMaster();
    require(static_cast<bool>(track) && master != 0, "lease-test registration failed");
    ControlFrame command;
    command.attenuation = 0.2f;
    require(first.writeControl(master, track, command), "lease-test command failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(2050));
    ControlFrame received;
    require(!second.hasMaster() && !second.readControl(track, 0, 1, received) && received.attenuation == 1.0f,
            "expired master did not fail to unity");
    const auto replacement = second.acquireMaster();
    require(replacement != 0 && replacement != master, "expired master was not reclaimable");
    require(!first.heartbeatMaster(master), "expired master reacquired ownership by heartbeat");
    const auto replacementTrack = second.claimTrack();
    require(static_cast<bool>(replacementTrack), "expired track was not reclaimable");
    require(!first.heartbeatTrack(track), "expired track reacquired ownership by heartbeat");
    second.releaseTrack(replacementTrack);
    second.releaseMaster(replacement);
    first.releaseTrack(track);
}
} // namespace

void testSharedBus() {
    TestNamespace space;
    testRoundTripAndOwnership(space.key);
    testCapacityAndIsolation(space.key);
    testConcurrentSnapshots(space.key);
#ifndef _WIN32
    testSeparateProcess(space.key);
#endif
    testLeaseExpiry(space.key);
    nopeaks::SharedBus invalid(17, space.key);
    require(!invalid.isOpen() && !invalid.error().empty(), "invalid group was accepted");
}

#ifdef NOPEAKS_SHARED_BUS_TEST_MAIN
#include <iostream>
int main() {
    try { testSharedBus(); std::cout << "SharedBus tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
