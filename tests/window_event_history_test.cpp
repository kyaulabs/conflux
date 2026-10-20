// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../tools/experiments/WindowEventHistory.hpp"
#include "../tools/experiments/PlaybackUiPolicy.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using History = conflux::experiments::WindowEventHistory;
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

class Target final : public QObject {
public:
    bool exposed = false;
    unsigned handled = 0;
protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::Expose) { exposed = true; ++handled; }
        return QObject::event(event);
    }
};

void eventSourcesAndFiltering()
{
    Target primary, unrelated;
    qint64 clock = 1000;
    History history([&] { return QJsonObject{{"clock_ns", ++clock}, {"exposed", primary.exposed}}; },
                    {{"anchor_clock_ns", 1000}});
    history.observe(&primary);
    QEvent unrelatedExpose(QEvent::Expose);
    QCoreApplication::sendEvent(&unrelated, &unrelatedExpose);
    QEvent expose(QEvent::Expose);
    QCoreApplication::sendEvent(&primary, &expose);
    QObject::connect(&primary, &QObject::objectNameChanged, &history, [&](const QString&) {
        history.record("signal:fixture_post_change", {{"signal_value", primary.exposed}});
    });
    primary.setObjectName("fixture");
    QEvent ignored(QEvent::User);
    QCoreApplication::sendEvent(&primary, &ignored);
    history.record(std::string(200, 's').c_str());
    QJsonObject result{{"passed", true}};
    history.freeze(result);
    const auto report = result["window_events"].toObject();
    const auto rows = report["events"].toArray();
    require(result["passed"].toBool(), "Normal result status was changed by history retention");
    require(report["attempted"].toInteger() == 5 && rows.size() == 5 && report["dropped"].toInteger() == 0,
            "Unrelated or unselected events entered the history");
    require(primary.handled == 1 && unrelated.handled == 1, "Event filter consumed a target event");
    const auto before = rows[1].toObject(), after = rows[2].toObject();
    require(before["source"].toString() == "event_filter_before_target:Expose" &&
            before["event_type"].toInt() == int(QEvent::Expose) && !before["exposed"].toBool(),
            "Expose did not retain its actual pre-handler observation");
    require(after["source"].toString() == "signal:fixture_post_change" &&
            after["signal_value"].toBool() && after["exposed"].toBool(), "Post-change signal observation was lost");
    require(rows[3].toObject()["source"].toString().size() == 64, "Source label bound changed");
    for (qsizetype i = 0; i < rows.size(); ++i)
        require(rows[i].toObject()["ordinal"].toInteger() == i + 1 &&
                rows[i].toObject()["clock_ns"].toInteger() == 1001 + i, "History observations were reordered");
}

void overflowRetainsLatestInOrder()
{
    Target primary;
    qint64 clock = 0;
    History history([&] { return QJsonObject{{"clock_ns", ++clock}}; }, {});
    history.observe(&primary);
    for (unsigned i = 0; i < 130; ++i) {
        QEvent event(i % 2 ? QEvent::WindowDeactivate : QEvent::Expose);
        QCoreApplication::sendEvent(&primary, &event);
    }
    QJsonObject result;
    history.freeze(result);
    const auto report = result["window_events"].toObject();
    const auto rows = report["events"].toArray();
    require(report["capacity"].toInteger() == 64 && report["attempted"].toInteger() == 132 &&
            report["retained"].toInteger() == 64 && report["dropped"].toInteger() == 68 && rows.size() == 64,
            "Ring overflow was hidden or retention exceeded its bound");
    for (qsizetype i = 0; i < rows.size(); ++i)
        require(rows[i].toObject()["ordinal"].toInteger() == 69 + i &&
                rows[i].toObject()["clock_ns"].toInteger() == 69 + i, "Wrapped history lost chronological order");
}

void resultSurvivesEarlyFailureAndCleanup()
{
    for (const bool attached : {false, true}) {
        Target primary;
        bool cleanup = false;
        qint64 clock = 100;
        History history([&] {
            require(!cleanup, "Cleanup read a frozen history's captured references");
            return QJsonObject{{"clock_ns", ++clock}, {"exists", attached}, {"visible", attached},
                               {"exposed", false}, {"frame_age_ns", 200000000}, {"last_heartbeat_ns", 90}};
        }, {{"anchor_clock_ns", 100}, {"capture_end", "before cleanup"}});
        QJsonObject result{{"passed", false}};
        try {
            if (attached) history.observe(&primary);
            history.record("snapshot:progress");
            throw std::runtime_error(attached ? "unexposed fixture" : "early validation fixture");
        } catch (const std::exception& error) {
            result["error"] = error.what();
            if (attached) {
                const QJsonObject failure{{"clock_ns", 123}, {"exists", true}, {"visible", true}, {"exposed", false},
                    {"frame_age_ns", 200000000}, {"last_heartbeat_ns", 90}};
                result["window_failure"] = failure;
                history.record("snapshot:ui_guard_failure", failure);
            }
        }
        history.freeze(result);
        const auto frozen = QJsonDocument(result).toJson(QJsonDocument::Compact);
        cleanup = true;
        QEvent hide(QEvent::Hide);
        QCoreApplication::sendEvent(&primary, &hide);
        history.record("signal:visibleChanged", {{"signal_value", false}});
        history.freeze(result);
        const auto roundTrip = QJsonDocument::fromJson(frozen).object();
        require(QJsonDocument(result).toJson(QJsonDocument::Compact) == frozen && roundTrip == result,
                "Cleanup overwrote frozen normal/early-failure result history");
        require(!result["passed"].toBool() && !result["error"].toString().isEmpty() &&
                result["window_events"].toObject()["frozen_before_cleanup"].toBool(), "Early failure lost its history");
        if (attached) require(result["window_failure"].toObject()["clock_ns"].toInteger() == 123 &&
                              result["window_failure"].toObject()["visible"].toBool(), "Pre-cleanup failure snapshot changed");
    }
}

