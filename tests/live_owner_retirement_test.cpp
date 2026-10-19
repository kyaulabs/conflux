// SPDX-License-Identifier: AGPL-3.0-or-later
// Ignored test draft: unreachable output + actual QtConcurrent retirement pattern.
// It does not inject live work into the unchanged DeckController or connect to
// an active PipeWire graph.
#include "audio/LiveSession.hpp"
#include "audio/LiveStemBuffer.hpp"
#include "audio/PreparedPlayback.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFuture>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace {
using namespace std::chrono_literals;
using conflux::audio::LiveSession;
using conflux::audio::LiveStemBuffer;
using conflux::audio::PreparedPlayback;

struct State {
    std::atomic<bool> cleanupStarted{false}, releaseCleanup{false}, expired{false};
    std::atomic<unsigned> exits{0};
    std::mutex mutex;
    std::thread::id producerThread{}, cleanupThread{}, retireThread{};
};
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

class MissingRuntime {
public:
    MissingRuntime()
    {
        require(directory_.isValid(), "Cannot create private missing-output runtime");
        for (std::size_t index = 0; index < names_.size(); ++index) {
            present_[index] = qEnvironmentVariableIsSet(names_[index]);
            previous_[index] = qgetenv(names_[index]);
        }
        const auto path = QFile::encodeName(directory_.path());
        require(qputenv(names_[0], path) && qputenv(names_[1], path) &&
                qputenv(names_[2], "conflux-live-owner-missing-server"), "Cannot isolate the private output failure");
    }
    ~MissingRuntime()
    {
        for (std::size_t index = 0; index < names_.size(); ++index)
            if (present_[index]) qputenv(names_[index], previous_[index]); else qunsetenv(names_[index]);
    }
private:
    QTemporaryDir directory_;
    const std::array<const char*, 3> names_{"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE"};
    std::array<QByteArray, 3> previous_{};
    std::array<bool, 3> present_{};
};
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    auto state = std::make_shared<State>();
    QFuture<void> retired;
    try {
        MissingRuntime privateMissingOutput;
        const auto guiThread = std::this_thread::get_id();
        LiveSession::PlaybackSources owners{};
        owners[0] = PreparedPlayback::makeLive(501, 400000,
            [state](std::stop_token stop, PreparedPlayback::LiveProducerPort& port) {
                {
                    const std::lock_guard lock(state->mutex);
                    state->producerThread = std::this_thread::get_id();
                }
                std::array<std::array<float, LiveStemBuffer::blockFrames * 2>, 4> pcm{};
                for (std::size_t role = 0; role < 4; ++role) pcm[role].fill(float(role + 1) / 256.F);
                std::uint64_t epoch = 0, cursor = 0;
                while (!stop.stop_requested()) {
                    LiveStemBuffer::Request request;
                    if (!port.tryRequest(request)) { std::this_thread::yield(); continue; }
                    if (epoch != request.epoch) { epoch = request.epoch; cursor = request.firstFrame; }
                    if (cursor == 400000) { std::this_thread::sleep_for(1ms); continue; }
                    const auto count = std::size_t(std::min<std::uint64_t>(4096, 400000 - cursor));
                    LiveStemBuffer::Packet packet{request.sourceId, epoch, cursor, count, cursor + count == 400000, {}};
                    for (std::size_t role = 0; role < 4; ++role)
                        packet.stems[role] = std::span(pcm[role]).first(count * 2);
                    const auto result = port.tryPublish(packet);
                    if (result == LiveStemBuffer::PublishResult::Accepted) cursor += count;
                    else std::this_thread::sleep_for(1ms);
                }
                {
                    const std::lock_guard lock(state->mutex);
                    state->cleanupThread = std::this_thread::get_id();
                }
                state->cleanupStarted.store(true);
                // Model a child cleanup still pending. The generous finite
                // bound prevents a broken teardown from hanging the suite.
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (!state->releaseCleanup.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                if (!state->releaseCleanup.load()) state->expired.store(true);
                ++state->exits;
            });
        LiveSession session(std::move(owners), LiveSession::InitialState{});
        std::string outputError;
        if (session.start(outputError)) {
            const auto deadline = std::chrono::steady_clock::now() + 6s;
            while (outputError.empty() && std::chrono::steady_clock::now() < deadline) {
                outputError = session.error();
                std::this_thread::sleep_for(10ms);
            }
            require(!outputError.empty() && !session.start(outputError),
                    "Repeated start did not quiesce the observed unreachable-output failure");
        }
        require(!outputError.empty() && !session.ready() && !state->cleanupStarted.load() && !state->exits.load(),
                "Output startup failure implicitly stopped/joined its live producer on the GUI caller");
        require(!session.start(outputError) && !outputError.empty() && !state->cleanupStarted.load(),
                "A terminal failed start restarted output or stopped its retained live producer");
        require(!session.requestRecorder(nullptr) && !session.recorderChangePending() &&
                !session.submit({conflux::audio::RenderCore::Command::Type::Play, 0}),
                "A terminal failed start accepted recorder or transport mutation");
        const auto closeFile = [](std::FILE* file) { std::fclose(file); };
        auto file = std::unique_ptr<std::FILE, decltype(closeFile)>(std::tmpfile(), closeFile);
        const std::array<float, 2> replacementPcm{.015625F, -.03125F};
        require(file && std::fwrite(replacementPcm.data(), sizeof(float), replacementPcm.size(), file.get()) == replacementPcm.size() &&
                std::fflush(file.get()) == 0, "Cannot write private rejected-replacement fixture");
        auto rejected = std::make_unique<PreparedPlayback>(fileno(file.get()), 1);
        const auto* rejectedIdentity = rejected.get();
        require(!session.requestSourceReplacement(0, rejected, 1) && rejected.get() == rejectedIdentity &&
                !session.sourceReplacementPending() && !session.takeSourceReplacementResult(),
                "Terminal failed start consumed a candidate or accepted a replacement mutation");
        rejected.reset();
        auto returned = session.stopAndTakePlaybacks();
        require(returned.active[0] && !returned.active[1] && !returned.replacement &&
                !state->cleanupStarted.load() && !state->exits.load(),
                "Control/GUI ownership return invoked producer cleanup or lost its live owner");
        // Destroying the detached session must also avoid the old source.
        session.stop();
        require(!state->cleanupStarted.load(), "Detached session stop requested producer cleanup");
        retired = QtConcurrent::run([state, returned = std::move(returned)]() mutable {
            {
                const std::lock_guard lock(state->mutex);
                state->retireThread = std::this_thread::get_id();
            }
            returned.active = {};
            returned.replacement.reset();
        });
        bool eventObservedWhilePending = false;
        QTimer poll;
        poll.setInterval(1);
        QObject::connect(&poll, &QTimer::timeout, &application, [&] {
            if (!state->cleanupStarted.load()) return;
            eventObservedWhilePending = !state->exits.load() && !retired.isFinished();
            state->releaseCleanup.store(true);
            application.quit();
        });
        poll.start();
        QTimer::singleShot(5000, &application, &QCoreApplication::quit);
        application.exec();
        state->releaseCleanup.store(true);
        retired.waitForFinished();
        require(eventObservedWhilePending && !state->expired.load() && state->exits == 1,
                "Application events did not run while whole-owner cleanup remained pending off GUI");
        {
            const std::lock_guard lock(state->mutex);
            require(state->retireThread != guiThread && state->producerThread != guiThread &&
                    state->cleanupThread == state->producerThread,
                    "Producer/process cleanup or owner join migrated onto the GUI/control thread");
        }
        std::cout << "Quiesced whole-owner return and responsive Qt retirement checks completed\n";
        return 0;
    } catch (const std::exception& error) {
        state->releaseCleanup.store(true);
        if (retired.isRunning()) retired.waitForFinished();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
