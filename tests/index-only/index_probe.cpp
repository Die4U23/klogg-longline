#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "compressedlinestorage.h"

namespace {
constexpr size_t kCount = 128;

QString jsonString(uint64_t value)
{
    return QString::number(static_cast<qulonglong>(value));
}

QJsonObject measure(const QString& name, uint64_t base, uint64_t span)
{
    QJsonObject result;
    result.insert("case", name);
    QJsonArray expectedValues;
    QJsonArray atValues;
    QJsonArray rangeValues;
    QElapsedTimer timer;
    timer.start();
    try {
        CompressedLinePositionStorage positions;
        for (size_t i = 0; i < kCount; ++i) {
            const auto value = base + span * i / (kCount - 1);
            expectedValues.append(jsonString(value));
            positions.append(OffsetInFile(static_cast<int64_t>(value)));
        }
        for (size_t i = 0; i < kCount; ++i)
            atValues.append(jsonString(static_cast<uint64_t>(positions.at(i).get())));
        const auto range = positions.range(LineNumber(0), LinesCount(kCount));
        for (const auto value : range)
            rangeValues.append(jsonString(static_cast<uint64_t>(value.get())));
        result.insert("allocated_size_bytes", static_cast<qint64>(positions.allocatedSize()));
        result.insert("status", "measured");
        result.insert("matches_expected", atValues == expectedValues && rangeValues == expectedValues);
        result.insert("expected_values", expectedValues);
        result.insert("at_values", atValues);
        result.insert("range_values", rangeValues);
    } catch (const std::exception& error) {
        result.insert("status", "exception");
        result.insert("reason", error.what());
    }
    result.insert("elapsed_ns", timer.nsecsElapsed());
    return result;
}

QJsonObject lifecyclePopRecompress()
{
    QElapsedTimer timer;
    timer.start();
    CompressedLinePositionStorage positions;
    for (int64_t i = 0; i < 128; ++i) positions.append(OffsetInFile(100 + i));
    const auto sizeBeforePop = positions.allocatedSize();
    positions.pop_back();
    const auto sizeAfterPop = positions.allocatedSize();
    positions.append(OffsetInFile(227));
    const auto range = positions.range(LineNumber(0), LinesCount(128));
    bool matches = range.size() == 128;
    for (size_t i = 0; matches && i < 128; ++i)
        matches = range[i].get() == 100 + static_cast<int64_t>(i)
                  && positions.at(i).get() == 100 + static_cast<int64_t>(i);
    return {{"case", "lifecycle-pop-recompress"}, {"status", "measured"},
            {"matches_expected", matches}, {"allocated_size_before_pop", static_cast<qint64>(sizeBeforePop)},
            {"allocated_size_after_pop", static_cast<qint64>(sizeAfterPop)},
            {"allocated_size_after_recompress", static_cast<qint64>(positions.allocatedSize())},
            {"elapsed_ns", timer.nsecsElapsed()}};
}

QJsonObject lifecycleMoveAppend()
{
    QElapsedTimer timer;
    timer.start();
    CompressedLinePositionStorage original;
    for (int64_t i = 0; i < 128; ++i) original.append(OffsetInFile(100 + i));
    CompressedLinePositionStorage moved(std::move(original));
    for (uint64_t i = 0; i < 128; ++i)
        moved.append(OffsetInFile(static_cast<int64_t>(1000 + uint64_t{UINT32_MAX} * i / 127)));
    const auto range = moved.range(LineNumber(0), LinesCount(256));
    bool matches = range.size() == 256;
    bool atMatches = true;
    QJsonArray expectedValues;
    QJsonArray actualValues;
    for (int64_t i = 0; i < 256 && matches; ++i) {
        const int64_t expected = i < 128
                                     ? 100 + i
                                     : static_cast<int64_t>(1000 + uint64_t{UINT32_MAX} * (i - 128) / 127);
        expectedValues.append(QString::number(expected));
        actualValues.append(QString::number(range[static_cast<size_t>(i)].get()));
        matches = range[static_cast<size_t>(i)].get() == expected;
        atMatches = atMatches && moved.at(static_cast<size_t>(i)).get() == expected;
    }
    return {{"case", "lifecycle-move-append"}, {"status", "measured"},
            {"matches_expected", matches}, {"at_matches_expected", atMatches},
            {"allocated_size_bytes", static_cast<qint64>(moved.allocatedSize())},
            {"expected_values", expectedValues}, {"range_values", actualValues},
            {"elapsed_ns", timer.nsecsElapsed()}};
}

QJsonObject rangeZero()
{
    CompressedLinePositionStorage positions;
    const auto range = positions.range(LineNumber(0), LinesCount(0));
    return {{"case", "range-zero"}, {"status", "measured"}, {"empty", range.empty()}};
}

QJsonObject lifecycleMixedStress()
{
    QElapsedTimer timer;
    timer.start();
    CompressedLinePositionStorage positions;
    std::vector<int64_t> expected;
    const auto appendBlock = [&]( int64_t base, uint64_t span ) {
        for ( size_t i = 0; i < kCount; ++i ) {
            const auto value = base + static_cast<int64_t>( span * i / ( kCount - 1 ) );
            positions.append( OffsetInFile( value ) );
            expected.push_back( value );
        }
    };
    appendBlock( 100, 127 );
    appendBlock( 1000, UINT32_MAX );
    appendBlock( 5000000000LL, 127 );

    bool matches = true;
    const auto range = positions.range( LineNumber( 120 ), LinesCount( 140 ) );
    for ( size_t i = 0; matches && i < range.size(); ++i )
        matches = range[i].get() == expected[120 + i];
    const auto wideToNarrow = positions.range( LineNumber( 240 ), LinesCount( 144 ) );
    for ( size_t i = 0; matches && i < wideToNarrow.size(); ++i )
        matches = wideToNarrow[i].get() == expected[240 + i];

    // Pop the narrow tail and then the wide block, then append a narrow block
    // again to exercise packed tail padding and wide-storage reclamation.
    for ( size_t i = 0; i < 256; ++i ) {
        positions.pop_back();
        expected.pop_back();
    }
    appendBlock( 5000000000LL, 127 );
    for ( size_t i = 0; matches && i < expected.size(); ++i )
        matches = positions.at( i ).get() == expected[i];

    // Move assignment over populated storage and reuse the moved-from object.
    CompressedLinePositionStorage destination;
    destination.append( OffsetInFile( 7 ) );
    destination = std::move( positions );
    positions.append( OffsetInFile( 1 ) );
    matches = matches && destination.size().get() == expected.size()
              && destination.at( 0 ).get() == expected.front()
              && positions.at( 0 ).get() == 1;
    destination = std::move( destination ); // self-move must preserve contents
    matches = matches && destination.size().get() == expected.size();

    // Repeated wide pop/reappend; all vector capacities should stabilize.
    const auto stableCapacity = destination.allocatedSize();
    bool stable = true;
    for ( size_t iteration = 0; iteration < 1000; ++iteration ) {
        destination.pop_back();
        destination.append( OffsetInFile( expected.back() + 1 ) );
        expected.back() += 1;
        stable = stable && destination.allocatedSize() == stableCapacity;
    }
    for ( size_t i = 0; matches && i < expected.size(); ++i )
        matches = destination.at( i ).get() == expected[i];
    return { { "case", "lifecycle-mixed-stress" }, { "status", "measured" },
             { "matches_expected", matches }, { "capacity_stable", stable },
             { "allocated_capacity_bytes", static_cast<qint64>( destination.allocatedSize() ) },
             { "iterations", 1000 }, { "elapsed_ns", timer.nsecsElapsed() } };
}

QJsonObject normalPerformance()
{
    constexpr uint64_t lineCount = 1000000;
    QElapsedTimer timer;
    CompressedLinePositionStorage positions;
    int64_t nextOffset = 0;
    timer.start();
    for ( uint64_t i = 0; i < lineCount; ++i ) {
        positions.append( OffsetInFile( nextOffset ) );
        nextOffset += 80 + static_cast<int64_t>( i % 13 );
    }
    const auto elapsed = timer.nsecsElapsed();
    const auto lastExpected = nextOffset - ( 80 + static_cast<int64_t>( ( lineCount - 1 ) % 13 ) );
    bool matches = positions.size().get() == lineCount && positions.at( 0 ).get() == 0
                   && positions.at( lineCount - 1 ).get() == lastExpected;
    const auto slice = positions.range( LineNumber( 127 ), LinesCount( 130 ) );
    int64_t expected = 127 * 80;
    for ( uint64_t i = 0; i < 127; ++i ) expected += static_cast<int64_t>( i % 13 );
    matches = matches && slice.size() == 130;
    for ( size_t i = 0; matches && i < slice.size(); ++i ) {
        matches = slice[ i ].get() == expected;
        expected += 80 + static_cast<int64_t>( ( 127 + i ) % 13 );
    }
    return {{"case", "normal-performance"}, {"status", "measured"},
            {"line_count", static_cast<qint64>( lineCount )}, {"matches_expected", matches},
            {"allocated_size_bytes", static_cast<qint64>( positions.allocatedSize() )},
            {"elapsed_ns", elapsed}};
}

void emitResult(const QJsonObject& object)
{
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << std::endl;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: klogg_index_probe CASE\n";
        return 2;
    }
    const std::string name = argv[1];
    if (name == "high-absolute-small-span") emitResult(measure("high-absolute-small-span", (uint64_t{1} << 32) + 17, 127));
    else if (name == "uint32-max-span") emitResult(measure("uint32-max-span", 1, UINT32_MAX));
    else if (name == "two-to-32-span") emitResult(measure("two-to-32-span", 1, uint64_t{1} << 32));
    else if (name == "lifecycle-pop-recompress") emitResult(lifecyclePopRecompress());
    else if (name == "lifecycle-move-append") emitResult(lifecycleMoveAppend());
    else if (name == "range-zero") emitResult(rangeZero());
    else if (name == "lifecycle-mixed-stress") emitResult(lifecycleMixedStress());
    else if (name == "normal-performance") emitResult(normalPerformance());
    else return 2;
    return 0;
}
