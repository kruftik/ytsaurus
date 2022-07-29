#pragma once

#include "public.h"
#include "common.h"

#include <yt/yt/client/api/public.h>

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/client/ypath/public.h>

#include <yt/yt/core/actions/future.h>

namespace NYT::NQueueClient {

////////////////////////////////////////////////////////////////////////////////

struct TPartitionInfo
{
    i64 PartitionIndex = -1;
    i64 NextRowIndex = -1;
    //! Latest time instant the corresponding partition was consumed.
    TInstant LastConsumeTime;
};

////////////////////////////////////////////////////////////////////////////////

//! Interface representing a consumer table.
struct IConsumerClient
    : public TRefCounted
{
    //! Advance the offset for the given partition, setting it to a new value within the given transaction.
    //!
    //! If oldOffset is specified, the current offset is read (within the transaction) and compared with oldOffset.
    //! If they are equal, the new offset is written, otherwise an exception is thrown.
    virtual void Advance(
        const NApi::ITransactionPtr& transaction,
        int partitionIndex,
        std::optional<i64> oldOffset,
        i64 newOffset) const = 0;

    //! Collect partition infos. If there are entries in the consumer table with partition
    //! indices outside of range [0, expectedPartitionCount), they will be ignored.
    virtual TFuture<std::vector<TPartitionInfo>> CollectPartitions(
        const NApi::IClientPtr& client,
        int expectedPartitionCount,
        bool withLastConsumeTime = false) const = 0;

    //! Collect partition infos.
    //! Entries in the consumer table with partition indices not in the given vector will be ignored.
    virtual TFuture<std::vector<TPartitionInfo>> CollectPartitions(
        const NApi::IClientPtr& client,
        const std::vector<int>& partitionIndexes,
        bool withLastConsumeTime = false) const = 0;

    //! Fetch and parse the target_queue attribute of this consumer.
    virtual TFuture<TCrossClusterReference> FetchTargetQueue(const NApi::IClientPtr& client) const = 0;

    struct TPartitionStatistics
    {
        i64 FlushedDataWeight = 0;
        i64 FlushedRowCount = 0;
    };

    // TODO(achulkov2): This should probably handle multiple partition indexes at some point.
    // TODO(achulkov2): Move this to a separate IQueueClient class?
    //! Fetch relevant per-tablet statistics for queue.
    virtual TFuture<TPartitionStatistics> FetchPartitionStatistics(
        const NApi::IClientPtr& client,
        const NYPath::TYPath& queue,
        int partitionIndex) const = 0;
};

DEFINE_REFCOUNTED_TYPE(IConsumerClient);

////////////////////////////////////////////////////////////////////////////////

//! Uses the given schema to dispatch the appropriate consumer client.
IConsumerClientPtr CreateConsumerClient(
    const NYPath::TYPath& path,
    const NTableClient::TTableSchema& schema);

//! Uses the table mount cache to fetch the consumer's schema and dispatch the appropriate consumer client.
IConsumerClientPtr CreateConsumerClient(
    const NApi::IClientPtr& client,
    const NYPath::TYPath& path);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueClient
