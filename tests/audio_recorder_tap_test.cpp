// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RecorderTap.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using conflux::audio::RecorderTap;
using conflux::audio::StereoRecorder;

bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool boundaries(const std::filesystem::path& directory)
{
    RecorderTap tap;
    StereoRecorder writer(directory / "boundaries.wav");
    constexpr std::array<float, 4> master{.125F, -.25F, .375F, -.5F};
    bool passed = require(!tap.attached() && !tap.changePending() && tap.push(master),
                          "Empty tap did not ignore PCM");
    passed &= require(tap.request(&writer) && tap.changePending() && !tap.attached() &&
                          !tap.request(nullptr), "Outstanding attach was overwritten or acknowledged early");
    tap.beginBlock();
    passed &= require(tap.push(master) && writer.acceptedFrames() == 2 && tap.changePending(),
                      "Attach did not apply at entry or was acknowledged before its final use");
    tap.endBlock();
    passed &= require(tap.attached() && !tap.changePending() && tap.request(nullptr),
                      "Completed attachment did not admit a detach");
    // A callback already using the old writer may finish before the next block
    // applies detach. The owner must retain it through that use.
    passed &= require(tap.push(master) && writer.acceptedFrames() == 4 && tap.changePending(),
                      "Detach skipped the acknowledgement boundary");
    tap.beginBlock();
    passed &= require(tap.push(master) && writer.acceptedFrames() == 4 && tap.changePending(),
                      "Detached block still wrote PCM or acknowledged before exit");
    tap.endBlock();
    passed &= require(!tap.attached() && !tap.changePending(), "Detach was not acknowledged at exit");
    writer.stop();
    passed &= require(!writer.failed() && writer.acceptedFrames() == 4 && writer.writtenFrames() == 4,
                      "Safe detach lost the accepted recording prefix");
    StereoRecorder pending(directory / "pending.wav");
    passed &= require(tap.request(&pending), "Pending attachment failed");
    tap.quiesce();
    pending.stop();
    passed &= require(!tap.attached() && !tap.changePending() && pending.acceptedFrames() == 0 &&
                          tap.push(master), "Quiescence resurrected an attachment or accessed stopped PCM");
    return passed;
}

bool concurrentLifetime(const std::filesystem::path& directory)
{
    RecorderTap tap;
    std::atomic<bool> failed{false};
    std::jthread callback([&](std::stop_token stop) {
        constexpr std::array<float, 2> block{.125F, -.25F};
        while (!stop.stop_requested()) {
            tap.beginBlock();
            if (!tap.push(block)) failed.store(true, std::memory_order_relaxed);
            tap.endBlock();
            // The test measures cross-thread lifetime, not disk throughput.
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });
    auto waitApplied = [&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (tap.changePending() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        return !tap.changePending();
    };
    bool passed = true;
    for (int take = 0; take < 32 && passed; ++take) {
        auto writer = std::make_unique<StereoRecorder>(directory / (std::to_string(take) + ".wav"));
        passed &= require(tap.request(writer.get()) && waitApplied() && tap.attached(),
                          "Concurrent writer attachment did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        passed &= require(tap.request(nullptr) && waitApplied() && !tap.attached(),
                          "Concurrent writer detachment did not complete");
        if (!passed) {
            callback.request_stop();
            callback.join();
            tap.quiesce();
            writer->stop();
            return false;
        }
        // Callback keeps running here. After detach ack, the old allocation can
        // be joined and freed without a process callback retaining its pointer.
        writer->stop();
        passed &= require(writer->writtenFrames() > 0 && !writer->failed() &&
                              writer->writtenFrames() == writer->acceptedFrames(),
                          "Concurrent tap lost frames or stopped a writer still in use");
    }
    callback.request_stop();
    callback.join();
    tap.quiesce();
    return require(!failed.load(std::memory_order_relaxed), "A callback wrote after writer detachment") && passed;
}
} // namespace

int main()
{
    auto pattern = (std::filesystem::temp_directory_path() / "conflux-recorder-tap-XXXXXX").string();
    const auto* created = mkdtemp(pattern.data());
    if (!created) return 1;
    const std::filesystem::path directory(created);
    bool passed = false;
    try { passed = boundaries(directory) && concurrentLifetime(directory); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return passed ? 0 : 1;
}
