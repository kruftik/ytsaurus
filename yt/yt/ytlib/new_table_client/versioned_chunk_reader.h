#pragma once

#include <yt/yt/client/table_client/versioned_reader.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/versioned_row.h>

#include <yt/yt/ytlib/table_client/public.h>
#include <yt/yt/ytlib/chunk_client/public.h>

namespace NYT::NNewTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TReaderTimeStatistics final
{
    TDuration DecodeSegmentTime;
    TDuration FetchBlockTime;
    TDuration BuildRangesTime;
    TDuration DoReadTime;
};

using TReaderTimeStatisticsPtr = TIntrusivePtr<TReaderTimeStatistics>;

template <class TItem>
NTableClient::IVersionedReaderPtr CreateVersionedChunkReader(
    TSharedRange<TItem> readItems,
    NTableClient::TTimestamp timestamp,
    NTableClient::TCachedVersionedChunkMetaPtr chunkMeta,
    const NTableClient::TColumnFilter& columnFilter,
    NChunkClient::IBlockCachePtr blockCache,
    const NTableClient::TChunkReaderConfigPtr config,
    NChunkClient::IChunkReaderPtr underlyingReader,
    NTableClient::TChunkReaderPerformanceCountersPtr performanceCounters,
    const NChunkClient::TClientBlockReadOptions& blockReadOptions,
    bool produceAll,
    TReaderTimeStatisticsPtr timeStatistics = nullptr);

////////////////////////////////////////////////////////////////////////////////

using THolderPtr = TIntrusivePtr<TRefCounted>;

// Chunk view support.
TSharedRange<NTableClient::TRowRange> ClipRanges(
    TSharedRange<NTableClient::TRowRange> ranges,
    NTableClient::TUnversionedRow lower,
    NTableClient::TUnversionedRow upper,
    THolderPtr holder);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNewTableClient
