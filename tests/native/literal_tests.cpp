// SPDX-License-Identifier: GPL-3.0-or-later

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include <configuration.h>
#include <logger.h>
#include <persistentinfo.h>

#include "logdata.h"
#include "logfiltereddata.h"
#include "longlinesearch.h"
#include "regularexpressionpattern.h"

const bool PersistentInfo::ForcePortable = true;

namespace {
constexpr int kLoadTimeoutMs = 30000;
constexpr int kSearchTimeoutMs = 60000;

class AttachedSource {
public:
    AttachedSource() : path_( directory_.filePath( QStringLiteral( "literal-input.log" ) ) ) {}

    bool write( const QByteArray& bytes ) const
    {
        QFile file( path_ );
        return file.open( QIODevice::WriteOnly | QIODevice::Truncate )
               && file.write( bytes ) == bytes.size() && file.flush();
    }

    bool attach( const QByteArray& bytes )
    {
        if ( !directory_.isValid() || !write( bytes ) ) return false;
        source_ = std::make_unique<LogData>();
        // These fixtures explicitly exercise UTF-8, independently of encoding guess.
        source_->setDisplayEncoding("UTF-8");

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot( true );
        LoadingStatus status = LoadingStatus::Interrupted;
        bool received = false;
        QObject::connect( source_.get(), &LogData::loadingFinished, &loop,
                          [&]( LoadingStatus value ) {
                              status = value;
                              received = true;
                              loop.quit();
                          } );
        QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
        source_->attachFile( path_ );
        if ( !received ) {
            timeout.start( kLoadTimeoutMs );
            loop.exec();
        }
        return received && status == LoadingStatus::Successful;
    }

    bool reloadAndWait()
    {
        if ( !source_ ) return false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot( true );
        bool received = false;
        LoadingStatus status = LoadingStatus::Interrupted;
        const auto connection = QObject::connect(
            source_.get(), &LogData::loadingFinished, &loop, [&]( LoadingStatus value ) {
                status = value;
                received = true;
                loop.quit();
            } );
        QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
        source_->reload();
        if ( !received ) {
            timeout.start( kLoadTimeoutMs );
            loop.exec();
        }
        QObject::disconnect( connection );
        return received && status == LoadingStatus::Successful;
    }

    LogData& source() const { return *source_; }
    const QString& path() const { return path_; }

private:
    QTemporaryDir directory_;
    QString path_;
    std::unique_ptr<LogData> source_;
};

LiteralSearchResult search( const LogData& source, const QByteArray& bytes,
                            const QByteArray& pattern, qint64 chunkBytes,
                            LineNumber firstLine = LineNumber( 0 ), qint64 begin = 0,
                            qint64 end = -1, size_t maxLines = 65536,
                            const AtomicFlag& cancelled = AtomicFlag{} )
{
    LiteralSearchOptions options;
    options.chunkBytes = chunkBytes;
    options.maxMatchingLines = maxLines;
    if ( end < 0 ) end = bytes.size();
    return searchLiteralBytes( source, source.snapshot(), firstLine, OffsetInFile( begin ),
                               OffsetInFile( end ), pattern, cancelled, options );
}

struct ExpectedHit {
    uint64_t line;
    qint64 offset;
};

std::vector<ExpectedHit> oracleHits( const QByteArray& input, const QByteArray& pattern,
                                     uint64_t firstLine = 0 )
{
    std::vector<ExpectedHit> expected;
    const std::string source( input.constData(), static_cast<size_t>( input.size() ) );
    const std::string needle( pattern.constData(), static_cast<size_t>( pattern.size() ) );
    size_t begin = 0;
    uint64_t line = firstLine;
    while ( begin < source.size() ) {
        const auto lf = source.find( '\n', begin );
        const auto end = lf == std::string::npos ? source.size() : lf;
        const auto found = source.find( needle, begin );
        if ( found != std::string::npos && found < end )
            expected.push_back( { line, static_cast<qint64>( found ) } );
        if ( lf == std::string::npos ) break;
        begin = lf + 1;
        ++line;
    }
    return expected;
}

void requireHits( const LiteralSearchResult& result, const std::vector<ExpectedHit>& expected )
{
    REQUIRE( result.completion == SearchCompletion::Complete );
    REQUIRE( result.hits.size() == expected.size() );
    for ( size_t i = 0; i < expected.size(); ++i ) {
        CAPTURE( i );
        REQUIRE( result.hits[i].line.get() == expected[i].line );
        REQUIRE( result.hits[i].byteOffset.get() == expected[i].offset );
    }
}

bool waitForSearch( LogFilteredData& data )
{
    if ( data.searchCompletion() != SearchCompletion::Running ) return true;
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    poll.setInterval( 10 );
    timeout.setSingleShot( true );
    QObject::connect( &poll, &QTimer::timeout, &loop, [&] {
        if ( data.searchCompletion() != SearchCompletion::Running ) loop.quit();
    } );
    QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
    poll.start();
    timeout.start( kSearchTimeoutMs );
    loop.exec();
    return data.searchCompletion() != SearchCompletion::Running;
}
} // namespace

