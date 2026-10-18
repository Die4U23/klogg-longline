/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <qsemaphore.h>
#include <utility>

#include <robin_hood.h>
#include <tbb/flow_graph.h>
#include <vector>

#include "configuration.h"
#include "dispatch_to.h"
#include "issuereporter.h"
#include "linetypes.h"
#include "log.h"
#include "progress.h"
#include "runnable_lambda.h"

#include "logdata.h"
#include "regularexpression.h"

#include "logfiltereddataworker.h"
#include "synchronization.h"

namespace {
// This covers active raw/decoded buffers, offsets, views and per-block results.
// The existing line index, retained hit sets and regex-engine scratch are separate.
struct SearchByteBudget {
    bool acquire( size_t bytes, const AtomicFlag& cancelled, const AtomicFlag& stopped )
    {
        if ( bytes > LogFilteredDataWorker::PipelineBudgetBytes )
            throw std::runtime_error( "Search block exceeds the active byte budget" );
        std::unique_lock<std::mutex> lock( mutex );
        while ( bytes > LogFilteredDataWorker::PipelineBudgetBytes - used ) {
            if ( cancelled || stopped ) return false;
            changed.wait_for( lock, std::chrono::milliseconds( 10 ) );
        }
        if ( cancelled || stopped ) return false;
        used += bytes;
        peak = std::max( peak, used );
        return true;
    }

    void release( size_t bytes )
    {
        { std::lock_guard<std::mutex> lock( mutex ); used -= bytes; }
        changed.notify_one();
    }

    std::mutex mutex;
    std::condition_variable changed;
    size_t used = 0, peak = 0;
};

struct SearchByteLease {
    SearchByteLease() = default;
    SearchByteLease( std::shared_ptr<SearchByteBudget> owner, size_t amount )
        : budget( std::move( owner ) ), bytes( amount ) {}
    SearchByteLease( const SearchByteLease& ) = delete;
    SearchByteLease& operator=( const SearchByteLease& ) = delete;
    SearchByteLease( SearchByteLease&& other ) noexcept
        : budget( std::move( other.budget ) ), bytes( other.bytes ) {}
    ~SearchByteLease() { if ( budget ) budget->release( bytes ); }

    std::shared_ptr<SearchByteBudget> budget;
    size_t bytes = 0;
};

struct PartialSearchResults {
    PartialSearchResults() = default;

    PartialSearchResults( const PartialSearchResults& ) = delete;
    PartialSearchResults( PartialSearchResults&& ) = default;
    PartialSearchResults& operator=( const PartialSearchResults& ) = delete;
    PartialSearchResults& operator=( PartialSearchResults&& ) = default;

    SearchResultArray matchingLines;
    LineLength maxLength;

    LineNumber chunkStart;
    LinesCount processedLines;
};

struct SearchBlockData {
    SearchBlockData() = default;
    SearchBlockData( LineNumber start, LogData::RawLines blockLines, SearchByteLease lease )
        : byteLease( std::move( lease ) )
        , chunkStart( start )
        , lines( std::move( blockLines ) )
    {
    }

    SearchBlockData( const SearchBlockData& ) = delete;
    SearchBlockData( SearchBlockData&& ) = default;

    SearchBlockData& operator=( const SearchBlockData& ) = delete;
    SearchBlockData& operator=( SearchBlockData&& ) = default;

    // Declared first so its reservation is returned after all buffers/results.
    SearchByteLease byteLease;
    LineNumber chunkStart;
    LogData::RawLines lines;

    PartialSearchResults searchResults;
};

PartialSearchResults filterLines( const PatternMatcher& matcher, const LogData::RawLines& rawLines,
                                  LineNumber chunkStart )
{
    LOG_DEBUG << "Filter lines at " << chunkStart;
    PartialSearchResults results;
    results.chunkStart = chunkStart;
    results.processedLines = LinesCount{ rawLines.endOfLines.size() };

    const auto& lines = rawLines.buildUtf8View();

    for ( auto offset = 0u; offset < lines.size(); ++offset ) {
        const auto& line = lines[ offset ];

        const auto hasMatch = matcher.hasMatch( line );

        if ( hasMatch ) {
            results.maxLength = qMax( results.maxLength, getUntabifiedLength( line ) );
            const auto lineNumber = chunkStart + LinesCount{ offset };
            results.matchingLines.add( lineNumber.get() );

            // LOG_INFO << "Match at " << lineNumber << ": " << line;
        }
    }
    return results;
}

} // namespace

