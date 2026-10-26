// Diagnostic driver of the real klogg Qt Widgets view.
// SPDX-License-Identifier: GPL-3.0-or-later
// No production read, decoding, wrapping or selection implementation is copied here.
#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "configuration.h"
#include "highlighterset.h"
#include "loadingstatus.h"
#include "logdata.h"
#include "logger.h"
#include "logmainview.h"
#include "overview.h"
#include "overviewwidget.h"
#include "persistentinfo.h"
#include "quickfindpattern.h"

// Same portable-settings seam as upstream tests: writes only beside the probe
// executable (build it under the ignored .local directory).
const bool PersistentInfo::ForcePortable = true;

namespace {
std::mutex outputMutex;
void output(QJsonObject object)
{
    const std::lock_guard<std::mutex> lock(outputMutex);
    object.insert("evidence_kind", "diagnostic_real_LogMainView");
    object.insert("platform", QGuiApplication::platformName());
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << '\n';
    std::cout.flush();
}

struct Memory {
    bool supported = false;
    quint64 working = 0, privateBytes = 0, processPeak = 0;
    QJsonObject json() const
    {
        if (!supported) return {{"status", "unavailable"}};
        return {{"status", "measured"},
                {"working_set_bytes", QString::number(working)},
                {"private_bytes", QString::number(privateBytes)},
                {"process_peak_working_set_bytes", QString::number(processPeak)}};
    }
};
Memory memory()
{
    Memory result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof(counters))) {
        result.supported = true;
        result.working = static_cast<quint64>(counters.WorkingSetSize);
        result.privateBytes = static_cast<quint64>(counters.PrivateUsage);
        result.processPeak = static_cast<quint64>(counters.PeakWorkingSetSize);
    }
#endif
    return result;
}

// Runs while GUI code is blocked; sampled peaks are lower bounds, not exact
// live allocations or independent stage-specific OS high-water counters.
class Monitor {
public:
    Monitor(int timeoutMs, std::function<QJsonObject()> timeoutMetadata)
        : timeoutMs_(timeoutMs), metadata_(std::move(timeoutMetadata)), worker_([this] { run(); }) {}
    ~Monitor()
    {
        { const std::lock_guard<std::mutex> lock(mutex_); finished_ = true; }
        condition_.notify_one();
        worker_.join();
    }
    void stage(int stage)
    {
        currentStage_.store(stage);
        sample();
    }
    Memory peak(int stage)
    {
        sample();
        const std::lock_guard<std::mutex> lock(mutex_);
        return peaks_.at(static_cast<size_t>(stage));
    }
    quint64 samples(int stage)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return samples_.at(static_cast<size_t>(stage));
    }
private:
    void sample()
    {
        const auto stage = currentStage_.load();
        const auto value = memory();
        if (stage < 0) return;
        const std::lock_guard<std::mutex> lock(mutex_);
        auto& peak = peaks_.at(static_cast<size_t>(stage));
        peak.supported = peak.supported || value.supported;
        peak.working = std::max(peak.working, value.working);
        peak.privateBytes = std::max(peak.privateBytes, value.privateBytes);
        peak.processPeak = std::max(peak.processPeak, value.processPeak);
        ++samples_.at(static_cast<size_t>(stage));
    }
    void run()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
        while (true) {
            sample();
            const auto usage = memory();
            if ( usage.supported && usage.privateBytes > 1024ULL * 1024 * 1024 ) {
                auto result = metadata_();
                result.insert("status", "memory_limit_exceeded");
                result.insert("private_limit_bytes", QString::number(1024ULL * 1024 * 1024));
                result.insert("active_stage", currentStage_.load());
                result.insert("memory", usage.json());
                result.insert("cleanup_verified", false);
                output(result);
                std::_Exit(125);
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (condition_.wait_for(lock, std::chrono::milliseconds(5), [this] { return finished_; })) return;
            if (std::chrono::steady_clock::now() >= deadline) {
                lock.unlock();
                auto result = metadata_();
                result.insert("status", "timeout");
                result.insert("active_stage", currentStage_.load());
                result.insert("timeout_ms", timeoutMs_);
                result.insert("cleanup_verified", false);
                result.insert("memory", memory().json());
                output(result);
                std::_Exit(124);
            }
        }
    }
    int timeoutMs_;
    std::function<QJsonObject()> metadata_;
    std::atomic<int> currentStage_{-1};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    std::array<Memory, 16> peaks_{};
    std::array<quint64, 16> samples_{};
    std::thread worker_;
};

