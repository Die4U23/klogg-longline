// SPDX-License-Identifier: GPL-3.0-or-later
#include "longlinesearch.h"
#include <algorithm>
#include <cstdint>

namespace {
// Carries only the incomplete UTF-8 sequence across reads. Rejects overlong
// sequences, surrogates and values above U+10FFFF without constructing UTF-16.
struct Utf8Validator {
    int remaining = 0;
    unsigned char lower = 0x80, upper = 0xbf;
    bool consume(unsigned char byte) {
        if (remaining) {
            if (byte < lower || byte > upper) return false;
            --remaining;
            lower = 0x80;
            upper = 0xbf;
            return true;
        }
        if (byte < 0x80) return true;
        if (byte >= 0xc2 && byte <= 0xdf) remaining = 1;
        else if (byte >= 0xe0 && byte <= 0xef) {
            remaining = 2;
            if (byte == 0xe0) lower = 0xa0;
            if (byte == 0xed) upper = 0x9f;
        }
        else if (byte >= 0xf0 && byte <= 0xf4) {
            remaining = 3;
            if (byte == 0xf0) lower = 0x90;
            if (byte == 0xf4) upper = 0x8f;
        }
        else return false;
        return true;
    }
};
}

QString searchCompletionName(SearchCompletion completion)
{
    switch (completion) {
    case SearchCompletion::Running: return QStringLiteral("running");
    case SearchCompletion::Complete: return QStringLiteral("complete");
    case SearchCompletion::Cancelled: return QStringLiteral("cancelled");
    case SearchCompletion::Partial: return QStringLiteral("partial");
    case SearchCompletion::SourceChanged: return QStringLiteral("source_changed");
    case SearchCompletion::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QString longLineSearchError(const LogData& source, const RegularExpressionPattern& pattern)
{
    if (source.getMaxLength().get() < 1024 * 1024) return {};
    if (!pattern.isPlainText || !pattern.isCaseSensitive || pattern.isExclude
        || pattern.isBoolean || pattern.isPrefilter || source.hasPrefilter()
        || source.getDisplayEncoding()->mibEnum() != 106)
        return QStringLiteral("Long-line search supports case-sensitive UTF-8 literal text only; "
                              "disable regex, Boolean, exclude and prefilter modes.");
    if (pattern.pattern.isEmpty() || pattern.pattern.size() > LiteralSearchOptions::MaxPatternBytes
        || pattern.pattern.contains(QChar::LineFeed)
        || pattern.pattern.toUtf8().size() > LiteralSearchOptions::MaxPatternBytes)
        return QStringLiteral("Long-line literal text must contain 1 to 1048576 UTF-8 bytes and no LF.");
    return {};
}

LiteralSearchResult searchLiteralBytes(
    const LogData& source, const LogData::SourceSnapshot& snapshot,
    LineNumber firstLine, OffsetInFile begin, OffsetInFile end, const QByteArray& pattern,
    const AtomicFlag& cancelled, const LiteralSearchOptions& options,
    const std::function<void(qint64, size_t)>& progress)
{
    LiteralSearchResult result;
    const auto finish = [&](SearchCompletion state, const QString& message) {
        result.completion = state;
        result.message = message;
        if (state == SearchCompletion::SourceChanged || state == SearchCompletion::Failed)
            result.hits.clear();
    };
    if (pattern.isEmpty() || pattern.size() > LiteralSearchOptions::MaxPatternBytes
        || pattern.contains('\n') || options.chunkBytes < 1
        || options.chunkBytes > LogData::MaxByteRead || options.maxMatchingLines > 65536
        || begin.get() < 0 || end < begin || end.get() > snapshot.size) {
        finish(SearchCompletion::Failed, QStringLiteral("Unsupported literal, byte range or search limits."));
        return result;
    }
    Utf8Validator patternValidator;
    for (const auto byte : pattern) {
        if (!patternValidator.consume(static_cast<unsigned char>(byte))) {
            finish(SearchCompletion::Failed, QStringLiteral("The search pattern must be valid UTF-8."));
            return result;
        }
    }
    if (patternValidator.remaining) {
        finish(SearchCompletion::Failed, QStringLiteral("The search pattern ends inside a UTF-8 sequence."));
        return result;
    }

    std::vector<uint32_t> prefix(static_cast<size_t>(pattern.size()), 0);
    uint32_t matched = 0;
    for (qsizetype i = 1; i < pattern.size(); ++i) {
        while (matched && pattern[i] != pattern[matched]) matched = prefix[matched - 1];
        if (pattern[i] == pattern[matched]) ++matched;
        prefix[static_cast<size_t>(i)] = matched;
    }
    matched = 0;
    result.hits.reserve(options.maxMatchingLines);
    const auto fixedBytes = prefix.capacity() * sizeof(uint32_t)
                            + result.hits.capacity() * sizeof(LiteralHit)
                            + static_cast<size_t>(pattern.capacity());
    Utf8Validator validator;
    auto line = firstLine;
    bool lineMatched = false, hasUnterminatedLine = false;
    qint64 offset = begin.get();
    while (offset < end.get()) {
        if (cancelled) {
            finish(SearchCompletion::Cancelled, QStringLiteral("Search cancelled; results are incomplete."));
            return result;
        }
        const auto requested = std::min(options.chunkBytes, end.get() - offset);
        const auto block = source.readBytes(snapshot, OffsetInFile(offset), requested);
        if (block.status != LogData::ReadStatus::Complete) {
            const auto state = block.status == LogData::ReadStatus::SourceChanged
                                   ? SearchCompletion::SourceChanged : SearchCompletion::Failed;
            finish(state, QStringLiteral("The source changed or its byte range could not be read."));
            return result;
        }
        result.peakBufferBytes = std::max(result.peakBufferBytes,
            fixedBytes + static_cast<size_t>(block.bytes.capacity()));
        for (qsizetype i = 0; i < block.bytes.size(); ++i) {
            if ((i & 65535) == 0 && cancelled) {
                result.bytesProcessed = offset + i - begin.get();
                finish(SearchCompletion::Cancelled, QStringLiteral("Search cancelled; results are incomplete."));
                return result;
            }
            const char byte = block.bytes[i];
            if (!validator.consume(static_cast<unsigned char>(byte))) {
                finish(SearchCompletion::Failed, QStringLiteral("Invalid UTF-8 in the selected source range."));
                return result;
            }
            if (byte == '\n') {
                ++line;
                ++result.completedLines;
                matched = 0;
                lineMatched = false;
                hasUnterminatedLine = false;
                continue;
            }
            hasUnterminatedLine = true;
            if (lineMatched) continue;
            while (matched && byte != pattern[matched]) matched = prefix[matched - 1];
            if (byte == pattern[matched]) ++matched;
            if (matched == static_cast<uint32_t>(pattern.size())) {
                if (result.hits.size() == options.maxMatchingLines) {
                    result.bytesProcessed = offset + i + 1 - begin.get();
                    finish(SearchCompletion::Partial, QStringLiteral("Matching-line limit reached; results are incomplete."));
                    return result;
                }
                result.hits.push_back({line, OffsetInFile(offset + i + 1 - pattern.size())});
                lineMatched = true;
                matched = 0;
            }
        }
        offset += block.bytes.size();
        result.bytesProcessed = offset - begin.get();
        if (progress) progress(result.bytesProcessed, result.hits.size());
    }
    if (!source.matchesSnapshot(snapshot)) {
        finish(SearchCompletion::SourceChanged, QStringLiteral("Source changed before search completion."));
    }
    else if (cancelled) {
        finish(SearchCompletion::Cancelled, QStringLiteral("Search cancelled; results are incomplete."));
    }
    else if (validator.remaining) {
        finish(SearchCompletion::Failed, QStringLiteral("Source ends inside a UTF-8 sequence."));
    }
    else {
        if (hasUnterminatedLine) ++result.completedLines;
        finish(SearchCompletion::Complete, {});
    }
    return result;
}
