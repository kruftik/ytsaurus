﻿#include "stdafx.h"
#include "partition_chunk_writer.h"
#include "private.h"
#include "config.h"
#include "channel_writer.h"
#include "chunk_meta_extensions.h"
#include "partitioner.h"

#include <ytlib/ytree/lexer.h>
#include <ytlib/chunk_client/async_writer.h>
#include <ytlib/chunk_client/private.h>
#include <ytlib/chunk_client/encoding_writer.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkServer;
using namespace NChunkClient;
using namespace NYTree;

static NLog::TLogger& Logger = TableWriterLogger;

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkWriter::TPartitionChunkWriter(
    TChunkWriterConfigPtr config,
    NChunkClient::IAsyncWriterPtr chunkWriter,
    const TKeyColumns& keyColumns,
    IPartitioner* partitioner)
    : TChunkWriterBase(config, chunkWriter, keyColumns)
    , Partitioner(partitioner)
    , CurrentSize(0)
    , BasicMetaSize(0)
{
    for (int i = 0; i < KeyColumns.Get().size(); ++i) {
        KeyColumnIndexes[KeyColumns.Get()[i]] = i;
    }
    *ChannelsExt.add_items()->mutable_channel() = TChannel::CreateUniversal().ToProto();

    for (int partitionTag = 0; partitionTag < Partitioner->GetPartitionCount(); ++partitionTag) {
        // Write range column sizes to effectively skip during reading.
        ChannelWriters.push_back(New<TChannelWriter>(0, true));
        auto* partitionAttributes = PartitionsExt.add_partitions();
        partitionAttributes->set_row_count(0);
        partitionAttributes->set_uncompressed_data_size(0);
    }

    BasicMetaSize =
        ChannelsExt.ByteSize() +
        sizeof(i64) * Partitioner->GetPartitionCount() + 
        sizeof(NChunkHolder::NProto::TMiscExt) + 
        sizeof(NChunkHolder::NProto::TChunkMeta);
}

TPartitionChunkWriter::~TPartitionChunkWriter()
{ }

bool TPartitionChunkWriter::TryWriteRow(const TRow& row)
{
    // TODO(babenko): check column names
    return TryWriteRowUnsafe(row);
}

bool TPartitionChunkWriter::TryWriteRowUnsafe(const TRow& row)
{
    YASSERT(!State.IsClosed());

    if (!State.IsActive() || !EncodingWriter->IsReady())
        return false;

    int keyColumnCount = KeyColumns.Get().size();
    TNonOwningKey key(keyColumnCount);
    RowValueIndexes.assign(row.size() + keyColumnCount, -1);

    {
        int nonKeyIndex = keyColumnCount;
        for (int i = 0; i < row.size(); ++i) {
            const auto& pair = row[i];
            auto it = KeyColumnIndexes.find(pair.first);
            if (it != KeyColumnIndexes.end()) {
                key.SetKeyPart(it->second, pair.second, Lexer);
                RowValueIndexes[it->second] = i;
            } else {
                RowValueIndexes[nonKeyIndex] = i;
                ++nonKeyIndex;
            }
        }
    }

    int partitionTag = Partitioner->GetPartitionTag(key);
    auto& channelWriter = ChannelWriters[partitionTag];

    i64 rowDataWeight = 1;
    FOREACH (int valueIndex, RowValueIndexes) {
        if (valueIndex >= 0) {
            const auto& pair = row[valueIndex];
            channelWriter->WriteRange(pair.first, pair.second);

            rowDataWeight += pair.first.size();
            rowDataWeight += pair.second.size();
            ValueCount += 1;
        }
    }
    channelWriter->EndRow();

    // Update partition counters.
    auto* partitionAttributes = PartitionsExt.mutable_partitions(partitionTag);
    partitionAttributes->set_row_count(partitionAttributes->row_count() + 1);

    // Update global counters.
    DataWeight += rowDataWeight;
    RowCount += 1;

    if (channelWriter->GetCurrentSize() > static_cast<size_t>(Config->BlockSize)) {
        PrepareBlock(partitionTag);
    }

    CurrentSize = EncodingWriter->GetCompressedSize() + channelWriter->GetCurrentSize();
    return true;
}

void TPartitionChunkWriter::PrepareBlock(int partitionTag)
{
    auto& channelWriter = ChannelWriters[partitionTag];
    auto* blockInfo = ChannelsExt.mutable_items(0)->add_blocks();
    blockInfo->set_row_count(channelWriter->GetCurrentRowCount());
    blockInfo->set_partition_tag(partitionTag);
    blockInfo->set_block_index(CurrentBlockIndex);

    LOG_DEBUG("Emitting block for partition %d (BlockIndex %d, RowCount: %"PRId64")", 
        partitionTag, 
        CurrentBlockIndex, 
        channelWriter->GetCurrentRowCount());

    ++CurrentBlockIndex;

    int size = 0;
    auto blockParts(channelWriter->FlushBlock());
    FOREACH (auto& part, blockParts) {
        size += part.Size();
    }
    blockInfo->set_block_size(size);

    auto* partitionAttributes = PartitionsExt.mutable_partitions(partitionTag);
    partitionAttributes->set_uncompressed_data_size(
        partitionAttributes->uncompressed_data_size() + size);

    EncodingWriter->WriteBlock(MoveRV(blockParts));
}

i64 TPartitionChunkWriter::GetCurrentSize() const
{
    return CurrentSize;
}

i64 TPartitionChunkWriter::GetMetaSize() const
{
    return BasicMetaSize + CurrentBlockIndex * sizeof(NProto::TBlockInfo);
}

NChunkHolder::NProto::TChunkMeta TPartitionChunkWriter::GetMasterMeta() const
{
    NChunkHolder::NProto::TChunkMeta meta;
    meta.set_type(EChunkType::Table);
    SetProtoExtension(meta.mutable_extensions(), MiscExt);

    return meta;
}

NChunkHolder::NProto::TChunkMeta TPartitionChunkWriter::GetSchedulerMeta() const
{
    NChunkHolder::NProto::TChunkMeta meta;
    meta.set_type(EChunkType::Table);
    SetProtoExtension(meta.mutable_extensions(), MiscExt);
    SetProtoExtension(meta.mutable_extensions(), PartitionsExt);

    return meta;
}

TAsyncError TPartitionChunkWriter::AsyncClose()
{
    YASSERT(!State.IsClosed());

    State.StartOperation();

    for (int partitionTag = 0; partitionTag < Partitioner->GetPartitionCount(); ++partitionTag) {
        auto& channelWriter = ChannelWriters[partitionTag];
        if (channelWriter->GetCurrentRowCount() > 0) {
            PrepareBlock(partitionTag);
        }
    }

    EncodingWriter->AsyncFlush().Subscribe(BIND(
        &TPartitionChunkWriter::OnFinalBlocksWritten,
        MakeWeak(this)).Via(WriterThread->GetInvoker()));

    return State.GetOperationError();
}

void TPartitionChunkWriter::OnFinalBlocksWritten(TError error)
{
    if (!error.IsOK()) {
        State.FinishOperation(error);
        return;
    }

    SetProtoExtension(Meta.mutable_extensions(), PartitionsExt);
    FinalizeWriter();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