// QTimer callbacks are the actual GUI-thread heartbeat. A post-operation drain
// makes the gap spanning a synchronous blocked call observable in raw times.
class Heartbeat {
public:
    Heartbeat()
    {
        clock_.start();
        QObject::connect(&timer_, &QTimer::timeout, [&] { ticks_.push_back(clock_.nsecsElapsed()); });
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.start(10);
    }
    void settle(int milliseconds = 50)
    {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
    qint64 now() const { return clock_.nsecsElapsed(); }
    size_t count() const { return ticks_.size(); }
    QJsonObject summary(size_t from) const
    {
        QJsonArray ticks;
        qint64 gap = 0;
        const auto begin = from > 0 ? from - 1 : 0;
        for (size_t i = begin; i < ticks_.size(); ++i) {
            ticks.append(QString::number(ticks_[i]));
            if (i > begin) gap = std::max(gap, ticks_[i] - ticks_[i - 1]);
        }
        return {{"timer_interval_ms", 10}, {"raw_monotonic_ns", ticks},
                {"max_observed_gap_ns", QString::number(gap)},
                {"measurement_scope", "GUI event-loop delivery on reported Qt platform; includes preceding and post-operation ticks"}};
    }
private:
    QTimer timer_;
    QElapsedTimer clock_;
    std::vector<qint64> ticks_;
};

struct Stage {
    int id;
    const char* name;
    std::function<QJsonObject()> action;
};
void perform(const Stage& stage, Monitor& monitor, Heartbeat& heartbeat)
{
    heartbeat.settle();
    monitor.stage(stage.id);
    const auto firstTick = heartbeat.count();
    const auto start = heartbeat.now();
    const auto before = memory();
    output({{"status", "stage_started"}, {"stage", stage.name}, {"stage_id", stage.id},
            {"started_monotonic_ns", QString::number(start)}, {"memory_before", before.json()}});
    QJsonObject detail;
    try {
        detail = stage.action();
    } catch (const std::exception& error) {
        output({{"status", "exception"}, {"stage", stage.name}, {"what", error.what()},
                {"elapsed_ns", QString::number(heartbeat.now() - start)}, {"memory", memory().json()}});
        throw;
    }
    const auto end = heartbeat.now();
    const auto after = memory();
    const auto peak = monitor.peak(stage.id);
    const auto samples = monitor.samples(stage.id);
    monitor.stage(-1);
    // render/action has completed; process deferred events before reporting its
    // spanning heartbeat gap. Secondary paints in this drain remain part of gap.
    heartbeat.settle();
    output({{"status", "stage_completed"}, {"stage", stage.name}, {"stage_id", stage.id},
            {"started_monotonic_ns", QString::number(start)},
            {"finished_monotonic_ns", QString::number(end)},
            {"synchronous_elapsed_ns", QString::number(end - start)},
            {"memory_before", before.json()}, {"memory_after", after.json()},
            {"sampled_stage_peak", peak.json()}, {"memory_sample_count", QString::number(samples)},
            {"memory_sample_interval_ms", 5}, {"heartbeat", heartbeat.summary(firstTick)},
            {"detail", detail}});
}

void render(LogMainView& view)
{
    view.forceRefresh();
    // Baseline has no bounded async preview API; candidate must finish the real
    // background read before this diagnostic records its rendered outcome.
    if (view.metaObject()->indexOfMethod("isPreviewReady()") >= 0) {
        bool ready = false;
        view.repaint();
        while (!ready) {
            QMetaObject::invokeMethod(&view, "isPreviewReady", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, ready));
            if (ready) break;
            QEventLoop loop;
            QTimer::singleShot(10, &loop, &QEventLoop::quit);
            loop.exec();
        }
    }
    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter); // real view's paintEvent -> actual product text/line drawer
    if (view.metaObject()->indexOfMethod("previewRetainedBytes()") >= 0) {
        quint64 retained = 0;
        QMetaObject::invokeMethod(&view, "previewRetainedBytes", Qt::DirectConnection,
                                  Q_RETURN_ARG(quint64, retained));
        output({{"status", "preview_cache_observed"},
                {"retained_bytes", QString::number(retained)},
                {"scope", "frame strings and wrap rows; excludes in-flight worker scratch, allocator overhead, pixmaps and other Qt objects"}});
    }
}
} // namespace

