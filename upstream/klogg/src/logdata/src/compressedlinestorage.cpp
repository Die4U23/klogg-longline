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

#include <QtEndian>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

#include "compressedlinestorage.h"
#include "containers.h"
#include "cpu_info.h"
#include "linetypes.h"
#include "log.h"

#include <streamvbyte.h>
#include <streamvbytedelta.h>

static constexpr size_t SimdIndexBlockSize = 128;

void CompressedLinePositionStorage::move_from( CompressedLinePositionStorage&& orig ) noexcept
{
    if ( this == &orig ) {
        return;
    }
    blocks_ = std::move( orig.blocks_ );
    wideBlockFlags_ = std::move( orig.wideBlockFlags_ );
    packedLinesStorage_ = std::move( orig.packedLinesStorage_ );
    packedLinesStorageUsedSize_ = orig.packedLinesStorageUsedSize_;
    wideLinesStorage_ = std::move( orig.wideLinesStorage_ );
    currentLinesBlock_ = std::move( orig.currentLinesBlock_ );
    currentLinesBlockShifted_ = std::move( orig.currentLinesBlockShifted_ );

    nbLines_ = orig.nbLines_;
    lastPos_ = orig.lastPos_;
    canUseSimdSelect_ = orig.canUseSimdSelect_;

    orig.nbLines_ = 0_lcount;
    orig.lastPos_ = 0_offset;
    orig.packedLinesStorageUsedSize_ = 0;
    orig.blocks_.clear();
    orig.wideBlockFlags_.clear();
    orig.packedLinesStorage_.clear();
    orig.wideLinesStorage_.clear();
    orig.currentLinesBlock_.clear();
    orig.currentLinesBlockShifted_.clear();
}

CompressedLinePositionStorage::CompressedLinePositionStorage()
{
    auto requiredInstructions = CpuInstructions::SSE41;
    canUseSimdSelect_ = hasRequiredInstructions( supportedCpuInstructions(), requiredInstructions );
}

CompressedLinePositionStorage::CompressedLinePositionStorage(
    CompressedLinePositionStorage&& orig ) noexcept
{
    move_from( std::move( orig ) );
}

CompressedLinePositionStorage&
CompressedLinePositionStorage::operator=( CompressedLinePositionStorage&& orig ) noexcept
{
    move_from( std::move( orig ) );
    return *this;
}

void CompressedLinePositionStorage::append( OffsetInFile pos )
{
    // Lines must be stored in order
    assert( ( pos > lastPos_ ) || ( pos == 0_offset ) );

    currentLinesBlock_.push_back( pos );
    const auto delta = pos.get() - currentLinesBlock_.front().get();
    currentLinesBlockShifted_.push_back(
        delta > std::numeric_limits<uint32_t>::max()
            ? 0u
            : type_safe::narrow_cast<uint32_t>( delta ) );

    if ( currentLinesBlock_.size() == SimdIndexBlockSize ) {
        compress_current_block();
    }

    lastPos_ = pos;
    ++nbLines_;
}

void CompressedLinePositionStorage::compress_current_block()
{
    BlockMetadata& block = blocks_.emplace_back();
    block.firstLineOffset = currentLinesBlock_.front();

    const auto blockSpan = static_cast<uint64_t>( currentLinesBlock_.back().get()
                                                  - currentLinesBlock_.front().get() );
    const bool usesWideStorage = blockSpan > std::numeric_limits<uint32_t>::max();
    wideBlockFlags_.push_back( usesWideStorage ? 1u : 0u );
    if ( usesWideStorage ) {
        block.storageOffset = wideLinesStorage_.size();
        for ( const auto pos : currentLinesBlock_ ) {
            wideLinesStorage_.push_back( static_cast<uint64_t>( pos.get()
                                                                - block.firstLineOffset.get() ) );
        }
    }
    else {
        const size_t packedLinesSize = streamvbyte_max_compressedbytes( SimdIndexBlockSize );
        packedLinesStorage_.resize( packedLinesStorageUsedSize_ + packedLinesSize );
        block.storageOffset = packedLinesStorageUsedSize_;

        const size_t packedBytes
            = streamvbyte_delta_encode( currentLinesBlockShifted_.data(), SimdIndexBlockSize,
                                        packedLinesStorage_.data() + block.storageOffset, 0 );

        packedLinesStorageUsedSize_ += packedBytes;
    }

    currentLinesBlock_.clear();
    currentLinesBlockShifted_.clear();
}

