// Real CrawlerWidget/Session regression tests, using the upstream access_by seam.
// SPDX-License-Identifier: GPL-3.0-or-later
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>

#include "configuration.h"
#include "crawlerwidget.h"
#include "filewatcher.h"
#include "highlighterset.h"
#include "infoline.h"
#include "logger.h"
#include "persistentinfo.h"
#include "quickfindpattern.h"
#include "savedsearches.h"
#include "session.h"

const bool PersistentInfo::ForcePortable = true;

struct CrawlerRegressionPrivate {};
template <>
struct CrawlerWidget::access_by<CrawlerRegressionPrivate> {
    std::unique_ptr<CrawlerWidget> crawler;
    LogData* data() const { return crawler->logData_.get(); }
    LogFilteredData* filtered() const { return crawler->logFilteredData_.get(); }
    LogMainView* view() const { return crawler->logMainView_; }
    bool loaded() const { return !crawler->loadingInProgress_; }
    QString info() const { return crawler->searchInfoLine_->text(); }
    void search(const QString& pattern, bool autoRefresh = false)
    {
        crawler->matchCaseButton_->setChecked(true);
        crawler->useRegexpButton_->setChecked(false);
        crawler->inverseButton_->setChecked(false);
        crawler->booleanButton_->setChecked(false);
        crawler->searchRefreshButton_->setChecked(autoRefresh);
        crawler->searchLineEdit_->setEditText(pattern);
        REQUIRE(crawler->searchLineEdit_->lineEdit()->maxLength() == 1024 * 1024);
        REQUIRE(crawler->searchLineEdit_->currentText() == pattern);
        crawler->startNewSearch();
    }
    void enableFollow()
    {
        Q_EMIT crawler->followSet(true);
    }
};
using CrawlerVisitor = CrawlerWidget::access_by<CrawlerRegressionPrivate>;