TEST_CASE( "literal byte search carries matches across tiny reads", "[literal][native]" )
{
    const QByteArray input( "needle-needle\nalpha needlex\nno match\n" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    for ( const qint64 chunk : { 1, 2, 3, 7 } ) {
        INFO( "chunkBytes=" << chunk );
        const auto result = search( file.source(), input, QByteArray( "needle" ), chunk );
        requireHits( result, oracleHits( input, QByteArray( "needle" ) ) );
    }
}

TEST_CASE( "literal byte search matches UTF-8 split at every byte boundary",
           "[literal][native]" )
{
    const QByteArray snow = QString::fromUtf8( "雪" ).toUtf8();
    const QByteArray input = QByteArray( "x" ) + snow + snow + "\n" + snow + "end";
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    for ( const qint64 chunk : { 1, 2, 3, 7 } ) {
        INFO( "chunkBytes=" << chunk );
        const auto result = search( file.source(), input, snow, chunk );
        requireHits( result, oracleHits( input, snow ) );
    }
}

TEST_CASE( "literal byte search rejects invalid UTF-8 in source and pattern",
           "[literal][native]" )
{
    QByteArray invalidSource( "ok\ninvalid:" );
    invalidSource.append( '\xff' );
    AttachedSource file;
    REQUIRE( file.attach( invalidSource ) );
    const auto badSource = search( file.source(), invalidSource, QByteArray( "ok" ), 2 );
    REQUIRE( badSource.completion == SearchCompletion::Failed );
    REQUIRE( badSource.hits.empty() );

    AttachedSource validFile;
    const QByteArray validInput( "valid utf8\n" );
    REQUIRE( validFile.attach( validInput ) );
    QByteArray invalidPattern;
    invalidPattern.append( '\xc0' ); // overlong UTF-8 prefix
    const auto badPattern = search( validFile.source(), validInput, invalidPattern, 1 );
    REQUIRE( badPattern.completion == SearchCompletion::Failed );
    REQUIRE( badPattern.hits.empty() );
}

TEST_CASE( "literal byte search handles CRLF, final unterminated line and empty input",
           "[literal][native]" )
{
    const QByteArray input( "first hit hit\r\nsecond\r\nfinal-hit" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    const auto result = search( file.source(), input, QByteArray( "hit" ), 3 );
    requireHits( result, { { 0, 6 }, { 2, 29 } } );
    REQUIRE( result.completedLines == 3 );

    AttachedSource empty;
    const QByteArray noBytes;
    REQUIRE( empty.attach( noBytes ) );
    const auto emptyResult = search( empty.source(), noBytes, QByteArray( "x" ), 1 );
    requireHits( emptyResult, {} );
    REQUIRE( emptyResult.completedLines == 0 );
}

TEST_CASE( "literal byte search honors selected byte and line ranges", "[literal][native]" )
{
    const QByteArray input( "skip\nfirst hit hit\nsecond hit\nignore" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    constexpr qint64 begin = 5;
    const auto result = search( file.source(), input, QByteArray( "hit" ), 2, LineNumber( 1 ),
                                begin, input.size() - 6 );
    requireHits( result, { { 1, 11 }, { 2, 26 } } );
    REQUIRE( result.completedLines == 2 );
}

TEST_CASE( "literal byte search marks truncated results partial", "[literal][native]" )
{
    const QByteArray input( "hit\nnone\nhit-again\n" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    const auto result = search( file.source(), input, QByteArray( "hit" ), 2,
                                LineNumber( 0 ), 0, -1, 1 );
    REQUIRE( result.completion == SearchCompletion::Partial );
    REQUIRE( result.hits.size() == 1 );
    REQUIRE( result.hits.front().line.get() == 0 );
    REQUIRE( result.hits.front().byteOffset.get() == 0 );
}

TEST_CASE( "literal byte search cancellation is observable through progress",
           "[literal][native]" )
{
    const QByteArray input( 512 * 1024, 'a' );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    AtomicFlag cancelled;
    LiteralSearchOptions options;
    options.chunkBytes = 7;
    size_t callbacks = 0;
    const auto result = searchLiteralBytes(
        file.source(), file.source().snapshot(), LineNumber( 0 ), OffsetInFile( 0 ),
        OffsetInFile( input.size() ), QByteArray( "missing" ), cancelled, options,
        [&]( qint64, size_t ) {
            ++callbacks;
            cancelled.set();
        } );
    REQUIRE( callbacks == 1 );
    REQUIRE( result.completion == SearchCompletion::Cancelled );
    REQUIRE( result.bytesProcessed > 0 );
    REQUIRE( result.bytesProcessed < input.size() );
}

TEST_CASE( "literal byte search rejects stale snapshots after file changes and reload",
           "[literal][native]" )
{
    const QByteArray input( "match\nkeep\n" );
    AttachedSource truncated;
    REQUIRE( truncated.attach( input ) );
    const auto oldTruncatedSnapshot = truncated.source().snapshot();
    QFile changedFile( truncated.path() );
    REQUIRE( changedFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( changedFile.write( "x" ) == 1 );
    REQUIRE( changedFile.flush() );
    changedFile.close();
    const auto truncatedResult = searchLiteralBytes(
        truncated.source(), oldTruncatedSnapshot, LineNumber( 0 ), OffsetInFile( 0 ),
        OffsetInFile( input.size() ), QByteArray( "match" ), AtomicFlag{} );
    REQUIRE( truncatedResult.completion == SearchCompletion::SourceChanged );
    REQUIRE( truncatedResult.hits.empty() );

    AttachedSource replaced;
    REQUIRE( replaced.attach( input ) );
    const auto oldReplacedSnapshot = replaced.source().snapshot();
    QFile replacement( replaced.path() );
    REQUIRE( replacement.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    const QByteArray replacementBytes( "other\nkeep\n" );
    REQUIRE( replacementBytes.size() == input.size() );
    REQUIRE( replacement.write( replacementBytes ) == replacementBytes.size() );
    REQUIRE( replacement.flush() );
    REQUIRE( replacement.setFileTime( QDateTime::currentDateTime().addSecs( 5 ),
                                      QFileDevice::FileModificationTime ) );
    replacement.close();
    const auto replacementResult = searchLiteralBytes(
        replaced.source(), oldReplacedSnapshot, LineNumber( 0 ), OffsetInFile( 0 ),
        OffsetInFile( input.size() ), QByteArray( "match" ), AtomicFlag{} );
    REQUIRE( replacementResult.completion == SearchCompletion::SourceChanged );
    REQUIRE( replacementResult.hits.empty() );

    AttachedSource reloaded;
    REQUIRE( reloaded.attach( input ) );
    const auto oldReloadSnapshot = reloaded.source().snapshot();
    REQUIRE( reloaded.write( input + "replacement\n" ) );
    REQUIRE( reloaded.reloadAndWait() );
    REQUIRE( reloaded.source().snapshot().generation != oldReloadSnapshot.generation );
    const auto reloadResult = searchLiteralBytes(
        reloaded.source(), oldReloadSnapshot, LineNumber( 0 ), OffsetInFile( 0 ),
        OffsetInFile( oldReloadSnapshot.size ), QByteArray( "match" ), AtomicFlag{} );
    REQUIRE( reloadResult.completion == SearchCompletion::SourceChanged );
    REQUIRE( reloadResult.hits.empty() );
}

TEST_CASE( "literal byte search agrees with a simple line oracle on randomized inputs",
           "[literal][native]" )
{
    std::mt19937 random( 0x4b4c4f47 );
    std::uniform_int_distribution<int> lineCount( 1, 40 );
    std::uniform_int_distribution<int> lineLength( 0, 48 );
    std::uniform_int_distribution<int> alphabet( 0, 4 );
    const std::string chars = "abcde";
    QByteArray input;
    const int rows = lineCount( random );
    for ( int i = 0; i < rows; ++i ) {
        const auto length = lineLength( random );
        for ( int j = 0; j < length; ++j ) input.append( chars[alphabet( random )] );
        if ( i + 1 < rows ) input.append( '\n' );
    }

    AttachedSource file;
    REQUIRE( file.attach( input ) );
    for ( const QByteArray pattern : { QByteArray( "a" ), QByteArray( "bc" ),
                                       QByteArray( "cde" ), QByteArray( "eeeea" ),
                                       QByteArray( "zz" ) } ) {
        const qint64 chunk = 1 + static_cast<qint64>( pattern.size() % 7 );
        const auto result = search( file.source(), input, pattern, chunk );
        requireHits( result, oracleHits( input, pattern ) );
    }
}

TEST_CASE( "filtered data retains regex search and rejects incompatible long-line regex",
           "[literal][native][filtered]" )
{
    const QByteArray input( "alpha hit42\nbeta hit7\n" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    auto filtered = file.source().getNewFilteredData();
    const RegularExpressionPattern regex( QStringLiteral( "hit[0-9]+" ), true, false, false,
                                          false );
    REQUIRE( filtered->searchError( regex ).isEmpty() );
    filtered->runSearch( regex );
    REQUIRE( waitForSearch( *filtered ) );
    REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
    REQUIRE( filtered->getNbMatches() == LinesCount( 2 ) );
    REQUIRE( filtered->getMatchingLineNumber( LineNumber( 0 ) ) == LineNumber( 0 ) );
    REQUIRE( filtered->getMatchingLineNumber( LineNumber( 1 ) ) == LineNumber( 1 ) );

    QByteArray longLine( 1024 * 1024 + 32, 'a' );
    longLine.replace( longLine.size() - 8, 8, "hit12345" );
    AttachedSource longFile;
    REQUIRE( longFile.attach( longLine ) );
    auto longFiltered = longFile.source().getNewFilteredData();
    REQUIRE( longFiltered->searchError( regex ).contains(
        QStringLiteral( "case-sensitive UTF-8 literal" ) ) );
    longFiltered->runSearch( regex );
    REQUIRE( waitForSearch( *longFiltered ) );
    REQUIRE( longFiltered->searchCompletion() == SearchCompletion::Failed );

    const RegularExpressionPattern oversized(
        QString( static_cast<int>( LiteralSearchOptions::MaxPatternBytes + 1 ),
                 QLatin1Char( 'x' ) ),
        true, false, false, true );
    REQUIRE( longFiltered->searchError( oversized ).contains( QStringLiteral( "1048576" ) ) );
    longFiltered->runSearch( oversized );
    REQUIRE( waitForSearch( *longFiltered ) );
    REQUIRE( longFiltered->searchCompletion() == SearchCompletion::Failed );
}

TEST_CASE( "a cleared long literal search cannot replace the newer generation",
           "[literal][native][filtered]" )
{
    QByteArray input( 8 * 1024 * 1024, 'a' );
    input.replace( 10, 9, "oldmarker" );
    input.replace( input.size() - 9, 9, "newmarker" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    auto filtered = file.source().getNewFilteredData();
    const RegularExpressionPattern oldPattern( QStringLiteral( "oldmarker" ), true, false, false,
                                               true );
    const RegularExpressionPattern newPattern( QStringLiteral( "newmarker" ), true, false, false,
                                               true );
    filtered->runSearch( oldPattern );
    filtered->clearSearch();
    filtered->runSearch( newPattern );
    REQUIRE( waitForSearch( *filtered ) );
    QEventLoop settleLoop;
    QTimer settleTimer;
    settleTimer.setSingleShot( true );
    QObject::connect( &settleTimer, &QTimer::timeout, &settleLoop, &QEventLoop::quit );
    settleTimer.start( 150 );
    settleLoop.exec();
    REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
    REQUIRE( filtered->getNbMatches() == LinesCount( 1 ) );
    REQUIRE( filtered->firstMatchByte( LineNumber( 0 ) ).has_value() );
    REQUIRE( filtered->firstMatchByte( LineNumber( 0 ) )->get() == input.size() - 9 );
}

TEST_CASE( "cached search follow replaces the unrelated worker accumulator",
           "[literal][native][filtered][cache]" )
{
    auto& config = Configuration::get();
    struct RestoreCacheSetting {
        Configuration& config;
        bool previous;
        ~RestoreCacheSetting() { config.setUseSearchResultsCache( previous ); }
    } restoreCache{ config, config.useSearchResultsCache() };
    config.setUseSearchResultsCache( true );

    AttachedSource file;
    REQUIRE( file.attach( QByteArray(
        "only-B\nonly-A\nonly-B\nonly-A\nonly-A\nneither\n" ) ) );
    auto filtered = file.source().getNewFilteredData();
    const RegularExpressionPattern patternB( QStringLiteral( "only-B" ), true, false,
                                             false, true );
    const RegularExpressionPattern patternA( QStringLiteral( "only-A" ), true, false,
                                             false, true );

    // Wait for the actual terminal signal, including cache's synchronous path;
    // a previous completion value alone cannot establish that updateSearch ran.
    const auto runAndWait = [&]( const std::function<void()>& start,
                                 LinesCount& reportedMatches ) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot( true );
        bool finished = false;
        const auto connection = QObject::connect(
            filtered.get(), &LogFilteredData::searchProgressed, &loop,
            [&]( LinesCount matches, int progress, LineNumber ) {
                if ( progress == 100 ) {
                    reportedMatches = matches;
                    finished = true;
                    loop.quit();
                }
            } );
        QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
        start();
        if ( !finished ) {
            timeout.start( kSearchTimeoutMs );
            loop.exec();
        }
        QObject::disconnect( connection );
        return finished;
    };

    LinesCount reportedMatches{ 0 };
    REQUIRE( runAndWait( [&] { filtered->runSearch( patternB ); }, reportedMatches ) );
    REQUIRE( reportedMatches == LinesCount( 2 ) );
    REQUIRE( runAndWait( [&] { filtered->runSearch( patternA ); }, reportedMatches ) );
    REQUIRE( reportedMatches == LinesCount( 3 ) );
    REQUIRE( runAndWait( [&] { filtered->runSearch( patternB ); }, reportedMatches ) );
    REQUIRE( reportedMatches == LinesCount( 2 ) );

    // B was restored from cache while the worker still holds A's three hits.
    // An update over the same range must rebuild B rather than reuse A's state.
    REQUIRE( runAndWait( [&] {
        filtered->updateSearch( LineNumber( 0 ), LineNumber( file.source().getNbLine().get() ) );
    }, reportedMatches ) );
    REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
    REQUIRE( reportedMatches == LinesCount( 2 ) );
    REQUIRE( filtered->getNbMatches() == LinesCount( 2 ) );
    REQUIRE( filtered->getMatchingLineNumber( LineNumber( 0 ) ) == LineNumber( 0 ) );
    REQUIRE( filtered->getMatchingLineNumber( LineNumber( 1 ) ) == LineNumber( 2 ) );
    REQUIRE( filtered->previewSource() == &file.source() );
    REQUIRE( filtered->previewSourceLine( LineNumber( 1 ) ) == LineNumber( 2 ) );
}

TEST_CASE( "search batch planning obeys physical bytes and stale source snapshots",
           "[literal][native][batch]" )
{
    AttachedSource file;
    const QByteArray input( "1234\n5678\nlast" );
    REQUIRE( file.attach( input ) );
    const auto snapshot = file.source().snapshot();
    const auto first = file.source().planSearchBatch(
        LineNumber( 0 ), LinesCount( 3 ), 11, snapshot );
    REQUIRE( first.has_value() );
    REQUIRE( first->lines == LinesCount( 2 ) );
    REQUIRE( first->rawBytes == 10 );
    const auto tail = file.source().planSearchBatch(
        LineNumber( 2 ), LinesCount( 3 ), 11, snapshot );
    REQUIRE( tail.has_value() );
    REQUIRE( tail->lines == LinesCount( 1 ) );
    REQUIRE( tail->rawBytes == 4 ); // The synthetic final LF is not a file byte.
    const auto tooSmall = file.source().planSearchBatch(
        LineNumber( 0 ), LinesCount( 3 ), 4, snapshot );
    REQUIRE( tooSmall.has_value() );
    REQUIRE( tooSmall->lines == LinesCount( 0 ) );
    REQUIRE( tooSmall->rawBytes == 5 );
    REQUIRE( file.write( input + "changed" ) );
    REQUIRE_FALSE( file.source().planSearchBatch(
        LineNumber( 0 ), LinesCount( 3 ), 11, snapshot ).has_value() );
}

TEST_CASE( "filtered literal search reports its real matching-line cap as partial",
           "[literal][native][filtered][limit][cache]" )
{
    auto& config = Configuration::get();
    struct RestoreCacheSetting {
        Configuration& config;
        bool previous;
        ~RestoreCacheSetting() { config.setUseSearchResultsCache( previous ); }
    } restore{ config, config.useSearchResultsCache() };
    config.setUseSearchResultsCache( true );

    QByteArray input( 1024 * 1024, 'a' );
    input.append( '\n' );
    constexpr int cap = 65536;
    for ( int i = 0; i < cap + 1; ++i ) input.append( "hit\n" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    auto filtered = file.source().getNewFilteredData();
    const RegularExpressionPattern pattern( QStringLiteral( "hit" ), true, false, false, true );

    // A repeated capped query must run again and retain partial, never restore
    // these incomplete rows through the synchronous complete-cache path.
    for ( int attempt = 0; attempt < 2; ++attempt ) {
        INFO( "attempt=" << attempt );
        filtered->runSearch( pattern );
        REQUIRE( filtered->searchCompletion() == SearchCompletion::Running );
        REQUIRE( waitForSearch( *filtered ) );
        REQUIRE( filtered->searchCompletion() == SearchCompletion::Partial );
        REQUIRE( filtered->getNbMatches() == LinesCount( cap ) );
        REQUIRE_FALSE( filtered->searchMessage().isEmpty() );
        for ( int i = 0; i < cap; ++i ) {
            const auto line = LineNumber( i + 1 );
            REQUIRE( filtered->getMatchingLineNumber( LineNumber( i ) ) == line );
            const auto anchor = filtered->firstMatchByte( line );
            REQUIRE( anchor.has_value() );
            REQUIRE( anchor->get() == 1024 * 1024 + 1 + qint64( i ) * 4 );
        }
        REQUIRE_FALSE( filtered->firstMatchByte( LineNumber( cap + 1 ) ).has_value() );
    }
}

TEST_CASE( "filtered long literal search preserves CR as line content",
           "[literal][native][filtered][crlf]" )
{
    QByteArray input( 1024 * 1024, 'a' );
    input.append( "\r\nhit\r\n" );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    auto filtered = file.source().getNewFilteredData();
    const RegularExpressionPattern pattern( QStringLiteral( "\r" ), true, false, false, true );
    REQUIRE( filtered->searchError( pattern ).isEmpty() );
    filtered->runSearch( pattern );
    REQUIRE( waitForSearch( *filtered ) );
    REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
    REQUIRE( filtered->getNbMatches() == LinesCount( 2 ) );
    REQUIRE( filtered->firstMatchByte( LineNumber( 0 ) ).has_value() );
    REQUIRE( filtered->firstMatchByte( LineNumber( 1 ) ).has_value() );
    REQUIRE( filtered->firstMatchByte( LineNumber( 0 ) )->get() == 1024 * 1024 );
    REQUIRE( filtered->firstMatchByte( LineNumber( 1 ) )->get() == 1024 * 1024 + 5 );
}

TEST_CASE( "ordinary search bounds subthreshold line batches including decoded buffers",
           "[literal][native][filtered][budget]" )
{
    auto& config = Configuration::get();
    struct RestoreSearchSettings {
        Configuration& config;
        bool parallel;
        int threads, lines;
        ~RestoreSearchSettings() {
            config.setUseParallelSearch( parallel );
            config.setSearchThreadPoolSize( threads );
            config.setSearchReadBufferSizeLines( lines );
        }
    } restore{ config, config.useParallelSearch(), config.searchThreadPoolSize(),
               config.searchReadBufferSizeLines() };
    config.setUseParallelSearch( true );
    config.setSearchThreadPoolSize( 4 );
    config.setSearchReadBufferSizeLines( 5000 );

    const QByteArray row = QByteArray( 900 * 1024, 'a' ) + "needle42\n";
    QByteArray input;
    constexpr int rows = 48;
    input.reserve( row.size() * rows );
    for ( int i = 0; i < rows; ++i ) input.append( row );
    AttachedSource file;
    REQUIRE( file.attach( input ) );
    REQUIRE( file.source().getMaxLength().get() < 1024 * 1024 );
    const auto plan = file.source().planSearchBatch(
        LineNumber( 0 ), LinesCount( 5000 ), LogData::MaxByteRead, file.source().snapshot() );
    REQUIRE( plan.has_value() );
    REQUIRE( plan->lines == LinesCount( 4 ) );
    REQUIRE( plan->rawBytes <= LogData::MaxByteRead );

    // Both the UTF-8 view and the allocating conversion/prefilter path must
    // preserve every row while accounting for active buffers before reading.
    for ( const bool prefilter : { false, true } ) {
        INFO( "prefilter=" << prefilter );
        file.source().setPrefilter( prefilter ? QStringLiteral( "never-present" ) : QString{} );
        auto filtered = file.source().getNewFilteredData();
        filtered->runSearch( RegularExpressionPattern(
            QStringLiteral( "needle[0-9]+" ), true, false, false, false ) );
        REQUIRE( waitForSearch( *filtered ) );
        REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
        REQUIRE( filtered->getNbMatches() == LinesCount( rows ) );
        REQUIRE( filtered->peakSearchBufferBytes() > 0 );
        REQUIRE( filtered->peakSearchBufferBytes() <= LogFilteredDataWorker::PipelineBudgetBytes );
        for ( int i = 0; i < rows; ++i )
            REQUIRE( filtered->getMatchingLineNumber( LineNumber( i ) ) == LineNumber( i ) );
    }
}

namespace {
void requireOrdinaryFollowAfterAppend( const QByteArray& suffix, LinesCount expectedMatches )
{
    // Reload is explicit here; a watcher must not start another source check.
    REQUIRE_FALSE( Configuration::get().nativeFileWatchEnabled() );
    REQUIRE_FALSE( Configuration::get().pollingEnabled() );
    AttachedSource file;
    REQUIRE( file.attach( QByteArray( "hit1\nhit42" ) ) );
    auto filtered = file.source().getNewFilteredData();
    const RegularExpressionPattern pattern( QStringLiteral( "hit[0-9]+$" ), true, false,
                                            false, false );
    LinesCount reportedMatches{ 0 };
    const auto runAndWait = [&]( const std::function<void()>& start ) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot( true );
        bool finished = false;
        const auto connection = QObject::connect(
            filtered.get(), &LogFilteredData::searchProgressed, &loop,
            [&]( LinesCount matches, int progress, LineNumber ) {
                if ( progress == 100 ) {
                    reportedMatches = matches;
                    finished = true;
                    loop.quit();
                }
            } );
        QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
        start();
        if ( !finished ) {
            timeout.start( kSearchTimeoutMs );
            loop.exec();
        }
        QObject::disconnect( connection );
        return finished;
    };

    REQUIRE( runAndWait( [&] { filtered->runSearch( pattern ); } ) );
    REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
    REQUIRE( filtered->getNbMatches() == LinesCount( 2 ) );
    REQUIRE( reportedMatches == LinesCount( 2 ) );
    {
        QFile appended( file.path() );
        REQUIRE( appended.open( QIODevice::WriteOnly | QIODevice::Append ) );
        REQUIRE( appended.write( suffix ) == suffix.size() );
        REQUIRE( appended.flush() );
    }

    // This verifies actual file append + explicit reload/updateSearch. It does
    // not exercise watcher delivery or CrawlerWidget's automatic follow UI.
    REQUIRE( file.reloadAndWait() );
    REQUIRE( runAndWait( [&] {
        filtered->updateSearch( LineNumber( 0 ), LineNumber( file.source().getNbLine().get() ) );
    } ) );
    REQUIRE( filtered->searchCompletion() == SearchCompletion::Complete );
    REQUIRE( filtered->getNbMatches() == expectedMatches );
    REQUIRE( reportedMatches == expectedMatches );
    REQUIRE( filtered->getMatchingLineNumber( LineNumber( 0 ) ) == LineNumber( 0 ) );
    if ( expectedMatches == LinesCount( 2 ) )
        REQUIRE( filtered->getMatchingLineNumber( LineNumber( 1 ) ) == LineNumber( 1 ) );
    REQUIRE( filtered->lineTypeByLine( LineNumber( 1 ) ).testFlag(
        AbstractLogData::LineTypeFlags::Match ) == ( expectedMatches == LinesCount( 2 ) ) );
}
} // namespace

TEST_CASE( "ordinary follow removes an unterminated last line that no longer matches",
           "[literal][native][filtered][follow]" )
{
    requireOrdinaryFollowAfterAppend( QByteArray( "x" ), LinesCount( 1 ) );
}

TEST_CASE( "ordinary follow counts a still-matching unterminated last line once",
           "[literal][native][filtered][follow]" )
{
    requireOrdinaryFollowAfterAppend( QByteArray( "7" ), LinesCount( 2 ) );
}

int main( int argc, char* argv[] )
{
    QApplication application( argc, argv );
    logging::enableLogging();
    auto& config = Configuration::getSynced();
    config.setDefaultEncodingMib( 106 ); // UTF-8
    config.setNativeFileWatchEnabled( false );
    config.setPollingEnabled( false );
    config.setUseSearchResultsCache( false );
    return Catch::Session().run( argc, argv );
}
