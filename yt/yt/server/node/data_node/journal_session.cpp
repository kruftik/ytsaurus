#include "journal_session.h"

#include "bootstrap.h"
#include "chunk_store.h"
#include "journal_chunk.h"
#include "journal_dispatcher.h"
#include "location.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/config.h>

#include <yt/yt/server/lib/hydra/file_changelog.h>

#include <yt/yt/ytlib/chunk_client/proto/chunk_info.pb.h>

namespace NYT::NDataNode {

using namespace NHydra;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NConcurrency;
using namespace NIO;

////////////////////////////////////////////////////////////////////////////////

TFuture<void> TJournalSession::DoStart()
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    const auto& dispatcher = Bootstrap_->GetJournalDispatcher();
    auto changelogFuture = dispatcher->CreateJournal(
        Location_,
        GetChunkId(),
        Options_.EnableMultiplexing,
        Options_.WorkloadDescriptor);

    return changelogFuture.Apply(BIND([=, this, this_ = MakeStrong(this)] (const IFileChangelogPtr& changelog) {
        VERIFY_INVOKER_AFFINITY(SessionInvoker_);

        Changelog_ = changelog;
        Chunk_ = New<TJournalChunk>(
            TChunkContext::Create(Bootstrap_),
            Location_,
            TChunkDescriptor(GetChunkId()));
        Chunk_->SetActive(true);
        ChunkUpdateGuard_ = TChunkUpdateGuard::Acquire(Chunk_);

        const auto& chunkStore = Bootstrap_->GetChunkStore();
        chunkStore->RegisterNewChunk(Chunk_, /*session*/ this, std::move(LockedChunkGuard_));
    }).AsyncVia(SessionInvoker_));
}

void TJournalSession::DoCancel(const TError& /*error*/)
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    OnFinished();
}

TFuture<TChunkInfo> TJournalSession::DoFinish(
    const TRefCountedChunkMetaPtr& /*chunkMeta*/,
    std::optional<int> blockCount)
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    auto result = Changelog_->Finish();

    if (blockCount) {
        if (*blockCount != Changelog_->GetRecordCount()) {
            return MakeFuture<TChunkInfo>(TError("Block count mismatch in journal session %v: expected %v, got %v",
                SessionId_,
                Changelog_->GetRecordCount(),
                *blockCount));
        }
        result = result.Apply(BIND(&TJournalChunk::Seal, Chunk_)
            .AsyncVia(SessionInvoker_));
    }

    return result.Apply(BIND([=, this, this_ = MakeStrong(this)] (const TError& error) {
        OnFinished();

        error.ThrowOnError();

        TChunkInfo info;
        info.set_disk_space(Chunk_->GetDataSize());
        info.set_sealed(Chunk_->IsSealed());
        return info;
    }).AsyncVia(SessionInvoker_));
}

TFuture<NIO::TIOCounters> TJournalSession::DoPutBlocks(
    int startBlockIndex,
    std::vector<TBlock> blocks,
    bool /*enableCaching*/)
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    int recordCount = Changelog_->GetRecordCount();

    if (startBlockIndex > recordCount) {
        THROW_ERROR_EXCEPTION("Missing blocks %v:%v-%v",
            GetId(),
            recordCount,
            startBlockIndex - 1);
    }

    if (startBlockIndex < recordCount) {
        YT_LOG_DEBUG("Skipped duplicate blocks (BlockIds: %v:%v-%v)",
            GetId(),
            startBlockIndex,
            recordCount - 1);
    }

    int payloadSize = 0;
    std::vector<TSharedRef> records;
    records.reserve(blocks.size() - recordCount + startBlockIndex);
    for (int index = recordCount - startBlockIndex;
         index < static_cast<int>(blocks.size());
         ++index)
    {
        records.push_back(blocks[index].Data);
        payloadSize += records.back().Size();
    }

    if (!records.empty()) {
        auto flushedRowCount = startBlockIndex + blocks.size();
        LastAppendResult_ = Changelog_->Append(records)
            .Apply(BIND([chunk = Chunk_, changelog = Changelog_, flushedRowCount] {
                chunk->UpdateFlushedRowCount(flushedRowCount);
                chunk->UpdateDataSize(changelog->GetDataSize());
            }));
    }

    return MakeFuture(TIOCounters{
        .Bytes = Changelog_->EstimateChangelogSize(payloadSize),
        .IORequests = 1,
    });
}

TFuture<TDataNodeServiceProxy::TRspPutBlocksPtr> TJournalSession::DoSendBlocks(
    int /*startBlockIndex*/,
    int /*blockCount*/,
    const TNodeDescriptor& /*target*/)
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    THROW_ERROR_EXCEPTION("Sending blocks is not supported for journal chunks");
}

TFuture<TIOCounters> TJournalSession::DoFlushBlocks(int blockIndex)
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    int recordCount = Changelog_->GetRecordCount();

    if (blockIndex > recordCount) {
        THROW_ERROR_EXCEPTION("Missing blocks %v:%v-%v",
            GetId(),
            recordCount - 1,
            blockIndex);
    }

    return LastAppendResult_
        .Apply(BIND([this, this_ = MakeStrong(this)] {
            i64 newDataSize = Chunk_->GetDataSize();
            auto oldDataSize = std::exchange(LastDataSize_, newDataSize);
            YT_VERIFY(oldDataSize <= newDataSize);

            // FinishChunk must induce a barrier as follows:
            // if FinishChunk succeeds and is subsequently followed by GetChunkMeta returning N rows,
            // no client writing to this chunk may ever receive a successful flush acknowlegement for >N rows.
            // See YT-21626 for the details.
            ValidateActive();

            return TIOCounters{
                .Bytes = newDataSize - oldDataSize,
                .IORequests = oldDataSize == newDataSize ? 0 : 1,
            };
        }).AsyncVia(SessionInvoker_));
}

void TJournalSession::OnFinished()
{
    VERIFY_INVOKER_AFFINITY(SessionInvoker_);

    if (Chunk_ && Changelog_) {
        Chunk_->UpdateFlushedRowCount(Changelog_->GetRecordCount());
        Chunk_->UpdateDataSize(Changelog_->GetDataSize());
    }

    if (Chunk_) {
        Chunk_->SetActive(false);

        const auto& chunkStore = Bootstrap_->GetChunkStore();
        chunkStore->UpdateExistingChunk(Chunk_);
    }

    Finished_.Fire(TError());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
