// Diagnostic harness for the actual klogg libraries. SPDX-License-Identifier: GPL-3.0-or-later
// No production algorithms are copied or replaced by this harness.
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#endif

#include "configuration.h"
#include "linepositionarray.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "logger.h"
#include "persistentinfo.h"

// Same portable-settings seam used by upstream tests. Writes stay beside this
// diagnostic executable inside the ignored build directory.
const bool PersistentInfo::ForcePortable = true;

namespace {
std::mutex outputMutex;

void emitJson(QJsonObject object)
{
    std::lock_guard<std::mutex> lock(outputMutex);
    object.insert("evidence_kind", "diagnostic_actual_klogg_code");
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << '\n';
    std::cout.flush();
}

QString decimal(quint64 value) { return QString::number(value); }

QJsonObject memorySnapshot()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info))) {
        DWORD handles = 0;
        const bool measuredHandles = GetProcessHandleCount(GetCurrentProcess(), &handles);
        return {{"status", "measured"},
                {"working_set_bytes", decimal(info.WorkingSetSize)},
                {"peak_working_set_bytes", decimal(info.PeakWorkingSetSize)},
                {"private_bytes", decimal(info.PrivateUsage)},
                {"process_handle_count", measuredHandles ? QJsonValue(static_cast<int>(handles))
                                                         : QJsonValue(QJsonValue::Null)}};
    }
    return {{"status", "failed"}, {"win32_error", static_cast<double>(GetLastError())}};
#else
    return {{"status", "unsupported"}, {"reason", "OS process metrics implemented for Windows only"}};
#endif
}

// Also stops a blocked synchronous read or blocked destructor. Timeout exits are
// deliberately not normal cleanup; they cannot prove resource release.
class Deadline {
public:
    explicit Deadline(int timeoutMs) : worker_([this, timeoutMs] {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return done_; })) {
            emitJson({{"status", "timeout"}, {"timeout_ms", timeoutMs},
                      {"cleanup_verified", false}});
            std::_Exit(124);
        }
    }) {}
    ~Deadline()
    {
        { std::lock_guard<std::mutex> lock(mutex_); done_ = true; }
        condition_.notify_one();
        worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
    std::thread worker_;
};

int indexBoundary()
{
    struct Case { const char* name; int64_t base; int64_t span; };
    const std::vector<Case> cases{
        {"high_absolute_small_span", (int64_t{1} << 32) + 17, 127},
        {"relative_uint32_max", 1, std::numeric_limits<uint32_t>::max()},
        {"relative_2pow32", 1, int64_t{1} << 32}};
    bool allPassed = true;
    for (const auto& test : cases) {
        emitJson({{"status", "case_started"}, {"mode", "index-boundary"}, {"case", test.name}});
        QElapsedTimer timer;
        timer.start();
        LinePositionArray positions;
        std::vector<int64_t> expected;
        expected.reserve(128);
        for (int64_t i = 0; i < 128; ++i) {
            const auto value = test.base + test.span * i / 127;
            expected.push_back(value);
            positions.append(OffsetInFile(value));
        }
        const auto range = positions.range(0_lnum, 128_lcount);
        QJsonArray mismatches;
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto actual = positions.at(i).get();
            const auto rangeActual = range.at(i).get();
            if (actual != expected[i] || rangeActual != expected[i]) {
                mismatches.append(QJsonObject{{"index", static_cast<int>(i)},
                    {"expected", QString::number(expected[i])}, {"at", QString::number(actual)},
                    {"range", QString::number(rangeActual)}});
            }
        }
        const bool passed = mismatches.empty();
        allPassed = allPassed && passed;
        emitJson({{"status", passed ? "passed" : "failed"}, {"mode", "index-boundary"},
                  {"case", test.name}, {"elapsed_ns", QString::number(timer.nsecsElapsed())},
                  {"reported_index_allocated_bytes", decimal(positions.allocatedSize())},
                  {"reported_allocation_scope", "upstream allocatedSize excludes temporary vectors and allocator overhead"},
                  {"mismatches", mismatches}});
    }
    return allPassed ? 0 : 1;
}

int fileProbe(const QString& mode, const QString& fileName, quint64 line, int expectedByte,
              int timeoutMs, const QString& pattern = {}, int cancelMs = -1)
{
    const QFileInfo before(fileName);
    if (!before.isFile() || !before.isReadable()) {
        emitJson({{"status", "failed"}, {"reason", "file is missing or unreadable"}, {"file", fileName}});
        return 2;
    }
    constexpr qint64 maxMaterializedFixture = 512LL * 1024 * 1024;
    if (mode != "file-index" && mode != "search" && before.size() > maxMaterializedFixture) {
        emitJson({{"status", "unsupported"}, {"reason", "raw/preview probe limited to fixtures at most 512 MiB"}});
        return 3;
    }

    LogData logData;
    QEventLoop loop;
    bool finished = false;
    LoadingStatus status = LoadingStatus::Interrupted;
    QObject::connect(&logData, &LogData::loadingFinished, &loop, [&](LoadingStatus value) {
        status = value;
        finished = true;
        loop.quit();
    });
    QTimer loadTimer;
    loadTimer.setSingleShot(true);
    QObject::connect(&loadTimer, &QTimer::timeout, &loop, [&] {
        emitJson({{"status", "timeout"}, {"phase", "index_load"}, {"cleanup_verified", false}});
        std::_Exit(124);
    });
    QElapsedTimer timer;
    timer.start();
    loadTimer.start(timeoutMs);
    logData.attachFile(before.absoluteFilePath());
    if (!finished) loop.exec();
    loadTimer.stop();
    const auto loadNs = timer.nsecsElapsed();
    if (!finished || status != LoadingStatus::Successful) {
        emitJson({{"status", "failed"}, {"phase", "index_load"},
                  {"loading_status", static_cast<int>(status)}});
        return 1;
    }
    if (line >= logData.getNbLine().get()) {
        emitJson({{"status", "failed"}, {"reason", "requested line out of range"},
                  {"indexed_lines", decimal(logData.getNbLine().get())}});
        return 1;
    }
    const auto memoryBefore = memorySnapshot();
    emitJson({{"status", "operation_started"}, {"mode", mode}, {"file", before.absoluteFilePath()},
              {"line_zero_based", decimal(line)}, {"file_size_bytes", QString::number(before.size())},
              {"indexed_lines", decimal(logData.getNbLine().get())},
              {"index_elapsed_ns", QString::number(loadNs)}, {"encoding", "UTF-8 forced"}});
    timer.restart();
    bool passed = true;
    QJsonObject result{{"mode", mode}, {"file", before.absoluteFilePath()},
                      {"line_zero_based", decimal(line)}, {"index_elapsed_ns", QString::number(loadNs)},
                      {"indexed_lines", decimal(logData.getNbLine().get())}, {"memory_before", memoryBefore}};
    if (mode == "search") {
        auto filtered = logData.getNewFilteredData();
        QEventLoop searchLoop;
        QTimer cancelTimer, heartbeat;
        cancelTimer.setSingleShot(true);
        qint64 cancelRequestedNs = -1, maxHeartbeatNs = 0;
        QElapsedTimer heartbeatClock;
        heartbeatClock.start();
        QObject::connect(&heartbeat, &QTimer::timeout, &searchLoop, [&] {
            maxHeartbeatNs = std::max(maxHeartbeatNs, heartbeatClock.nsecsElapsed());
            heartbeatClock.restart();
        });
        QObject::connect(&cancelTimer, &QTimer::timeout, &searchLoop, [&] {
            cancelRequestedNs = timer.nsecsElapsed();
            filtered->interruptSearch();
        });
        bool searchDone = false;
        QObject::connect(filtered.get(), &LogFilteredData::searchProgressed, &searchLoop,
                         [&](LinesCount, int percent, LineNumber) {
            if (percent == 100) { searchDone = true; searchLoop.quit(); }
        });
        heartbeat.start(10);
        if (cancelMs >= 0) cancelTimer.start(cancelMs);
        timer.restart();
        filtered->runSearch(RegularExpressionPattern(pattern, true, false, false, true));
        if (!searchDone) searchLoop.exec();
        const auto finishedNs = timer.nsecsElapsed();
        cancelTimer.stop();
        heartbeat.stop();
        maxHeartbeatNs = std::max(maxHeartbeatNs, heartbeatClock.nsecsElapsed());
        result.insert("operation_elapsed_ns", QString::number(finishedNs));
        result.insert("cancel_requested_ns", QString::number(cancelRequestedNs));
        result.insert("cancel_to_completion_ns", cancelRequestedNs < 0 ? "not_requested" : QString::number(finishedNs - cancelRequestedNs));
        result.insert("max_event_loop_heartbeat_gap_ns", QString::number(maxHeartbeatNs));
        result.insert("matched_lines_count", decimal(filtered->getNbMatches().get()));
        QJsonArray matchedLines;
        const auto boundedCount = std::min<uint64_t>(filtered->getNbMatches().get(), 100000);
        for (uint64_t i = 0; i < boundedCount; ++i)
            matchedLines.append(decimal(filtered->getMatchingLineNumber(LineNumber(i)).get()));
        result.insert("matched_lines_zero_based", matchedLines);
#ifdef KLOGG_LONG_LINE_CANDIDATE
        result.insert("search_path", logData.getMaxLength().get() >= 1024 * 1024 ? "bounded_literal" : "legacy");
        result.insert("search_completion", searchCompletionName(filtered->searchCompletion()));
        result.insert("search_message", filtered->searchMessage());
        result.insert("peak_search_buffer_bytes", decimal(filtered->peakSearchBufferBytes()));
        QJsonArray offsets;
        for (uint64_t i = 0; i < boundedCount; ++i) {
            const auto sourceLine = filtered->getMatchingLineNumber(LineNumber(i));
            const auto offset = filtered->firstMatchByte(sourceLine);
            offsets.append(offset ? QString::number(offset->get()) : QJsonValue(QJsonValue::Null));
        }
        result.insert("first_hit_byte_offsets", offsets);
#endif
        result.insert("line_list_truncated", filtered->getNbMatches().get() > boundedCount);
        result.insert("memory_while_result_alive", memorySnapshot());
        result.insert("scope", "real literal search; completion signal is observed, output must be compared to independent oracle; event loop heartbeat excludes widget painting");
        passed = searchDone;
    } else if (mode == "preview") {
        const auto lines = logData.getLines(LineNumber(line), 1_lcount);
        result.insert("operation_elapsed_ns", QString::number(timer.nsecsElapsed()));
        result.insert("decoded_line_count", static_cast<int>(lines.size()));
        result.insert("decoded_utf16_units", lines.empty() ? "0" : QString::number(lines.front().size()));
        result.insert("scope", "synchronous backend getLines; no widget, paint, or GUI heartbeat measurement");
        passed = lines.size() == 1;
        result.insert("memory_while_result_alive", memorySnapshot());
    } else {
        const auto raw = logData.getLinesRaw(LineNumber(line), 1_lcount);
        result.insert("operation_elapsed_ns", QString::number(timer.nsecsElapsed()));
        result.insert("raw_buffer_size_bytes", decimal(raw.buffer.size()));
        result.insert("raw_buffer_capacity_bytes", decimal(raw.buffer.capacity()));
        result.insert("line_endpoints_count", static_cast<int>(raw.endOfLines.size()));
        result.insert("memory_while_result_alive", memorySnapshot());
        if (mode == "file-index") {
            const auto actual = raw.buffer.empty() ? -1 : static_cast<unsigned char>(raw.buffer.front());
            result.insert("actual_first_byte", actual);
            result.insert("expected_byte", expectedByte);
            passed = raw.buffer.size() == 1 && actual == expectedByte;
            result.insert("assertion", "exactly one byte, equal to expected-byte");
        } else {
            passed = raw.endOfLines.size() == 1 && !raw.buffer.empty();
            result.insert("scope", "allocation observation only; no full search or content correctness assertion");
        }
    }
    const QFileInfo after(before.absoluteFilePath());
    const bool sameMetadata = before.size() == after.size() && before.lastModified() == after.lastModified();
    result.insert("file_size_bytes", QString::number(before.size()));
    result.insert("source_metadata_unchanged", sameMetadata);
    result.insert("hash_verified_by_probe", false);
    result.insert("memory_after_result_release", memorySnapshot());
    result.insert("status", passed && sameMetadata ? "passed" : "failed");
    emitJson(result);
    return passed && sameMetadata ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    QStringList args;
#ifdef _WIN32
    int wideArgc = 0;
    auto* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (!wideArgv) return 2;
    for (int i = 1; i < wideArgc; ++i) args.append(QString::fromWCharArray(wideArgv[i]));
    LocalFree(wideArgv);
#else
    for (int i = 1; i < argc; ++i) args.append(QString::fromLocal8Bit(argv[i]));
#endif
    Q_UNUSED(argc);
    if (args.empty() || args.contains("--help")) {
        emitJson({{"status", "help"}, {"usage", QJsonArray{
            "index-boundary [--timeout-ms N]", "raw-allocation FILE [--timeout-ms N]",
            "file-index FILE ZERO_BASED_LINE EXPECTED_BYTE [--timeout-ms N]",
            "search FILE PATTERN [--cancel-ms N] [--timeout-ms N]",
            "preview FILE [--timeout-ms N]"}},
            {"expected_byte", "decimal integer 0..255; file-index asserts one-byte line"},
            {"default_timeout_ms", 120000}, {"unsupported", QJsonArray{
            "GUI paint acceptance", "raw/preview fixture larger than 512 MiB",
            "process memory metrics outside Windows", "fixture hashing (verify externally)"}}});
        return args.empty() ? 2 : 0;
    }
    int timeoutMs = 120000;
    int cancelMs = -1;
    const int cancelIndex = args.indexOf("--cancel-ms");
    if (cancelIndex >= 0) {
        bool ok = false;
        if (cancelIndex + 1 < args.size()) cancelMs = args.at(cancelIndex + 1).toInt(&ok);
        if (!ok || cancelMs < 0) return 2;
        args.removeAt(cancelIndex + 1);
        args.removeAt(cancelIndex);
    }
    const int timeoutIndex = args.indexOf("--timeout-ms");
    if (timeoutIndex >= 0) {
        bool ok = false;
        if (timeoutIndex + 1 < args.size()) timeoutMs = args.at(timeoutIndex + 1).toInt(&ok);
        if (!ok || timeoutMs < 1) {
            emitJson({{"status", "failed"}, {"reason", "invalid timeout"}});
            return 2;
        }
        args.removeAt(timeoutIndex + 1);
        args.removeAt(timeoutIndex);
    }
    const auto mode = args.front();
    const bool indexMode = mode == "index-boundary";
    const bool fileIndex = mode == "file-index";
    const bool searchMode = mode == "search";
    if ((indexMode && args.size() != 1) || (fileIndex && args.size() != 4)
        || (searchMode && args.size() != 3)
        || ((mode == "raw-allocation" || mode == "preview") && args.size() != 2)
        || (!indexMode && !fileIndex && !searchMode && mode != "raw-allocation" && mode != "preview")) {
        emitJson({{"status", "unsupported"}, {"reason", "unknown mode or invalid arguments; use --help"}});
        return 2;
    }
    quint64 line = 0;
    int expectedByte = -1;
    if (fileIndex) {
        bool lineOk = false, byteOk = false;
        line = args.at(2).toULongLong(&lineOk);
        expectedByte = args.at(3).toInt(&byteOk);
        if (!lineOk || !byteOk || expectedByte < 0 || expectedByte > 255) {
            emitJson({{"status", "failed"}, {"reason", "invalid line or byte"}});
            return 2;
        }
    }
    Deadline deadline(timeoutMs);
    try {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        int qtArgc = 1;
        QApplication application(qtArgc, argv);
        logging::enableLogging(false);
        qRegisterMetaType<LoadingStatus>("LoadingStatus");
        qRegisterMetaType<LinesCount>("LinesCount");
        qRegisterMetaType<LineNumber>("LineNumber");
        qRegisterMetaType<LineLength>("LineLength");
        auto& config = Configuration::getSynced();
        config.setDefaultEncodingMib(106); // UTF-8; AttachOperation uses this codec for indexing.
        config.setIndexReadBufferSizeMb(1);
        config.setPollingEnabled(false);
        config.setNativeFileWatchEnabled(false);
        config.setUseSearchResultsCache(false);
        config.setSearchThreadPoolSize(4);
        emitJson({{"status", "probe_started"}, {"mode", mode}, {"timeout_ms", timeoutMs},
                  {"qt_version", qVersion()}, {"settings", "portable beside diagnostic executable"}});
        return indexMode ? indexBoundary() : fileProbe(mode, args.at(1), line, expectedByte, timeoutMs,
                                                       searchMode ? args.at(2) : QString{}, cancelMs);
    } catch (const std::exception& error) {
        emitJson({{"status", "failed"}, {"reason", QString::fromUtf8(error.what())}});
        return 1;
    } catch (...) {
        emitJson({{"status", "failed"}, {"reason", "non-standard exception"}});
        return 1;
    }
}
