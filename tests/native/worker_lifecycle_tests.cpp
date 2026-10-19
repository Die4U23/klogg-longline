// Regressions for LogDataWorker start/destruction and back-to-back operations.
// SPDX-License-Identifier: GPL-3.0-or-later
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <QApplication>
#include <QByteArray>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextCodec>

#include <memory>

#include "configuration.h"
#include "loadingstatus.h"
#include "logger.h"
#include "logdata.h"
#include "logdataworker.h"
#include "persistentinfo.h"

const bool PersistentInfo::ForcePortable = true;

namespace {
constexpr int kStressIterations = 100;
constexpr int kCompletionTimeoutMs = 30000;

class WorkerFixture {
public:
    WorkerFixture()
    {
        path_ = directory_.filePath( QStringLiteral( "worker-input.log" ) );
        firstPath_ = directory_.filePath( QStringLiteral( "first-input.log" ) );
        secondPath_ = directory_.filePath( QStringLiteral( "second-input.log" ) );
        QFile file( path_ );
        valid_ = directory_.isValid() && file.open( QIODevice::WriteOnly );
        if ( valid_ ) {
            // A physical file large enough to keep the worker schedulable while
            // the caller immediately destroys it or submits a second operation.
            const QByteArray block( 64 * 1024, 'x' );
            for ( int i = 0; i < 32 && valid_; ++i ) {
                valid_ = file.write( block ) == block.size() && file.write( "\n", 1 ) == 1;
            }
            valid_ = valid_ && file.flush();
        }
        file.close();
        const QByteArray firstBytes( "first-alpha\nfirst-beta\n" );
        const QByteArray secondBytes( "second-alpha\nsecond-beta\nsecond-gamma" );
        valid_ = valid_ && write( firstPath_, firstBytes ) && write( secondPath_, secondBytes );
        secondBytes_ = secondBytes;
    }

    bool valid() const { return valid_; }
    const QString& path() const { return path_; }
    const QString& firstPath() const { return firstPath_; }
    const QString& secondPath() const { return secondPath_; }
    const QByteArray& secondBytes() const { return secondBytes_; }

private:
    static bool write( const QString& path, const QByteArray& bytes )
    {
        QFile file( path );
        return file.open( QIODevice::WriteOnly ) && file.write( bytes ) == bytes.size()
               && file.flush();
    }

    QTemporaryDir directory_;
    QString path_;
    QString firstPath_;
    QString secondPath_;
    QByteArray secondBytes_;
    bool valid_ = false;
};

std::unique_ptr<LogDataWorker> makeAttachedWorker( const QString& path )
{
    auto indexingData = std::make_shared<IndexingData>();
    auto worker = std::make_unique<LogDataWorker>( indexingData );
    worker->attachFile( path );
    return worker;
}

QTextCodec* utf8Codec()
{
    return QTextCodec::codecForName( "UTF-8" );
}

void destroyImmediatelyAfterFullIndex( const QString& path )
{
    auto worker = makeAttachedWorker( path );
    worker->indexAll( utf8Codec() );
    worker.reset();
}

void submitPartialThenDestroy( const QString& path )
{
    auto worker = makeAttachedWorker( path );
    worker->indexAll( utf8Codec() );
    worker->indexAdditionalLines();
    worker.reset();
}

void submitCheckThenDestroy( const QString& path )
{
    auto worker = makeAttachedWorker( path );
    worker->indexAll( utf8Codec() );
    worker->checkFileChanges();
    worker.reset();
}

void submitAnotherFullIndex( const QString& path )
{
    auto worker = makeAttachedWorker( path );
    worker->indexAll( utf8Codec() );
    worker->indexAll( utf8Codec() );
    worker.reset();
}

bool waitForIndexCompletions( LogDataWorker& worker, int expectedCompletions,
                              int& successfulCompletions )
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot( true );
    bool timedOut = false;
    QObject::connect( &worker, &LogDataWorker::indexingFinished, &loop,
                      [&]( LoadingStatus status ) {
                          if ( status == LoadingStatus::Successful ) ++successfulCompletions;
                          if ( successfulCompletions >= expectedCompletions ) loop.quit();
                      }, Qt::QueuedConnection );
    QObject::connect( &timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    } );
    if ( successfulCompletions < expectedCompletions ) {
        timeout.start( kCompletionTimeoutMs );
        loop.exec();
    }
    return !timedOut && successfulCompletions >= expectedCompletions;
}
} // namespace

TEST_CASE( "LogDataWorker destruction immediately after full-index submission drains its task",
           "[worker][lifecycle][native]" )
{
    WorkerFixture fixture;
    REQUIRE( fixture.valid() );
    for ( int i = 0; i < kStressIterations; ++i )
        destroyImmediatelyAfterFullIndex( fixture.path() );
}

TEST_CASE( "LogDataWorker destruction immediately after partial-index submission drains its task",
           "[worker][lifecycle][native]" )
{
    WorkerFixture fixture;
    REQUIRE( fixture.valid() );
    for ( int i = 0; i < kStressIterations; ++i )
        submitPartialThenDestroy( fixture.path() );
}

TEST_CASE( "LogDataWorker destruction immediately after file-check submission drains its task",
           "[worker][lifecycle][native]" )
{
    WorkerFixture fixture;
    REQUIRE( fixture.valid() );
    for ( int i = 0; i < kStressIterations; ++i )
        submitCheckThenDestroy( fixture.path() );
}

TEST_CASE( "LogDataWorker accepts a second full-index request immediately after the first",
           "[worker][lifecycle][native]" )
{
    WorkerFixture fixture;
    REQUIRE( fixture.valid() );
    for ( int i = 0; i < kStressIterations; ++i )
        submitAnotherFullIndex( fixture.path() );
}

TEST_CASE( "LogData destruction immediately after attach drains the active index",
           "[worker][lifecycle][native]" )
{
    WorkerFixture fixture;
    REQUIRE( fixture.valid() );
    for ( int i = 0; i < kStressIterations; ++i ) {
        auto data = std::make_unique<LogData>();
        data->setDisplayEncoding( "UTF-8" );
        data->attachFile( fixture.path() );
        data.reset();
    }
}

TEST_CASE( "LogDataWorker reattaches to a second file and indexes its complete snapshot",
           "[worker][lifecycle][native]" )
{
    WorkerFixture fixture;
    REQUIRE( fixture.valid() );
    auto indexingData = std::make_shared<IndexingData>();
    LogDataWorker worker( indexingData );
    worker.attachFile( fixture.firstPath() );

    worker.indexAll( utf8Codec() );
    // This call races the first operation's worker-side lock acquisition. It
    // must wait for that operation before changing the filename snapshot.
    worker.attachFile( fixture.secondPath() );
    worker.indexAll( utf8Codec() );

    int observedCompletions = 0;
    REQUIRE( waitForIndexCompletions( worker, 2, observedCompletions ) );
    const IndexingData::ConstAccessor indexed{ indexingData.get() };
    REQUIRE( indexed.getNbLines().get() == 3 );
    REQUIRE( indexed.getIndexedSize() == fixture.secondBytes().size() );
}

int main( int argc, char* argv[] )
{
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
    QApplication application( argc, argv );
    logging::enableLogging( false );
    auto& config = Configuration::getSynced();
    config.setDefaultEncodingMib( 106 ); // UTF-8
    config.setNativeFileWatchEnabled( false );
    config.setPollingEnabled( false );
    qRegisterMetaType<LoadingStatus>( "LoadingStatus" );
    qRegisterMetaType<MonitoredFileStatus>( "MonitoredFileStatus" );
    return Catch::Session().run( argc, argv );
}