namespace {
bool waitUntil(const std::function<bool()>& ready, int milliseconds = 15000)
{
    if (ready()) return true;
    QEventLoop loop;
    QTimer heartbeat, deadline;
    heartbeat.setInterval(10);
    deadline.setSingleShot(true);
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { if (ready()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    heartbeat.start();
    deadline.start(milliseconds);
    loop.exec();
    return ready();
}

struct CrawlerFixture {
    QTemporaryDir directory{QDir(QCoreApplication::applicationDirPath()).filePath("crawler-test-XXXXXX")};
    QString path;
    Session session;
    CrawlerVisitor visitor;
    explicit CrawlerFixture(const QByteArray& bytes)
    {
        REQUIRE(directory.isValid());
        path = directory.filePath("input.log");
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(bytes) == bytes.size());
        file.close();
        session.savedSearches().clear();
        visitor.crawler.reset(static_cast<CrawlerWidget*>(session.open(path, [] { return new CrawlerWidget; })));
        visitor.crawler->resize(1000, 700);
        visitor.crawler->show();
        REQUIRE(waitUntil([&] { return visitor.loaded(); }));
        REQUIRE(visitor.data()->matchesSnapshot(visitor.data()->snapshot()));
    }
    void close()
    {
        if (visitor.crawler) {
            session.close(visitor.crawler.get());
            visitor.crawler.reset();
        }
        // Flush receiver-owned notifications/deferred child deletion after the
        // data/view have been released. Normal test success is no sanitizer proof.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
    ~CrawlerFixture() { close(); }
};
}

TEST_CASE("Real Crawler search accepts a one-MiB literal without regex compilation", "[crawler][literal]")
{
    const auto pattern = QString(1024 * 1024 - 3, QChar('a')) + "END";
    const QByteArray bom("\xef\xbb\xbf");
    CrawlerFixture fixture(bom + pattern.toUtf8() + "\n" + QByteArray(32767, 'a') + "NOEND\n");
    fixture.visitor.search(pattern);
    REQUIRE(waitUntil([&] { return fixture.visitor.filtered()->searchCompletion() != SearchCompletion::Running; }, 30000));
    INFO(fixture.visitor.info().toStdString());
    INFO(fixture.visitor.filtered()->searchMessage().toStdString());
    REQUIRE(fixture.visitor.filtered()->searchCompletion() == SearchCompletion::Complete);
    REQUIRE(fixture.visitor.filtered()->getNbMatches() == 1_lcount);
    REQUIRE(fixture.visitor.filtered()->getMatchingLineNumber(0_lnum) == 0_lnum);
    REQUIRE(fixture.visitor.filtered()->firstMatchByte(0_lnum).has_value());
    REQUIRE(fixture.visitor.filtered()->firstMatchByte(0_lnum)->get() == bom.size());
    REQUIRE_FALSE(fixture.visitor.info().contains("Error in expression"));
}

TEST_CASE("Real polling follow appends with unchanged codec and preserves old and new hits", "[crawler][follow]")
{
    // Real file writes plus the actual product polling watcher; no synthetic
    // fileChanged/loaded/search signals. Native watcher timing is not covered.
    CrawlerFixture fixture(QByteArray("\xef\xbb\xbf") + QByteArray(1024 * 1024 + 17, 'x')
                           + " NEEDLE\nold NEEDLE\n");
    fixture.visitor.enableFollow();
    REQUIRE(fixture.visitor.crawler->isFollowEnabled());
    fixture.visitor.search("NEEDLE", true);
    REQUIRE(waitUntil([&] { return fixture.visitor.filtered()->searchCompletion() != SearchCompletion::Running; }));
    REQUIRE(fixture.visitor.filtered()->searchCompletion() == SearchCompletion::Complete);
    REQUIRE(fixture.visitor.filtered()->getNbMatches() == 2_lcount);
    const auto oldFirst = fixture.visitor.filtered()->firstMatchByte(0_lnum);
    const auto oldSecond = fixture.visitor.filtered()->firstMatchByte(1_lnum);
    REQUIRE(oldFirst.has_value());
    REQUIRE(oldSecond.has_value());
    const auto before = fixture.visitor.data()->snapshot();
    fixture.visitor.crawler->setEncoding(106);
    REQUIRE(fixture.visitor.data()->snapshot().generation == before.generation);
    REQUIRE(fixture.visitor.data()->matchesSnapshot(before));
    const auto signals = std::make_shared<std::pair<int, int>>(0, 0);
    QObject::connect(fixture.visitor.data(), &LogData::fileChanged, fixture.visitor.crawler.get(),
                     [signals](MonitoredFileStatus) { ++signals->first; });
    QObject::connect(fixture.visitor.crawler.get(), &CrawlerWidget::loadingFinished, fixture.visitor.crawler.get(),
                     [signals](LoadingStatus) { ++signals->second; });
    QFile append(fixture.path);
    REQUIRE(append.open(QIODevice::WriteOnly | QIODevice::Append));
    REQUIRE(append.write("new NEEDLE\n") == 11);
    REQUIRE(append.flush());
    append.close();
    const auto followed = waitUntil([&] {
        return signals->first > 0 && signals->second > 0
               && fixture.visitor.data()->getNbLine() == 3_lcount
               && fixture.visitor.filtered()->searchCompletion() == SearchCompletion::Complete
               && fixture.visitor.filtered()->getNbMatches() == 3_lcount;
    }, 30000);
    INFO("polling watcher fileChanged=" << signals->first << ", loadingFinished=" << signals->second);
    INFO(fixture.visitor.filtered()->searchMessage().toStdString());
    INFO(fixture.visitor.info().toStdString());
    REQUIRE(followed);
    const auto retainedFirst = fixture.visitor.filtered()->firstMatchByte(0_lnum);
    const auto retainedSecond = fixture.visitor.filtered()->firstMatchByte(1_lnum);
    REQUIRE(retainedFirst.has_value());
    REQUIRE(retainedSecond.has_value());
    REQUIRE(retainedFirst->get() == oldFirst->get());
    REQUIRE(retainedSecond->get() == oldSecond->get());
    REQUIRE(fixture.visitor.filtered()->firstMatchByte(2_lnum).has_value());
    for (uint64_t i = 0; i < 3; ++i)
        REQUIRE(fixture.visitor.filtered()->getMatchingLineNumber(LineNumber(i)) == LineNumber(i));
    REQUIRE_FALSE(fixture.visitor.info().contains("source_changed"));
}

TEST_CASE("Closing real Crawler joins queued or running preview and Quick Find work", "[crawler][close]")
{
    QByteArray bytes;
    for (int i = 0; i < 20000; ++i) bytes += "short ordinary row without the target\n";
    for (int iteration = 0; iteration < 5; ++iteration) {
        CrawlerFixture fixture(bytes);
        fixture.session.quickFindPattern()->changeSearchPattern("ABSENT_NEEDLE", false, false);
        fixture.visitor.view()->searchForward();
        const auto range = fixture.visitor.data()->lineByteRange(1_lnum, fixture.visitor.data()->snapshot());
        REQUIRE(range.has_value());
        fixture.visitor.view()->showByteWindow(1_lnum, range->begin);
        fixture.visitor.crawler->grab(); // schedules the real preview worker
        REQUIRE_FALSE(fixture.visitor.view()->isPreviewReady());
        const QPointer<LogMainView> viewGuard(fixture.visitor.view());
        const QPointer<LogData> dataGuard(fixture.visitor.data());
        const QPointer<CrawlerWidget> crawlerGuard(fixture.visitor.crawler.get());
        fixture.close();
        REQUIRE(crawlerGuard.isNull());
        REQUIRE(viewGuard.isNull());
        REQUIRE(dataGuard.isNull());
    }
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    logging::enableLogging(false);
    auto& config = Configuration::getSynced();
    config.setDefaultEncodingMib(106);
    config.setUseTextWrap(false);
    config.setMainRegexpType(SearchRegexpType::FixedString);
    config.setQuickfindRegexpType(SearchRegexpType::FixedString);
    config.setSearchIgnoreCaseDefault(false);
    config.setSearchAutoRefreshDefault(false);
    config.setSearchLogicalCombiningDefault(false);
    config.setAutoRunSearchOnPatternChange(false);
    config.setUseSearchResultsCache(false);
    config.setIndexReadBufferSizeMb(1);
    config.setNativeFileWatchEnabled(false);
    config.setPollingEnabled(true);
    config.setPollIntervalMs(20);
    HighlighterSetCollection::getSynced().deactivateAll();
    FileWatcher::getFileWatcher().updateConfiguration();
    qRegisterMetaType<LoadingStatus>("LoadingStatus");
    qRegisterMetaType<MonitoredFileStatus>("MonitoredFileStatus");
    qRegisterMetaType<LinesCount>("LinesCount");
    qRegisterMetaType<LineNumber>("LineNumber");
    qRegisterMetaType<LineLength>("LineLength");
    return Catch::Session().run(argc, argv);
}
