// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "atomicflag.h"
#include "logdata.h"
#include "regularexpressionpattern.h"
#include <functional>
#include <vector>

enum class SearchCompletion { Running, Complete, Cancelled, Partial, Failed, SourceChanged };

struct LiteralHit {
    LineNumber line;
    OffsetInFile byteOffset;
};

struct LiteralSearchOptions {
    qint64 chunkBytes = 1024 * 1024;
    size_t maxMatchingLines = 65536;
    static constexpr qint64 MaxPatternBytes = 1024 * 1024;
};

struct LiteralSearchResult {
    SearchCompletion completion = SearchCompletion::Failed;
    QString message;
    std::vector<LiteralHit> hits;
    qint64 bytesProcessed = 0;
    quint64 completedLines = 0;
    size_t peakBufferBytes = 0;
};

// Every hit is the first occurrence on a source line. LF is a delimiter, CR is
// content (the same raw-line semantics as the existing UTF-8 search path).
LiteralSearchResult searchLiteralBytes(
    const LogData& source, const LogData::SourceSnapshot& snapshot,
    LineNumber firstLine, OffsetInFile begin, OffsetInFile end, const QByteArray& pattern,
    const AtomicFlag& cancelled, const LiteralSearchOptions& options = {},
    const std::function<void(qint64, size_t)>& progress = {});

QString searchCompletionName(SearchCompletion completion);
QString longLineSearchError(const LogData& source, const RegularExpressionPattern& pattern);