OffsetInFile CompressedLinePositionStorage::at( LineNumber index ) const
{
    if ( index >= nbLines_ ) {
        LOG_ERROR << "Line number not in storage: " << index.get() << ", storage size is "
                  << nbLines_;
        throw std::runtime_error( "Line number not in storage" );
    }

    const size_t blockIndex = index.get() / SimdIndexBlockSize;
    const size_t indexInBlock = index.get() % SimdIndexBlockSize;

    if ( blockIndex == blocks_.size() ) {
        return currentLinesBlock_[ indexInBlock ];
    }

    const BlockMetadata& block = blocks_[ blockIndex ];
    if ( wideBlockFlags_[ blockIndex ] ) {
        const auto delta = wideLinesStorage_[ block.storageOffset + indexInBlock ];
        return block.firstLineOffset + OffsetInFile( static_cast<int64_t>( delta ) );
    }
    std::array<uint32_t, SimdIndexBlockSize> unpackedBlock;
    streamvbyte_delta_decode( &packedLinesStorage_[ block.storageOffset ],
                              unpackedBlock.data(), SimdIndexBlockSize, 0 );

    return block.firstLineOffset + OffsetInFile( unpackedBlock[ indexInBlock ] );
}

void CompressedLinePositionStorage::append_list( const klogg::vector<OffsetInFile>& positions )
{
    // This is not very clever, but caching should make it
    // reasonably fast.
    for ( auto position : positions )
        append( position );
}

void CompressedLinePositionStorage::uncompress_last_block()
{
    currentLinesBlock_.resize( SimdIndexBlockSize );
    currentLinesBlockShifted_.resize( SimdIndexBlockSize );
    const BlockMetadata& block = blocks_.back();
    const bool usesWideStorage = wideBlockFlags_.back() != 0;

    if ( usesWideStorage ) {
        for ( size_t i = 0; i < SimdIndexBlockSize; ++i ) {
            const auto delta = wideLinesStorage_[ block.storageOffset + i ];
            currentLinesBlock_[ i ]
                = block.firstLineOffset + OffsetInFile( static_cast<int64_t>( delta ) );
            currentLinesBlockShifted_[ i ]
                = delta <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>( delta ) : 0u;
        }
        wideLinesStorage_.resize( block.storageOffset );
    }
    else {
        streamvbyte_delta_decode( &packedLinesStorage_[ block.storageOffset ],
                                  currentLinesBlockShifted_.data(), SimdIndexBlockSize, 0 );

        std::transform( currentLinesBlockShifted_.begin(), currentLinesBlockShifted_.end(),
                        currentLinesBlock_.begin(), [ &block ]( uint32_t pos ) {
                            return OffsetInFile( pos ) + block.firstLineOffset;
                        } );
        packedLinesStorageUsedSize_ = block.storageOffset;
        packedLinesStorage_.resize( packedLinesStorageUsedSize_ + STREAMVBYTE_PADDING );
    }

    blocks_.pop_back();
    wideBlockFlags_.pop_back();
}

void CompressedLinePositionStorage::pop_back()
{
    if ( currentLinesBlock_.empty() && !blocks_.empty() ) {
        // Last entry caused block compression, so we need to uncompress it
        // to de-alloc last entry.
        uncompress_last_block();
    }

    if ( !currentLinesBlock_.empty() ) {
        currentLinesBlock_.pop_back();
        currentLinesBlockShifted_.pop_back();
    }

    if ( nbLines_.get() == 0 ) {
        lastPos_ = 0_offset;
    }
    else {
        --nbLines_;
        lastPos_ = nbLines_.get() > 0 ? at( nbLines_.get() - 1 ) : 0_offset;
    }
}