SearchResults SearchData::takeCurrentResults() const
{
    UniqueLock lock( dataMutex_ );
    return SearchResults{ std::exchange( newMatches_, {} ), maxLength_, nbLinesProcessed_,
                          completion_, message_, std::exchange(firstHits_, {}), peakBufferBytes_ };
}

void SearchData::addAll( LineLength length, const SearchResultArray& matches,
                         LinesCount matchedLines, LinesCount processedLines )
{
    UniqueLock lock( dataMutex_ );

    maxLength_ = qMax( maxLength_, length );
    nbLinesProcessed_ = qMax( nbLinesProcessed_, processedLines );
    nbMatches_ += matchedLines;

    newMatches_ |= matches;
}

LinesCount SearchData::getNbMatches() const
{
    SharedLock lock( dataMutex_ );
    return nbMatches_;
}

LineNumber SearchData::getLastProcessedLine() const
{
    SharedLock lock( dataMutex_ );
    return LineNumber{ nbLinesProcessed_.get() };
}

void SearchData::deleteMatch( LineNumber line )
{
    UniqueLock lock( dataMutex_ );
    matches_.remove( line.get() );
}

void SearchData::clear()
{
    UniqueLock locker( dataMutex_ );

    maxLength_ = LineLength( 0 );
    nbLinesProcessed_ = LinesCount( 0 );
    nbMatches_ = LinesCount( 0 );
    matches_ = {};
    newMatches_ = {};
    completion_ = SearchCompletion::Running;
    message_.clear();
    firstHits_.clear();
    peakBufferBytes_ = 0;
}

void SearchData::finish(SearchCompletion completion, const QString& message,
                        std::vector<LiteralHit> hits, size_t peakBufferBytes)
{
    UniqueLock lock(dataMutex_);
    completion_ = completion;
    message_ = message;
    firstHits_ = std::move(hits);
    peakBufferBytes_ = peakBufferBytes;
}

LogFilteredDataWorker::LogFilteredDataWorker( const LogData& sourceLogData )
    : sourceLogData_( sourceLogData )
{
    operationsPool_.setMaxThreadCount( 1 );
}

LogFilteredDataWorker::~LogFilteredDataWorker() noexcept
{
    try {
        interruptRequested_.set();
        ScopedLock locker( operationsMutex_ );
        operationsPool_.waitForDone();
        LOG_INFO << "LogFilteredDataWorker shutdown";
    } catch ( const std::exception& e ) {
        LOG_ERROR << "Failed to destroy LogFilteredDataWorker: " << e.what();
    }
}

void LogFilteredDataWorker::connectSignalsAndRun( SearchOperation* operationRequested )
{
    connect( operationRequested, &SearchOperation::searchProgressed, this,
             &LogFilteredDataWorker::searchProgressed );
    connect( operationRequested, &SearchOperation::searchFinished, this,
             &LogFilteredDataWorker::searchFinished, Qt::QueuedConnection );

    operationRequested->run( searchData_ );
    operationRequested->disconnect( this );
}

void LogFilteredDataWorker::search( const RegularExpressionPattern& regExp, LineNumber startLine,
                                    LineNumber endLine, quint64 generation )
{
    ScopedLock locker( operationsMutex_ ); // to protect operationRequested_
    operationsPool_.waitForDone();
    interruptRequested_.clear();

    LOG_INFO << "Search requested";
    QSemaphore operationStarted;
    operationsPool_.start( createRunnable( [ this, &operationStarted, regExp, startLine, endLine, generation ] {
        operationStarted.release();
        auto operationRequested = std::make_unique<FullSearchOperation>(
            sourceLogData_, interruptRequested_, regExp, startLine, endLine, generation );
        connectSignalsAndRun( operationRequested.get() );
    } ) );
    operationStarted.acquire();
}