void destroyedTargetRemainsSafe()
{
    QPointer<Target> primary = new Target;
    History history([&] { return QJsonObject{{"exists", bool(primary)}}; }, {});
    history.observe(primary);
    delete primary.data();
    require(!primary, "Fixture target did not clear its QPointer");
    history.record("snapshot:missing_window");
    QJsonObject result{{"passed", false}};
    history.freeze(result);
    const auto rows = result["window_events"].toObject()["events"].toArray();
    require(rows.size() == 3 && rows[0].toObject()["exists"].toBool() &&
            !rows[1].toObject()["exists"].toBool() && !rows[2].toObject()["exists"].toBool(),
            "Destroyed target history was not safely retained");
}

void backgroundPolicyKeepsWindowAndStartupBoundaries()
{
    using Policy = conflux::experiments::PlaybackUiPolicy;
    const Policy visible, background(true);
    require(visible.requiresStartupPresentation(false) && !visible.requiresStartupPresentation(true) &&
            !background.requiresStartupPresentation(false) && !background.requiresStartupPresentation(true),
            "Default visible startup or background presentation policy changed");
    const Policy::State fresh{true, true, true, true, 1999999999};
    const Policy::State unexposed{true, true, false, true, 10000000};
    const Policy::State hidden{true, false, false, true, 3000000000LL};
    const Policy::State stale{true, true, true, true, 2000000000LL};
    require(visible.healthy(false, fresh) && background.healthy(false, fresh), "Fresh exposed window was rejected");
    require(!visible.healthy(false, unexposed) && background.healthy(false, unexposed),
            "Workspace unexposure did not respect the selected policy");
    require(!visible.healthy(false, hidden) && background.healthy(false, hidden),
            "Hidden/minimized window did not respect the selected policy");
    require(!visible.healthy(false, stale) && background.healthy(false, stale),
            "Exact two-second frame-age boundary changed");
    const Policy::State missing{false, false, false, false, 0};
    require(!visible.healthy(false, missing) && !background.healthy(false, missing) &&
            !background.healthy(true, missing), "Background waived the live-window requirement");
    require(visible.healthy(true, missing), "Existing private functional UI bypass changed");
}

void measuredUiCoverageRetainsGapsWithoutQualifyingBackground()
{
    using Policy = conflux::experiments::PlaybackUiPolicy;
    Policy background(true);
    const Policy::State healthy{true, true, true, true, 10000000};
    background.observe(healthy);
    const auto first = background.report();
    require(!first["any_gap"].toBool() && !first["visible_ui_qualification_eligible"].toBool(),
            "Fully exposed background observations became UI qualification");
    background.observe({true, true, false, true, 20000000});
    background.observe({true, false, false, true, 2500000000LL});
    background.observe(healthy);
    background.observe({false, false, false, false, 0});
    background.observe({true, true, true, false, 3000000000LL});
    const auto report = QJsonDocument::fromJson(QJsonDocument(background.report()).toJson()).object();
    require(report["checks"].toInteger() == 6 && report["visible_healthy_checks"].toInteger() == 2 &&
            report["missing_window_checks"].toInteger() == 1 && report["hidden_checks"].toInteger() == 2 &&
            report["unexposed_checks"].toInteger() == 3 && report["stale_frame_checks"].toInteger() == 2 &&
            report["missing_frame_checks"].toInteger() == 2 && report["maximum_frame_age_ns"].toInteger() == 2500000000LL &&
            report["any_gap"].toBool() && !report["visible_ui_qualification_eligible"].toBool(),
            "Measured UI coverage hid a recovered gap or fabricated frame/qualification coverage");
    require(Policy().report()["visible_ui_qualification_eligible"].toBool(), "Default visible eligibility changed");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        eventSourcesAndFiltering();
        overflowRetainsLatestInOrder();
        resultSurvivesEarlyFailureAndCleanup();
        destroyedTargetRemainsSafe();
        backgroundPolicyKeepsWindowAndStartupBoundaries();
        measuredUiCoverageRetainsGapsWithoutQualifyingBackground();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