size_t CompressedLinePositionStorage::allocatedSize() const
{
    return packedLinesStorage_.capacity() + wideLinesStorage_.capacity() * sizeof( uint64_t )
           + blocks_.capacity() * sizeof( BlockMetadata ) + wideBlockFlags_.capacity()
           + currentLinesBlock_.capacity() * sizeof( OffsetInFile )
           + currentLinesBlockShifted_.capacity() * sizeof( uint32_t );
}

klogg::vector<OffsetInFile> CompressedLinePositionStorage::range( LineNumber firstLine,
                                                                  LinesCount count ) const
{
    if ( count.get() == 0 ) {
        return {};
    }
    if ( firstLine.get() >= nbLines_.get() || count.get() > nbLines_.get() - firstLine.get() ) {
        throw std::out_of_range( "Line range not in storage" );
    }

    const size_t firstBlockIndex = firstLine.get() / SimdIndexBlockSize;
    const size_t indexInFirstBlock = firstLine.get() % SimdIndexBlockSize;
    const size_t lastLineIndex = static_cast<size_t>( firstLine.get() + count.get() - 1 );
    const size_t lastBlockIndex = lastLineIndex / SimdIndexBlockSize;
    const size_t indexInLastBlock = lastLineIndex % SimdIndexBlockSize;

    klogg::vector<OffsetInFile> result;
    result.reserve( static_cast<size_t>( count.get() ) );

    if ( firstBlockIndex == blocks_.size() ) {
        std::copy( currentLinesBlock_.begin() + static_cast<int64_t>( indexInFirstBlock ),
                   currentLinesBlock_.begin() + static_cast<int64_t>( indexInLastBlock + 1 ),
                   std::back_inserter( result ) );
    }
    else {
        for ( size_t blockIndex = firstBlockIndex;
              blockIndex <= lastBlockIndex && blockIndex < blocks_.size(); ++blockIndex ) {
            const BlockMetadata& block = blocks_[ blockIndex ];
            const bool usesWideStorage = wideBlockFlags_[ blockIndex ] != 0;
            const size_t copyFromIndex = blockIndex == firstBlockIndex ? indexInFirstBlock : 0u;
            const size_t copyToIndex = blockIndex == lastBlockIndex ? indexInLastBlock + 1
                                                                    : SimdIndexBlockSize;
            if ( usesWideStorage ) {
                for ( size_t i = copyFromIndex; i < copyToIndex; ++i ) {
                    const auto delta = wideLinesStorage_[ block.storageOffset + i ];
                    result.push_back( block.firstLineOffset + OffsetInFile( static_cast<int64_t>( delta ) ) );
                }
            }
            else {
                std::array<uint32_t, SimdIndexBlockSize> unpackedBlock;
                streamvbyte_delta_decode( &packedLinesStorage_[ block.storageOffset ],
                                          unpackedBlock.data(), SimdIndexBlockSize, 0 );
                std::transform( unpackedBlock.begin() + copyFromIndex,
                                unpackedBlock.begin() + copyToIndex, std::back_inserter( result ),
                                [ &block ]( uint32_t pos ) {
                                    return OffsetInFile( pos ) + block.firstLineOffset;
                                } );
            }
        }

        if ( lastBlockIndex == blocks_.size() ) {
            const size_t copyFromIndex = firstBlockIndex == blocks_.size() ? indexInFirstBlock : 0u;
            std::copy( currentLinesBlock_.begin() + static_cast<int64_t>( copyFromIndex ),
                       currentLinesBlock_.begin() + static_cast<int64_t>( indexInLastBlock + 1 ),
                       std::back_inserter( result ) );
        }
    }

    return result;
}
