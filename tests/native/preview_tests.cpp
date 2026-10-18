// Actual klogg bounded-preview and text compatibility tests.
// SPDX-License-Identifier: GPL-3.0-or-later
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <memory>
#include "configuration.h"
#include "logdata.h"
#include "logger.h"
#include "persistentinfo.h"
#include "selection.h"
#include "quickfind.h"
#include "quickfindpattern.h"
#include "wrappedstring.h"

const bool PersistentInfo::ForcePortable = true;

namespace {
// Small compatibility reference for the original mutation-based tab behavior;
// deliberately used only on tiny strings, never as a product implementation.
QString oldExpansion(QString text, qsizetype initial)
{
    text.replace(QChar::Null, QChar::Space);
    auto position = text.indexOf(QChar::Tabulation);
    while (position >= 0) {
        const auto spaces = TabStop - ((initial + position) % TabStop);
        text.replace(position, 1, QString(spaces, QChar::Space));
        position = text.indexOf(QChar::Tabulation, position);
    }
    return text;
}
struct Fixture {
    QTemporaryDir directory{QDir(QCoreApplication::applicationDirPath()).filePath("preview-test-XXXXXX")};
    QString path;
    std::unique_ptr<LogData> data;
    explicit Fixture(const QByteArray& bytes)
    {
        REQUIRE(directory.isValid());
        path = directory.filePath("input.log");
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(bytes) == bytes.size());
        file.close();
        data = std::make_unique<LogData>();
        data->setDisplayEncoding("UTF-8");
        QEventLoop loop;
        bool finished = false;
        LoadingStatus status = LoadingStatus::Interrupted;
        QObject::connect(data.get(), &LogData::loadingFinished, &loop, [&](LoadingStatus value) {
            finished = true;
            status = value;
            loop.quit();
        });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(10000);
        data->attachFile(path);
        if (!finished) loop.exec();
        REQUIRE(finished);
        REQUIRE(status == LoadingStatus::Successful);
        REQUIRE(data->matchesSnapshot(data->snapshot()));
    }
};
}

TEST_CASE("Linear expansion preserves tabs NUL Unicode CR and initial column", "[preview][tabs]")
{
    QString input = QString::fromUtf8("a\t\xc3\xa9\t\xf0\x9f\x98\x80\r");
    input.append(QChar::Null);
    input.append("\tZ");
    for (qsizetype origin = 0; origin < 17; ++origin) {
        REQUIRE(untabify(QString(input), LineColumn(origin)) == oldExpansion(input, origin));
    }
    REQUIRE(untabify(QString("\t\t")) == QString(16, QChar::Space));
    REQUIRE(untabify(QString("x\r")) == QString("x\r"));
    REQUIRE(untabify(QString{}) == QString{});
}

TEST_CASE("Wrapped rows retain text through shared copies", "[preview][wrap]")
{
    const auto make = [] {
        WrappedString original(QString::fromUtf8("one two \xe4\xb8\x89 four"), LineLength(5));
        return WrappedString(original);
    };
    const auto copy = make();
    REQUIRE(copy.unwrappedLine() == QString::fromUtf8("one two \xe4\xb8\x89 four"));
    REQUIRE(copy.wrappedLinesCount() > 1);
    QString joined;
    for (size_t row = 0; row < copy.wrappedLinesCount(); ++row) joined += copy.wrappedLine(row);
    REQUIRE(joined == copy.unwrappedLine());
    const WrappedString empty(QString{}, LineLength(80));
    REQUIRE(empty.wrappedLinesCount() == 1);
    REQUIRE(empty.isEmpty());
    REQUIRE(WrappedString(QString("abc"), 0_length).wrappedLinesCount() == 3);
}