void LogFilteredDataWorker::updateSearch( const RegularExpressionPattern& regExp,
                                          LineNumber startLine, LineNumber endLine,
                                          LineNumber position, quint64 generation )
{
    ScopedLock locker( operationsMutex_ ); // to protect operationRequested_
    operationsPool_.waitForDone();
    interruptRequested_.clear();

    LOG_INFO << "Search update requested from " << position.get();

    QSemaphore operationStarted;
    operationsPool_.start(
        createRunnable( [ this, &operationStarted, regExp, startLine, endLine, position, generation ] {
            operationStarted.release();
            auto operationRequested = std::make_unique<UpdateSearchOperation>(
                sourceLogData_, interruptRequested_, regExp, startLine, endLine, position, generation );
            connectSignalsAndRun( operationRequested.get() );
        } ) );

    operationStarted.acquire();
}

void LogFilteredDataWorker::interrupt()
{
    LOG_INFO << "Search interruption requested";
    interruptRequested_.set();
}

// This will do an atomic copy of the object
SearchResults LogFilteredDataWorker::getSearchResults() const
{
    return searchData_.takeCurrentResults();
}

//
// Operations implementation
//

SearchOperation::SearchOperation( const LogData& sourceLogData, AtomicFlag& interruptRequested,
                                  const RegularExpressionPattern& regExp, LineNumber startLine,
                                  LineNumber endLine, quint64 generation )

    : interruptRequested_( interruptRequested )
    , regexp_( regExp )
    , sourceLogData_( sourceLogData )
    , startLine_( startLine )
    , endLine_( endLine )
    , generation_( generation )

{
}

