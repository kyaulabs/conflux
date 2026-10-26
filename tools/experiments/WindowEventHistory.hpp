// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace conflux::experiments {

// Soak-harness metadata only. Never consumes an event or changes window state.
class WindowEventHistory final : public QObject {
public:
    static constexpr std::size_t capacity = 64;
    WindowEventHistory(std::function<QJsonObject()> snapshot, QJsonObject metadata)
        : snapshot_(std::move(snapshot)), metadata_(std::move(metadata)) {}

    void observe(QObject* window)
    {
        if (window_) window_->removeEventFilter(this);
        window_ = window;
        if (window_) window_->installEventFilter(this);
        record("snapshot:window_attached");
    }

    void record(const char* source, const QJsonObject& detail = {})
    {
        if (frozen_) return;
        auto row = snapshot_();
        for (auto it = detail.begin(); it != detail.end(); ++it) row.insert(it.key(), it.value());
        row["source"] = QString::fromLatin1(source).left(64);
        row["ordinal"] = qint64(++attempted_);
        rows_[next_] = std::move(row);
        next_ = (next_ + 1) % capacity;
        retained_ = std::min(retained_ + 1, capacity);
    }

    void freeze(QJsonObject& result)
    {
        if (!frozen_) {
            const auto failure = result.value("window_failure");
            if (failure.isObject() && !failure.toObject().isEmpty()) {
                // Measurement has already failed. Observe a late compositor callback
                // before cleanup without changing the original failure or UI policy.
                QElapsedTimer elapsed;
                elapsed.start();
                record("snapshot:failure_followup_begin");
                const auto before = rows_[(next_ + capacity - 1) % capacity];
                QEventLoop loop;
                QTimer::singleShot(250, &loop, &QEventLoop::quit);
                loop.exec(QEventLoop::ExcludeUserInputEvents);
                record("snapshot:failure_followup_end");
                const auto after = rows_[(next_ + capacity - 1) % capacity];
                result["window_failure_followup"] = QJsonObject{{"requested_ms", 250}, {"elapsed_ms", elapsed.elapsed()},
                    {"outside_measured_interval", true}, {"before", before}, {"after", after},
                    {"scope", "Post-failure event-loop observation; original failure and measured interval unchanged. Whole-command deadline still applies."}};
            }
            record("snapshot:before_cleanup");
            frozen_ = true;
            if (window_) window_->removeEventFilter(this);
        }
        QJsonArray events;
        const auto begin = (next_ + capacity - retained_) % capacity;
        for (std::size_t i = 0; i < retained_; ++i) events.append(rows_[(begin + i) % capacity]);
        result["window_events"] = QJsonObject{{"capacity", qint64(capacity)}, {"attempted", qint64(attempted_)},
            {"retained", qint64(retained_)}, {"dropped", qint64(attempted_ - retained_)},
            {"frozen_before_cleanup", true}, {"metadata", metadata_}, {"events", events}};
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == window_) {
            const char* source = nullptr;
            switch (event->type()) {
            case QEvent::Expose: source = "event_filter_before_target:Expose"; break;
            case QEvent::Show: source = "event_filter_before_target:Show"; break;
            case QEvent::Hide: source = "event_filter_before_target:Hide"; break;
            case QEvent::WindowActivate: source = "event_filter_before_target:WindowActivate"; break;
            case QEvent::WindowDeactivate: source = "event_filter_before_target:WindowDeactivate"; break;
            case QEvent::WindowStateChange: source = "event_filter_before_target:WindowStateChange"; break;
            case QEvent::FocusIn: source = "event_filter_before_target:FocusIn"; break;
            case QEvent::FocusOut: source = "event_filter_before_target:FocusOut"; break;
            case QEvent::Close: source = "event_filter_before_target:Close"; break;
            default: break;
            }
            if (source) record(source, {{"event_type", int(event->type())}});
        }
        return false;
    }

private:
    std::function<QJsonObject()> snapshot_;
    QJsonObject metadata_;
    QPointer<QObject> window_;
    std::array<QJsonObject, capacity> rows_;
    std::size_t next_ = 0, retained_ = 0;
    std::uint64_t attempted_ = 0;
    bool frozen_ = false;
};

} // namespace conflux::experiments