class DiagnosticApplication : public QApplication {
public:
    using QApplication::QApplication;
    bool traceEvents = false;
    QJsonArray slowEvents;
    bool notify(QObject* receiver, QEvent* event) override
    {
        if (QThread::currentThread() != thread() || !traceEvents)
            return QApplication::notify(receiver, event);
        const QString receiverClass = receiver->metaObject()->className();
        const auto type = event->type();
        QElapsedTimer timer;
        timer.start();
        const bool result = QApplication::notify(receiver, event);
        const auto elapsed = timer.nsecsElapsed();
        if (elapsed >= 5'000'000 && slowEvents.size() < 256)
            slowEvents.append(QJsonObject{{"receiver_class", receiverClass}, {"event_type", int(type)},
                {"elapsed_ns", QString::number(elapsed)}});
        return result;
    }
};

int main(int argc, char** argv)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    DiagnosticApplication app(argc, argv);
    logging::enableLogging(false);
    app.setApplicationName("klogg_gui_probe");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("Real klogg LogMainView diagnostic; JSONL on stdout, no acceptance claim.");
    parser.addHelpOption();
    parser.addOptions({
        {{"f", "file"}, "Existing deterministic input fixture (not modified).", "path"},
        {{"a", "action"}, "render, wrap, selection, copy, copy-range, or all.", "name", "render"},
        {{"l", "line"}, "Zero-based source line.", "index", "0"},
        {"timeout-ms", "Whole-process wall-time limit including teardown.", "milliseconds", "120000"},
        {"anchor-byte", "Candidate-only absolute file-byte anchor for the requested line.", "offset"},
        {"expect-text", "Require this nonempty token in the actual cached preview window.", "token"},
        {"trace-events", "Record up to 256 event deliveries taking at least 5 ms (diagnostic only)."},
        {"label", "Caller-supplied baseline/candidate/diagnostic label.", "text", "unspecified"}});
    parser.process(app);
    bool lineOk = false, timeoutOk = false;
    const auto lineValue = parser.value("line").toULongLong(&lineOk);
    const auto timeout = parser.value("timeout-ms").toInt(&timeoutOk);
    const auto file = QFileInfo(parser.value("file"));
    const auto action = parser.value("action");
    const bool candidate = LogMainView::staticMetaObject.indexOfMethod("isPreviewReady()") >= 0;
    bool anchorOk = true;
    const auto anchor = parser.isSet("anchor-byte") ? parser.value("anchor-byte").toLongLong(&anchorOk) : 0;
    const QStringList actions{"render", "wrap", "selection", "copy", "copy-range", "all"};
    if (!lineOk || !timeoutOk || !anchorOk || anchor < 0 || timeout < 1000 || !file.isFile() || !file.isReadable()
        || (parser.isSet("expect-text") && parser.value("expect-text").isEmpty())
        || !actions.contains(action)) {
        output({{"status", "invalid_arguments"}, {"reason", "Readable --file, nonnegative --line, valid --action and timeout >=1000 required"}});
        return 2;
    }
    // This harness deliberately materializes selected text in its baseline
    // operations; do not accidentally point it at the separate 5 GiB index file.
    if (!candidate && (file.size() > 512LL * 1024 * 1024 || parser.isSet("anchor-byte") || parser.isSet("expect-text"))) {
        output({{"status", "unsupported"}, {"reason", "Baseline GUI fixtures are limited to 512 MiB and do not support byte-anchor/cached-text validation"}});
        return 2;
    }
    const QJsonObject metadata{{"mode", "gui"}, {"action", action},
        {"label", parser.value("label")}, {"file", file.absoluteFilePath()},
        {"file_size_bytes", QString::number(file.size())}, {"source_line", QString::number(lineValue)},
        {"qt_version", qVersion()}, {"platform", QGuiApplication::platformName()},
        {"candidate_api", candidate}, {"anchor_byte", parser.isSet("anchor-byte") ? QString::number(anchor) : QString{}},
        {"event_trace_enabled", parser.isSet("trace-events")},
        {"portable", true}, {"cache_state", "uncontrolled"},
        {"proof_boundary", "real product widget on reported Qt platform; process memory sampling is not exact allocation attribution or formal acceptance"}};
    output(QJsonObject{{"status", "started"}, {"metadata", metadata}});
    Monitor monitor(timeout, [metadata] { return metadata; });
    try {
        auto& config = Configuration::getSynced();
        config.setUseTextWrap(false);
        config.setOverviewVisible(false);
        config.setPollingEnabled(false);
        config.setNativeFileWatchEnabled(false);
        config.setUseSearchResultsCache(false);
        config.setIndexReadBufferSizeMb(1);
        HighlighterSetCollection::getSynced().deactivateAll();
        qRegisterMetaType<LoadingStatus>("LoadingStatus");
        qRegisterMetaType<LinesCount>("LinesCount");
        qRegisterMetaType<LineNumber>("LineNumber");
        qRegisterMetaType<LineLength>("LineLength");
        Heartbeat heartbeat;
        LogData data;
        LoadingStatus loadedStatus = LoadingStatus::Interrupted;
        bool loaded = false;
        QObject::connect(&data, &LogData::loadingFinished, &app, [&](LoadingStatus status) {
            loadedStatus = status;
            loaded = true;
        });
        perform({0, "index", [&] {
            data.setDisplayEncoding("UTF-8");
            data.attachFile(file.absoluteFilePath());
            QEventLoop loop;
            const auto connection = QObject::connect(&data, &LogData::loadingFinished, &loop, &QEventLoop::quit);
            if (!loaded) loop.exec();
            QObject::disconnect(connection);
            return QJsonObject{{"loading_status", static_cast<int>(loadedStatus)},
                {"line_count", QString::number(data.getNbLine().get())},
                {"display_encoding", "UTF-8"}, {"index_encoding", "auto-detected"}};
        }}, monitor, heartbeat);
        if (!loaded || loadedStatus != LoadingStatus::Successful || lineValue >= data.getNbLine().get()) {
            output({{"status", "failed"}, {"reason", "indexing did not complete successfully or requested line is outside index"}});
            return 3;
        }
        QuickFindPattern pattern;
        Overview overview;
        overview.setVisible(false);
        auto overviewWidget = std::make_unique<OverviewWidget>();
        overviewWidget->setOverview(&overview);
        std::unique_ptr<LogMainView> view;
        app.traceEvents = parser.isSet("trace-events");
        perform({1, "view_setup_and_initial_paint", [&] {
            QJsonObject setupStepNs;
            auto timed = [&](const char* name, const auto& action) {
                const auto begin = heartbeat.now();
                action();
                setupStepNs.insert(name, QString::number(heartbeat.now() - begin));
            };
            // A real OverviewWidget is mandatory: refreshOverview dereferences
            // it even when the overview itself is invisible.
            timed("construct_view", [&] {
                view = std::make_unique<LogMainView>(&data, &pattern, &overview, overviewWidget.get());
            });
            timed("resize", [&] { view->resize(1000, 600); });
            timed("update_font", [&] { view->updateFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); });
            timed("line_numbers", [&] { view->setLineNumbersVisible(true); });
            timed("update_data", [&] { view->updateData(); });
            timed("jump_to_line", [&] { view->jumpToLine(LineNumber(lineValue)); });
            timed("show", [&] { view->show(); });
            timed("render", [&] { render(*view); });
            return QJsonObject{{"widget", view->metaObject()->className()}, {"width", view->width()},
                               {"height", view->height()}, {"text_wrap", view->isTextWrapEnabled()},
                               {"first_visible_line", QString::number(view->getTopLine().get())},
                               {"font", view->font().toString()}, {"setup_step_ns", setupStepNs}};
        }}, monitor, heartbeat);
        if (app.traceEvents) {
            app.traceEvents = false;
            output({{"status", "setup_event_diagnostic"}, {"slow_events", app.slowEvents},
                    {"scope", "nested event delivery durations; do not sum nested events"}});
        }
        if (parser.isSet("anchor-byte")) {
            perform({9, "byte_anchor_jump_and_render", [&] {
                const bool invoked = QMetaObject::invokeMethod(view.get(), "showByteWindow", Qt::DirectConnection,
                    Q_ARG(LineNumber, LineNumber(lineValue)), Q_ARG(OffsetInFile, OffsetInFile(anchor)));
                if (!invoked) throw std::runtime_error("candidate byte-window slot invocation failed");
                render(*view);
                QVariantMap info;
                QMetaObject::invokeMethod(view.get(), "previewWindowInfo", Qt::DirectConnection,
                    Q_RETURN_ARG(QVariantMap, info), Q_ARG(quint64, lineValue));
                return QJsonObject{{"absolute_anchor_byte", QString::number(anchor)},
                                   {"cached_window", QJsonObject::fromVariantMap(info)}};
            }}, monitor, heartbeat);
        }
        if (parser.isSet("expect-text")) {
            QString cachedText;
            const bool invoked = QMetaObject::invokeMethod(view.get(), "previewWindowText", Qt::DirectConnection,
                Q_RETURN_ARG(QString, cachedText), Q_ARG(quint64, lineValue));
            const bool contains = invoked && cachedText.contains(parser.value("expect-text"));
            output({{"status", contains ? "cached_text_verified" : "failed"},
                    {"expected_token", parser.value("expect-text")}, {"found_in_cached_window", contains},
                    {"cached_utf16_units", QString::number(cachedText.size())},
                    {"verification_source", "actual LogMainView preview cache; no independent file read"}});
            if (!contains) throw std::runtime_error("expected token was absent from the actual GUI preview cache");
        }
        auto run = [&](const char* name) { return action == name || action == "all"; };
        if (run("render")) {
            perform({2, "forced_unwrapped_render", [&] {
                render(*view);
                return QJsonObject{{"text_wrap", view->isTextWrapEnabled()}};
            }}, monitor, heartbeat);
        }
        if (run("wrap")) {
            perform({3, "wrap_enable_and_render", [&] {
                view->textWrapSet(true);
                render(*view);
                return QJsonObject{{"text_wrap", view->isTextWrapEnabled()}};
            }}, monitor, heartbeat);
            perform({4, "wrap_disable_and_render", [&] {
                view->textWrapSet(false);
                render(*view);
                return QJsonObject{{"text_wrap", view->isTextWrapEnabled()}};
            }}, monitor, heartbeat);
        }
        if (run("selection")) {
            perform({5, "select_line_and_get_text_length", [&] {
                view->selectAndDisplayLine(LineNumber(lineValue));
                const auto selected = view->getSelectedText();
                return QJsonObject{{"selected_utf16_units", QString::number(selected.size())},
                                   {"contains_tab", selected.contains(QChar::Tabulation)}};
            }}, monitor, heartbeat);
        }
        for (const bool wholeRange : {false, true}) {
            if (!(wholeRange ? run("copy-range") : run("copy"))) continue;
            if (QGuiApplication::platformName() != "offscreen") {
                output({{"status", "unsupported"}, {"stage", "clipboard"},
                        {"reason", "Copy diagnostics run only offscreen to preserve the user's system clipboard"}});
                continue;
            }
            perform({wholeRange ? 8 : 6, wholeRange ? "select_all_and_copy_range" : "select_line_and_copy", [&] {
                if (wholeRange) view->selectAll();
                else view->selectAndDisplayLine(LineNumber(lineValue));
                QApplication::clipboard()->clear();
                const bool invoked = QMetaObject::invokeMethod(view.get(), "copy", Qt::DirectConnection);
                if (!invoked) throw std::runtime_error("real copy slot invocation failed");
                const auto clipboardText = QApplication::clipboard()->text();
                return QJsonObject{{"slot_invoked", invoked},
                    {"scope", wholeRange ? "all indexed lines" : "requested line"},
                    {"clipboard_utf16_units", QString::number(clipboardText.size())},
                    {"clipboard_backend", QGuiApplication::platformName()}};
            }}, monitor, heartbeat);
        }
        perform({7, "view_teardown", [&] {
            view.reset();
            if (QGuiApplication::platformName() == "offscreen") QApplication::clipboard()->clear();
            return QJsonObject{{"view_destroyed", true}};
        }}, monitor, heartbeat);
        output({{"status", "completed"}, {"metadata", metadata}, {"memory", memory().json()},
                {"acceptance", "not_assessed"}});
        return 0;
    } catch (const std::exception& error) {
        output({{"status", "failed"}, {"what", error.what()}, {"metadata", metadata},
                {"memory", memory().json()}});
        return 1;
    }
}