void SearchOperation::doSearch( SearchData& searchData, LineNumber initialLine )
{
    const auto nbSourceLines = sourceLogData_.getNbLine();
    const auto sourceSnapshot = sourceLogData_.snapshot();
    initialLine = qMax(initialLine, startLine_);
    const auto selectedEnd = qMin(LineNumber(nbSourceLines.get()), endLine_);

    // Preserve the existing engine for ordinary logs. A long logical line may
    // not enter a line-count-batched reader or a non-cancellable regex call.
    if (sourceLogData_.getMaxLength().get() >= 1024 * 1024) {
        auto finish = [&](SearchCompletion completion, const QString& message) {
            searchData.finish(completion, message);
            Q_EMIT searchProgressed(searchData.getNbMatches(), 100, initialLine, generation_);
            Q_EMIT searchFinished();
        };
        const auto compatibilityError = longLineSearchError(sourceLogData_, regexp_);
        if (!compatibilityError.isEmpty()) {
            finish(SearchCompletion::Failed, compatibilityError);
            return;
        }
        if (initialLine >= selectedEnd) {
            finish(SearchCompletion::Complete, {});
            return;
        }
        const auto firstRange = sourceLogData_.lineByteRange(initialLine, sourceSnapshot);
        const auto lastRange = sourceLogData_.lineByteRange(selectedEnd - 1_lcount, sourceSnapshot);
        if (!firstRange || !lastRange) {
            finish(SearchCompletion::SourceChanged, QStringLiteral("Source index is no longer current."));
            return;
        }
        const auto bytesToSearch = lastRange->end.get() - firstRange->begin.get();
        int reportedPercent = -1;
        auto result = searchLiteralBytes(sourceLogData_, sourceSnapshot, initialLine,
            firstRange->begin, lastRange->end, regexp_.pattern.toUtf8(), interruptRequested_, {},
            [&](qint64 bytes, size_t count) {
                const int percent = bytesToSearch ? static_cast<int>(100.0 * static_cast<double>(bytes)
                                                          / static_cast<double>(bytesToSearch)) : 100;
                if (percent != reportedPercent) {
                    reportedPercent = percent;
                    Q_EMIT searchProgressed(LinesCount(count), std::min(99, percent), initialLine, generation_);
                }
            });
        if (!sourceLogData_.matchesSnapshot(sourceSnapshot)) {
            result.completion = SearchCompletion::SourceChanged;
            result.message = QStringLiteral("Source changed during search.");
            result.hits.clear();
        }
        SearchResultArray matchingLines;
        for (const auto& hit : result.hits) matchingLines.add(hit.line.get());
        searchData.addAll(LineLength(AbstractLogData::PreviewByteLimit), matchingLines,
            LinesCount(result.hits.size()), LinesCount(initialLine.get() + result.completedLines));
        searchData.finish(result.completion, result.message, std::move(result.hits), result.peakBufferBytes);
        Q_EMIT searchProgressed(searchData.getNbMatches(), 100, initialLine, generation_);
        Q_EMIT searchFinished();
        return;
    }
    if (initialLine >= selectedEnd) {
        searchData.finish(SearchCompletion::Complete);
        Q_EMIT searchProgressed(searchData.getNbMatches(), 100, initialLine, generation_);
        Q_EMIT searchFinished();
        return;
    }

    LOG_INFO << "Searching from line " << initialLine << " to " << nbSourceLines;

    using namespace std::chrono;
    high_resolution_clock::time_point t1 = high_resolution_clock::now();

    const auto& config = Configuration::get();
    const auto matchingThreadsCount = static_cast<uint32_t>( [ &config ]() {
        if ( !config.useParallelSearch() ) {
            return 1;
        }
        const auto configuredThreadPoolSize = config.searchThreadPoolSize();
        return qMax( 1, configuredThreadPoolSize == 0 ? tbb::info::default_concurrency()
                                                      : configuredThreadPoolSize );
    }() );

    LOG_INFO << "Using " << matchingThreadsCount << " matching threads";

    tbb::flow::graph searchGraph;

    if ( initialLine < startLine_ ) {
        initialLine = startLine_;
    }

    const auto endLine = qMin( LineNumber( nbSourceLines.get() ), endLine_ );
    const auto nbLinesInChunk = LinesCount( static_cast<LinesCount::UnderlyingType>(
        std::clamp( config.searchReadBufferSizeLines(), 1, 65536 ) ) );
    const auto byteBudget = std::make_shared<SearchByteBudget>();
    AtomicFlag pipelineStopped;
    std::mutex nodeErrorMutex;
    std::exception_ptr nodeError;
    const auto recordNodeError = [&] {
        {
            std::lock_guard<std::mutex> lock( nodeErrorMutex );
            if ( !nodeError ) nodeError = std::current_exception();
        }
        // Keep graph nodes draining rather than cancelling the graph: queued
        // blocks own reservations needed by the producer to finish its wait.
        pipelineStopped.set();
        byteBudget->changed.notify_all();
    };
    const bool needsDecode = sourceLogData_.hasPrefilter()
        || !EncodingParameters( sourceLogData_.getDisplayEncoding() ).isUtf8Compatible;

    std::chrono::microseconds fileReadingDuration{ 0 };

    using BlockDataType = std::shared_ptr<SearchBlockData>;
    auto blockPrefetcher
        = tbb::flow::limiter_node<BlockDataType>( searchGraph, matchingThreadsCount * 3 );

    auto lineBlocksQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    using RegexMatcherNode
        = tbb::flow::function_node<BlockDataType, BlockDataType, tbb::flow::rejecting>;

    using PatternMatcherPtr = std::unique_ptr<PatternMatcher>;
    using MatcherContext = std::tuple<PatternMatcherPtr, microseconds, RegexMatcherNode>;

    klogg::vector<MatcherContext> regexMatchers;
    regexMatchers.reserve( matchingThreadsCount );
    RegularExpression regularExpression{ regexp_ };
    for ( auto index = 0u; index < matchingThreadsCount; ++index ) {
        regexMatchers.emplace_back(
            regularExpression.createMatcher(), microseconds{ 0 },
            RegexMatcherNode(
                searchGraph, 1, [ &regexMatchers, &pipelineStopped, &recordNodeError,
                                  index, this ]( const BlockDataType& blockData ) {
                    try {
                        if ( interruptRequested_ || pipelineStopped ) return blockData;

                        const auto& matcher = std::get<PatternMatcherPtr>( regexMatchers.at( index ) );
                        const auto matchStartTime = high_resolution_clock::now();

                        blockData->searchResults
                            = filterLines( *matcher, blockData->lines, blockData->chunkStart );

                        const auto matchEndTime = high_resolution_clock::now();

                        microseconds& matchDuration
                            = std::get<microseconds>( regexMatchers.at( index ) );
                        matchDuration += duration_cast<microseconds>( matchEndTime - matchStartTime );
                        LOG_DEBUG << "Searcher " << index << " block " << blockData->chunkStart
                                  << " sending matches "
                                  << blockData->searchResults.matchingLines.cardinality();
                    } catch ( ... ) {
                        recordNodeError();
                    }
                    return blockData;
                } ) );
    }

    auto resultsQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    const auto totalLines = endLine - initialLine;
    LinesCount totalProcessedLines = 0_lcount;
    LineLength maxLength = 0_length;
    LinesCount nbMatches = searchData.getNbMatches();
    auto reportedMatches = nbMatches;
    int reportedPercentage = 0;

    std::chrono::microseconds matchCombiningDuration{ 0 };

    auto matchProcessor
        = tbb::flow::function_node<BlockDataType, tbb::flow::continue_msg, tbb::flow::rejecting>(
            searchGraph, 1, [ & ]( const BlockDataType& blockData ) {
                try {
                    if ( interruptRequested_ || pipelineStopped ) return tbb::flow::continue_msg{};

                    const auto& matchResults = blockData->searchResults;

                    const auto matchProcessorStartTime = high_resolution_clock::now();

                    if ( matchResults.processedLines.get() ) {

                        maxLength = qMax( maxLength, matchResults.maxLength );
                        const LinesCount matchesCount
                            = LinesCount( matchResults.matchingLines.cardinality() );
                        nbMatches += matchesCount;

                        const auto processedLines = LinesCount{ matchResults.chunkStart.get()
                                                                + matchResults.processedLines.get() };

                        totalProcessedLines += matchResults.processedLines;

                        // After each block, copy the data to shared data
                        // and update the client
                        searchData.addAll( maxLength, matchResults.matchingLines, matchesCount,
                                           processedLines );

                        LOG_DEBUG << "done Searching chunk starting at " << matchResults.chunkStart
                                  << ", " << matchResults.processedLines << " lines read.";
                    }

                    const auto matchProcessorEndTime = high_resolution_clock::now();
                    matchCombiningDuration += duration_cast<microseconds>( matchProcessorEndTime
                                                                           - matchProcessorStartTime );
                    const int percentage
                        = calculateProgress( totalProcessedLines.get(), totalLines.get() );

                    if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {

                        Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine, generation_ );

                        reportedPercentage = percentage;
                        reportedMatches = nbMatches;
                    }
                } catch ( ... ) {
                    recordNodeError();
                }

                return tbb::flow::continue_msg{};
            } );

    tbb::flow::make_edge( blockPrefetcher, lineBlocksQueue );

    for ( auto& regexMatcher : regexMatchers ) {
        tbb::flow::make_edge( lineBlocksQueue, std::get<RegexMatcherNode>( regexMatcher ) );
        tbb::flow::make_edge( std::get<RegexMatcherNode>( regexMatcher ), resultsQueue );
    }

    tbb::flow::make_edge( resultsQueue, matchProcessor );
    tbb::flow::make_edge( matchProcessor, blockPrefetcher.decrementer() );

    auto chunkStart = initialLine;
    std::exception_ptr readError;
    while ( chunkStart < endLine && !interruptRequested_ && !pipelineStopped ) {
        const auto lineSourceStartTime = high_resolution_clock::now();
        LOG_DEBUG << "Reading chunk starting at " << chunkStart;

        try {
            const auto plan = sourceLogData_.planSearchBatch( chunkStart,
                LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) ),
                LogData::MaxByteRead, sourceSnapshot );
            if ( !plan )
                throw std::runtime_error( "Source index changed while planning a search block" );
            if ( plan->lines.get() == 0 )
                throw std::runtime_error( "A source line exceeds the bounded legacy search block" );
            // UTF-8 path: raw R + up to 2R for Qt matcher's per-line UTF-16.
            // Decode path: raw R + UTF-16 2R + existing UTF-8 temporary 4R,
            // plus 2R for QString detach/replacement. Two offset arrays and the
            // string_view array fit 32*N; <=65536 consecutive line numbers touch
            // at most two Roaring containers, covered by 64 KiB per-block overhead.
            const auto reservation = static_cast<size_t>( plan->rawBytes ) * ( needsDecode ? 9 : 3 )
                + static_cast<size_t>( plan->lines.get() ) * 32 + 64 * 1024;
            if ( !byteBudget->acquire( reservation, interruptRequested_, pipelineStopped ) ) break;
            SearchByteLease lease( byteBudget, reservation );
            auto lines = sourceLogData_.getLinesRaw( chunkStart, plan->lines );
            if ( lines.endOfLines.size() != plan->lines.get()
                 || lines.buffer.size() != static_cast<size_t>( plan->rawBytes )
                 || !sourceLogData_.matchesSnapshot( sourceSnapshot ) )
                throw std::runtime_error( "Source changed or a search block could not be read" );

            auto blockData = std::make_shared<SearchBlockData>(
                chunkStart, std::move(lines), std::move(lease));
            const auto lineSourceEndTime = high_resolution_clock::now();
            fileReadingDuration += duration_cast<microseconds>( lineSourceEndTime - lineSourceStartTime );
            chunkStart = chunkStart + plan->lines;

            while ( !interruptRequested_ && !pipelineStopped
                    && !blockPrefetcher.try_put( blockData ) ) {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        } catch ( ... ) {
            readError = std::current_exception();
            break;
        }
    }

    searchGraph.wait_for_all();
    if ( nodeError ) std::rethrow_exception( nodeError );
    if ( readError && sourceLogData_.matchesSnapshot( sourceSnapshot ) )
        std::rethrow_exception( readError );

    high_resolution_clock::time_point t2 = high_resolution_clock::now();
    const auto durationUs = duration_cast<microseconds>( t2 - t1 );
    const auto durationMs = duration_cast<milliseconds>( t2 - t1 );

    LOG_INFO << "Searching done, overall duration " << durationUs;
    LOG_INFO << "Line reading took " << fileReadingDuration;
    LOG_INFO << "Results combining took " << matchCombiningDuration;

    for ( const auto& regexMatcher : regexMatchers ) {
        LOG_INFO << "Matching took " << std::get<microseconds>( regexMatcher );
    }

    const auto totalFileSize = sourceLogData_.getFileSize();

    LOG_INFO << "Searching perf "
             << static_cast<uint64_t>(
                    std::floor( 1000.f * static_cast<float>( ( endLine - initialLine ).get() )
                                / static_cast<float>( durationMs.count() ) ) )
             << " lines/s";
    LOG_INFO << "Searching io perf "
             << ( 1000.f * static_cast<float>( totalFileSize )
                  / static_cast<float>( durationMs.count() ) )
                    / ( 1024 * 1024 )
             << " MiB/s";

    if (!sourceLogData_.matchesSnapshot(sourceSnapshot)) {
        searchData.clear();
        nbMatches = 0_lcount;
        searchData.finish(SearchCompletion::SourceChanged, QStringLiteral("Source changed during search."),
                          {}, byteBudget->peak);
    }
    else {
        searchData.finish(interruptRequested_ ? SearchCompletion::Cancelled : SearchCompletion::Complete,
            interruptRequested_ ? QStringLiteral("Search cancelled; results are incomplete.") : QString{},
            {}, byteBudget->peak);
    }
    Q_EMIT searchProgressed( nbMatches, 100, initialLine, generation_ );
    Q_EMIT searchFinished();
}

// Called in the worker thread's context
void FullSearchOperation::run( SearchData& searchData )
{
    try {
        // Clear the shared data
        searchData.clear();
        doSearch( searchData, 0_lnum );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "FullSearchOperation failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        searchData.clear();
        searchData.finish(SearchCompletion::Failed, errorString);
        Q_EMIT searchProgressed(0_lcount, 100, startLine_, generation_);
        Q_EMIT searchFinished();
    }
}

// Called in the worker thread's context
void UpdateSearchOperation::run( SearchData& searchData )
{
    try {
        auto initialLine = qMax( searchData.getLastProcessedLine(), initialPosition_ );

        if ( initialLine.get() >= 1 ) {
            // We need to re-search the last line because it might have
            // been updated (if it was not LF-terminated)
            --initialLine;
            // In case the last line matched, we don't want it to match twice.
            searchData.deleteMatch( initialLine );
        }

        doSearch( searchData, initialLine );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "UpdateSearchOpertaion failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        searchData.clear();
        searchData.finish(SearchCompletion::Failed, errorString);
        Q_EMIT searchProgressed(0_lcount, 100, startLine_, generation_);
        Q_EMIT searchFinished();
    }
}
