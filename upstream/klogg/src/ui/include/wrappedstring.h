/*
 * Copyright (C) 2023 -- 2024 Anton Filimonov and other contributors
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

#include <QString>
#include <cstddef>
#include <memory>
#include <qchar.h>
#include <qglobal.h>

#include <QStringView>

#include "containers.h"
#include "linetypes.h"

class WrappedString {
    struct Storage { klogg::vector<QStringView> rows; QString text; };
public:
    using WrappedStringPart = QStringView;

    static WrappedStringPart makeWrappedStringPart(const QString& lineText, 
        LineColumn firstCol, LineLength length ) {
        return QStringView( lineText ).mid( firstCol.get(), length.get() );
    }

    explicit WrappedString( QString longLine, LineLength visibleColumns )
    {
        const auto width = std::max<LineLength::UnderlyingType>( 1, visibleColumns.get() );
        storage_->text = longLine;
        if ( longLine.isEmpty() ) {
            storage_->rows.push_back( WrappedStringPart{} );
        }
        else {
            WrappedStringPart lineToWrap( longLine );
            while ( lineToWrap.size() > width ) {
                WrappedStringPart stringToWrap = lineToWrap.left( width );
                auto lastSpaceIt = std::find_if( stringToWrap.rbegin(), stringToWrap.rend(),
                                                 []( QChar c ) { return c.isSpace(); } );
                if ( lastSpaceIt == stringToWrap.rend() ) {
                    storage_->rows.push_back( lineToWrap.left( width ) );
                    lineToWrap = lineToWrap.mid( width );
                }
                else {
                    auto spacePos = std::distance( stringToWrap.begin(), lastSpaceIt.base() );
                    storage_->rows.push_back( lineToWrap.left( spacePos ) );
                    lineToWrap = lineToWrap.mid( spacePos );
                }
            }
            if ( lineToWrap.size() > 0 ) {
                storage_->rows.push_back( lineToWrap );
            }
        }
    }

    size_t wrappedLinesCount() const
    {
        return storage_->rows.size();
    }

    size_t retainedBytes() const
    {
        return sizeof( Storage ) + storage_->rows.capacity() * sizeof( WrappedStringPart )
               + static_cast<size_t>( storage_->text.capacity() ) * sizeof( QChar );
    }

    klogg::vector<WrappedStringPart> mid( LineColumn start, LineLength length ) const
    {
        auto getLength = []( const auto& view ) -> LineLength::UnderlyingType {
            return type_safe::narrow_cast<LineLength::UnderlyingType>( view.size() );
        };

        klogg::vector<WrappedStringPart> resultChunks;
        if ( storage_->rows.size() == 1 ) {
            auto& wrappedLine = storage_->rows.front();
            auto len = std::min( length.get(), getLength( wrappedLine ) - start.get() );
            resultChunks.push_back( wrappedLine.mid( start.get(), ( len > 0 ? len : 0 ) ) );
            return resultChunks;
        }

        size_t wrappedLineIndex = 0;
        auto positionInWrappedLine = start.get();
        while ( positionInWrappedLine > getLength( storage_->rows[ wrappedLineIndex ] ) ) {
            positionInWrappedLine -= getLength( storage_->rows[ wrappedLineIndex ] );
            wrappedLineIndex++;
            if ( wrappedLineIndex >= storage_->rows.size() ) {
                return resultChunks;
            }
        }

        auto chunkLength = length.get();
        while ( positionInWrappedLine + chunkLength
                > getLength( storage_->rows[ wrappedLineIndex ] ) ) {
            resultChunks.push_back(
                storage_->rows[ wrappedLineIndex ].mid( positionInWrappedLine ) );
            wrappedLineIndex++;
            positionInWrappedLine = 0;
            chunkLength -= getLength( resultChunks.back() );
            if ( wrappedLineIndex >= storage_->rows.size() ) {
                return resultChunks;
            }
        }

        if ( chunkLength > 0 ) {
            auto& wrappedLine = storage_->rows[ wrappedLineIndex ];
            auto len = std::min( chunkLength, getLength( wrappedLine ) - positionInWrappedLine );
            resultChunks.push_back(
                wrappedLine.mid( positionInWrappedLine, ( len > 0 ? len : 0 ) ) );
        }

        return resultChunks;
    }

    bool isEmpty() const
    {
        return storage_->text.isEmpty();
    }

    WrappedStringPart unwrappedLine() const {
        return WrappedStringPart{storage_->text};
    }

    WrappedStringPart wrappedLine(size_t index) const {
        return storage_->rows[index];
    }

private:
    std::shared_ptr<Storage> storage_ = std::make_shared<Storage>();
};