TEST_CASE("Preview reads real capped byte ranges and preserves final unterminated content", "[preview][file]")
{
    const QByteArray shortLine("short\t\xc3\xa9\r\n");
    const QByteArray longLine(80 * 1024, 'a');
    Fixture fixture(shortLine + longLine + "\n" + QByteArray("tail\xc3\xa9"));
    auto& data = *fixture.data;
    const auto snap = data.snapshot();
    data.setDisplayEncoding("UTF-8");
    REQUIRE(data.snapshot().generation == snap.generation);
    REQUIRE(data.matchesSnapshot(snap));
    REQUIRE(data.getNbLine() == 3_lcount);
    const auto normal = data.linePreview(0_lnum);
    REQUIRE(normal.error.isEmpty());
    REQUIRE_FALSE(normal.truncated);
    REQUIRE(normal.text == QString::fromUtf8("short\t\xc3\xa9"));
    const auto longPreview = data.linePreview(1_lnum);
    REQUIRE(longPreview.error.isEmpty());
    REQUIRE(longPreview.truncated);
    REQUIRE(longPreview.text == QString(AbstractLogData::PreviewByteLimit, QChar('a')));
    REQUIRE(longPreview.byteEnd - longPreview.byteBegin <= AbstractLogData::PreviewByteLimit);
    REQUIRE(longPreview.absoluteLineBegin == shortLine.size());
    const auto finalRange = data.lineByteRange(2_lnum, snap);
    REQUIRE(finalRange.has_value());
    REQUIRE(finalRange->end.get() == snap.size);
    const auto tail = data.linePreview(2_lnum);
    REQUIRE(tail.text == QString::fromUtf8("tail\xc3\xa9"));
    const auto rawTail = data.getLinesRaw(2_lnum, 1_lcount);
    REQUIRE(rawTail.buffer.size() == 6);
    REQUIRE(data.getLinesRaw(0_lnum, 0_lcount).buffer.empty());
    const auto rejected = data.readBytes(snap, OffsetInFile(0), LogData::MaxByteRead + 1);
    REQUIRE(rejected.status == LogData::ReadStatus::TooLarge);
    REQUIRE(rejected.bytes.isEmpty());
    Selection selection;
    selection.selectLine(1_lnum);
    REQUIRE_THROWS_AS(selection.getSelectedText(&data), std::length_error);
    selection.selectLine(0_lnum);
    REQUIRE(selection.getSelectedText(&data) == normal.text);
    selection.selectRange(0_lnum, 2_lnum);
    REQUIRE_FALSE(selection.isEmpty());
    selection.clear();
    REQUIRE(selection.isEmpty());
    REQUIRE(selection.getSelectedText(&data).isEmpty());
}

TEST_CASE("UTF8 windows align byte anchors and incomplete trailing sequences", "[preview][utf8]")
{
    const QByteArray prefix(16383, 'a');
    Fixture fixture(prefix + QByteArray("\xe2\x82\xac") + QByteArray(40000, 'b'));
    auto& data = *fixture.data;
    const auto head = data.linePreview(0_lnum);
    REQUIRE(head.text == QString(16383, QChar('a')));
    REQUIRE(head.byteEnd == 16383);
    const auto atCharacter = data.linePreview(0_lnum, 16383);
    REQUIRE(atCharacter.text.startsWith(QChar(0x20ac)));
    REQUIRE(atCharacter.byteBegin == 16383);
    const auto insideCharacter = data.linePreview(0_lnum, 16384);
    REQUIRE(insideCharacter.byteBegin == 16386);
    REQUIRE(insideCharacter.text.startsWith(QChar('b')));
    REQUIRE_FALSE(insideCharacter.text.contains(QChar::ReplacementCharacter));
}

TEST_CASE("A changed file cannot be blessed with a new snapshot over an old index", "[preview][source]")
{
    Fixture fixture(QByteArray(80000, 'x'));
    const auto before = fixture.data->snapshot();
    QFile file(fixture.path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.resize(1));
    file.close();
    REQUIRE_FALSE(fixture.data->matchesSnapshot(before));
    REQUIRE_FALSE(fixture.data->matchesSnapshot(fixture.data->snapshot()));
    const auto read = fixture.data->readBytes(before, OffsetInFile(0), 16);
    REQUIRE(read.status == LogData::ReadStatus::SourceChanged);
    REQUIRE(read.bytes.isEmpty());
    REQUIRE_FALSE(fixture.data->linePreview(0_lnum).error.isEmpty());
}

TEST_CASE("Quick Find preserves expanded tab columns and rejects long rows", "[preview][quickfind]")
{
    Configuration::get().setQuickfindRegexpType(SearchRegexpType::FixedString);
    QuickFindPattern pattern;
    pattern.changeSearchPattern("needle", false, false);
    {
        Fixture fixture("first\n\tneedle\nlast\n");
        QuickFind finder(*fixture.data);
        QEventLoop loop;
        bool found = false;
        Portion result;
        QObject::connect(&finder, &QuickFind::searchDone, &loop, [&](bool match, Portion portion) {
            found = match;
            result = portion;
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        finder.searchForward(Selection{}, pattern.getMatcher());
        loop.exec();
        finder.stopSearch();
        REQUIRE(found);
        REQUIRE(result.isValid());
        REQUIRE(result.line() == 1_lnum);
        REQUIRE(result.startColumn() == 8_lcol);
        REQUIRE(result.endColumn() == 13_lcol);
    }
    {
        Fixture fixture(QByteArray(80000, 'a') + "needle\n");
        QuickFind finder(*fixture.data);
        QEventLoop loop;
        QString notification;
        QObject::connect(&finder, &QuickFind::notify, &loop, [&](const QFNotification& message) {
            notification = message.message();
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        finder.searchForward(Selection{}, pattern.getMatcher());
        loop.exec();
        finder.stopSearch();
        REQUIRE(notification.contains("cannot search this long line"));
    }
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    logging::enableLogging(false);
    auto& config = Configuration::getSynced();
    config.setPollingEnabled(false);
    config.setNativeFileWatchEnabled(false);
    config.setUseSearchResultsCache(false);
    qRegisterMetaType<LoadingStatus>("LoadingStatus");
    return Catch::Session().run(argc, argv);
}
