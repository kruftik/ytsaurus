#include "stdafx.h"
#include "recovery.h"
#include "snapshot_downloader.h"
#include "change_log_downloader.h"

#include <ytlib/actions/action_util.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/foreach.h>

// TODO: wrap with try/catch to handle IO errors

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("MetaState");
static NProfiling::TProfiler Profiler("meta_state");

////////////////////////////////////////////////////////////////////////////////

TRecovery::TRecovery(
    TPersistentStateManagerConfig* config,
    TCellManager* cellManager,
    TDecoratedMetaState* decoratedState,
    TChangeLogCache* changeLogCache,
    TSnapshotStore* snapshotStore,
    const TEpoch& epoch,
    TPeerId leaderId,
    IInvoker* epochControlInvoker,
    IInvoker* epochStateInvoker)
    : Config(config)
    , CellManager(cellManager)
    , DecoratedState(decoratedState)
    , ChangeLogCache(changeLogCache)
    , SnapshotStore(snapshotStore)
    , Epoch(epoch)
    , LeaderId(leaderId)
    , EpochControlInvoker(epochControlInvoker)
    , EpochStateInvoker(epochStateInvoker)
{
    YASSERT(config);
    YASSERT(cellManager);
    YASSERT(decoratedState);
    YASSERT(changeLogCache);
    YASSERT(snapshotStore);
    YASSERT(epochControlInvoker);
    YASSERT(epochStateInvoker);
    VERIFY_THREAD_AFFINITY(ControlThread);
    VERIFY_INVOKER_AFFINITY(epochStateInvoker, StateThread);
    VERIFY_INVOKER_AFFINITY(epochControlInvoker, ControlThread);
}

TRecovery::TAsyncResult::TPtr TRecovery::RecoverFromSnapshotAndChangeLog(
    TMetaVersion targetVersion,
    i32 snapshotId)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    auto currentVersion = DecoratedState->GetVersion();

    LOG_INFO("Recovering state from %s to %s",
        ~currentVersion.ToString(),
        ~targetVersion.ToString());

    // Check if loading a snapshot is preferable.
    // Currently this is done by comparing segmentIds and is subject to further optimization.
    if (snapshotId != NonexistingSnapshotId && currentVersion.SegmentId < snapshotId)
    {
        // Load the snapshot.
        LOG_DEBUG("Using snapshot %d for recovery", snapshotId);

        auto readerResult = SnapshotStore->GetReader(snapshotId);
        if (!readerResult.IsOK()) {
            if (IsLeader()) {
                LOG_FATAL("Snapshot %d is not available\n%s",
                    snapshotId,
                    ~readerResult.ToString());
            }

            LOG_DEBUG("Snapshot cannot be found locally and will be downloaded");

            TSnapshotDownloader snapshotDownloader(
                ~Config->SnapshotDownloader,
                CellManager);

            auto snapshotWriter = SnapshotStore->GetRawWriter(snapshotId);

            auto downloadResult = snapshotDownloader.GetSnapshot(snapshotId, ~snapshotWriter);
            if (downloadResult != TSnapshotDownloader::EResult::OK) {
                LOG_ERROR("Error downloading snapshot %d\n%s",
                    snapshotId,
                    ~downloadResult.ToString());
                return New<TAsyncResult>(EResult::Failed);
            }

            SnapshotStore->UpdateMaxSnapshotId(snapshotId);

            readerResult = SnapshotStore->GetReader(snapshotId);
            if (!readerResult.IsOK()) {
                LOG_FATAL("Snapshot is not available\n%s", ~readerResult.ToString());
            }
        }

        auto snapshotReader = readerResult.Value();
        snapshotReader->Open();
        DecoratedState->Load(snapshotId, &snapshotReader->GetStream());

        // The reader reference is being held by the closure action.
        return RecoverFromChangeLog(    
            targetVersion,
            snapshotReader->GetPrevRecordCount());
    } else {
        // Recover using changelogs only.
        LOG_DEBUG("No snapshot is used for recovery");

        i32 prevRecordCount =
            currentVersion.SegmentId == 0
            ? NonexistingPrevRecordCount
            : UnknownPrevRecordCount;

        return RecoverFromChangeLog(targetVersion, prevRecordCount);
    }
}

TRecovery::TAsyncResult::TPtr TRecovery::RecoverFromChangeLog(
    TMetaVersion targetVersion,
    i32 expectedPrevRecordCount)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    // Iterate through the segments and apply the changelogs.
    for (i32 segmentId = DecoratedState->GetVersion().SegmentId;
         segmentId <= targetVersion.SegmentId;
         ++segmentId)
    {
        bool isFinal = segmentId == targetVersion.SegmentId;
        bool mayBeMissing = isFinal && targetVersion.RecordCount == 0 || !IsLeader();

        TCachedAsyncChangeLog::TPtr changeLog;
        auto changeLogResult = ChangeLogCache->Get(segmentId);
        if (!changeLogResult.IsOK()) {
            if (!mayBeMissing) {
                LOG_FATAL("Changelog %d is not available\n%s",
                    segmentId,
                    ~changeLogResult.ToString());
            }

            LOG_INFO("Changelog %d is missing and will be created", segmentId);
            YASSERT(expectedPrevRecordCount != UnknownPrevRecordCount);
            changeLog = ChangeLogCache->Create(segmentId, expectedPrevRecordCount);
        } else {
            changeLog = changeLogResult.Value();
        }

        LOG_DEBUG("Found changelog %d (RecordCount: %d, PrevRecordCount: %d, IsFinal: %s)",
            segmentId,
            changeLog->GetRecordCount(),
            changeLog->GetPrevRecordCount(),
            ~ToString(isFinal));

        if (expectedPrevRecordCount != UnknownPrevRecordCount &&
            changeLog->GetPrevRecordCount() != expectedPrevRecordCount)
        {
            LOG_FATAL("PrevRecordCount mismatch: expected: %d but found %d",
                expectedPrevRecordCount,
                changeLog->GetPrevRecordCount());
        }

        if (!IsLeader()) {
            auto request =
                CellManager->GetMasterProxy<TProxy>(LeaderId)
                ->GetChangeLogInfo()
                ->SetTimeout(Config->RpcTimeout);
            request->set_change_log_id(segmentId);

            auto response = request->Invoke()->Get();
            if (!response->IsOK()) {
                LOG_ERROR("Error getting changelog %d info from leader\n%s",
                    segmentId,
                    ~response->GetError().ToString());
                return New<TAsyncResult>(EResult::Failed);
            }

            i32 localRecordCount = changeLog->GetRecordCount();
            i32 remoteRecordCount = response->record_count();

            LOG_INFO("Changelog %d has %d local and %d remote records",
                segmentId,
                localRecordCount,
                remoteRecordCount);

            if (segmentId == targetVersion.SegmentId &&
                remoteRecordCount < targetVersion.RecordCount)
            {
                LOG_FATAL("Remote changelog %d has insufficient records",
                    segmentId);
            }

            if (localRecordCount > remoteRecordCount) {
                changeLog->Truncate(remoteRecordCount);
                changeLog->Finalize();
                LOG_INFO("Local changelog %d has %d records, truncated to %d records",
                    segmentId,
					localRecordCount,
                    remoteRecordCount);
            }

            auto currentVersion = DecoratedState->GetVersion();
            YASSERT(currentVersion.SegmentId <= segmentId);

            // Check if the current state contains some changes that are not present in the remote changelog.
            // If so, clear the state and restart recovery.
            if (currentVersion.SegmentId == segmentId && currentVersion.RecordCount > remoteRecordCount) {
                LOG_INFO("Current version is %s while only %d changes are expected, performing clear restart",
					~currentVersion.ToString(),
					remoteRecordCount);
                DecoratedState->Clear();
                return FromMethod(&TRecovery::Run, MakeStrong(this))
                        ->AsyncVia(~EpochControlInvoker)
                        ->Do();
            }

            // Do not download more than actually needed.
            int desiredRecordCount =
                segmentId == targetVersion.SegmentId
                ? targetVersion.RecordCount
                : remoteRecordCount;
            
            if (localRecordCount < desiredRecordCount) {
                TChangeLogDownloader changeLogDownloader(
                    ~Config->ChangeLogDownloader,
                    CellManager);
                auto changeLogResult = changeLogDownloader.Download(
                    TMetaVersion(segmentId, desiredRecordCount),
                    *changeLog);

                if (changeLogResult != TChangeLogDownloader::EResult::OK) {
                    LOG_ERROR("Error downloading changelog %d\n%s",
                        segmentId,
                        ~changeLogResult.ToString());
                    return New<TAsyncResult>(EResult::Failed);
                }
            }
        }

        if (!isFinal && !changeLog->IsFinalized()) {
            LOG_INFO("Finalizing an intermediate changelog %d", segmentId);
            changeLog->Finalize();
        }

        if (segmentId == targetVersion.SegmentId) {
            YASSERT(changeLog->GetRecordCount() == targetVersion.RecordCount);
        }

        ReplayChangeLog(*changeLog, changeLog->GetRecordCount());

        if (!isFinal) {
            DecoratedState->AdvanceSegment();
        }

        expectedPrevRecordCount = changeLog->GetRecordCount();
    }

    YASSERT(DecoratedState->GetVersion() == targetVersion);

    return New<TAsyncResult>(EResult::OK);
}

void TRecovery::ReplayChangeLog(
    TAsyncChangeLog& changeLog,
    i32 targetRecordCount)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    YASSERT(DecoratedState->GetVersion().SegmentId == changeLog.GetId());
    
    i32 startRecordId = DecoratedState->GetVersion().RecordCount;
    i32 recordCount = targetRecordCount - startRecordId;

    if (recordCount == 0)
        return;

    LOG_INFO("Reading records %d-%d from changelog %d",
        startRecordId,
        targetRecordCount - 1, 
        changeLog.GetId());

    yvector<TSharedRef> records;
    changeLog.Read(startRecordId, recordCount, &records);
    if (records.size() != recordCount) {
        LOG_FATAL("Not enough records in changelog %d: expected %d but found %d (StartRecordId: %d)",
            changeLog.GetId(),
            recordCount,
            records.ysize(),
            startRecordId);
    }

    LOG_INFO("Applying %d changes to meta state", recordCount);
    Profiler.Enqueue("replay_change_count", recordCount);

    PROFILE_TIMING ("replay_time") {
        FOREACH (const auto& changeData, records)  {
            auto version = DecoratedState->GetVersion();
            try {
                DecoratedState->ApplyChange(changeData);
            } catch (const std::exception& ex) {
                LOG_DEBUG("Failed to apply the change during recovery (Version: %s)\n%s",
                    ~version.ToString(),
                    ex.what());
            }
        }
    }

    LOG_INFO("Finished applying changes");
}

////////////////////////////////////////////////////////////////////////////////

TLeaderRecovery::TLeaderRecovery(
    TPersistentStateManagerConfig* config,
    TCellManager* cellManager,
    TDecoratedMetaState* decoratedState,
    TChangeLogCache* changeLogCache,
    TSnapshotStore* snapshotStore,
    const TEpoch& epoch,
    IInvoker* epochControlInvoker,
    IInvoker* epochStateInvoker)
    : TRecovery(
        config,
        cellManager,
        decoratedState,
        changeLogCache,
        snapshotStore,
        epoch,
        cellManager->GetSelfId(),
        epochControlInvoker,
        epochStateInvoker)
{ }

TRecovery::TAsyncResult::TPtr TLeaderRecovery::Run()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto version = DecoratedState->GetReachableVersionAsync();
    i32 maxAvailableSnapshotId = SnapshotStore->GetMaxSnapshotId();
    YASSERT(maxAvailableSnapshotId <= version.SegmentId);

    return FromMethod(
               &TRecovery::RecoverFromSnapshotAndChangeLog,
               MakeStrong(this),
               version,
               maxAvailableSnapshotId)
           ->AsyncVia(~EpochStateInvoker)
           ->Do();
}

bool TLeaderRecovery::IsLeader() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return true;
}

////////////////////////////////////////////////////////////////////////////////

TFollowerRecovery::TFollowerRecovery(
    TPersistentStateManagerConfig* config,
    TCellManager* cellManager,
    TDecoratedMetaState* decoratedState,
    TChangeLogCache* changeLogCache,
    TSnapshotStore* snapshotStore,
    const TEpoch& epoch,
    TPeerId leaderId,
    IInvoker* epochControlInvoker,
    IInvoker* epochStateInvoker,
    TMetaVersion targetVersion,
    i32 maxSnapshotId)
    : TRecovery(
        config,
        cellManager,
        decoratedState,
        changeLogCache,
        snapshotStore,
        epoch,
        leaderId,
        epochControlInvoker,
        epochStateInvoker)
    , Result(New<TAsyncResult>())
    , TargetVersion(targetVersion)
    , MaxSnapshotId(maxSnapshotId)
{ }

TRecovery::TAsyncResult::TPtr TFollowerRecovery::Run()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    PostponedVersion = TargetVersion;
    YASSERT(PostponedChanges.empty());

    FromMethod(
        &TRecovery::RecoverFromSnapshotAndChangeLog,
        MakeStrong(this),
        TargetVersion,
        MaxSnapshotId)
    ->AsyncVia(~EpochStateInvoker)
    ->Do()->Apply(FromMethod(
        &TFollowerRecovery::OnSyncReached,
        MakeStrong(this)))
    ->Subscribe(FromMethod(
        &TAsyncResult::Set,
        Result));

    return Result;
}

TRecovery::TAsyncResult::TPtr TFollowerRecovery::OnSyncReached(EResult result)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (result != EResult::OK) {
        return New<TAsyncResult>(result);
    }

    LOG_INFO("Sync reached");

    return FromMethod(&TFollowerRecovery::CapturePostponedChanges, MakeStrong(this))
           ->AsyncVia(~EpochControlInvoker)
           ->Do();
}

TRecovery::TAsyncResult::TPtr TFollowerRecovery::CapturePostponedChanges()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (PostponedChanges.empty()) {
        LOG_INFO("No postponed changes left");
        return New<TAsyncResult>(EResult::OK);
    }

    THolder<TPostponedChanges> changes(new TPostponedChanges());
    changes->swap(PostponedChanges);

    LOG_INFO("Captured %d postponed changes", changes->ysize());

    return FromMethod(
               &TFollowerRecovery::ApplyPostponedChanges,
               MakeStrong(this),
               changes)
           ->AsyncVia(~EpochStateInvoker)
           ->Do();
}

TRecovery::TAsyncResult::TPtr TFollowerRecovery::ApplyPostponedChanges(
    TAutoPtr<TPostponedChanges> changes)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    LOG_INFO("Applying %d postponed changes", changes->ysize());
    
    FOREACH(const auto& change, *changes) {
        switch (change.Type) {
            case TPostponedChange::EType::Change: {
                auto version = DecoratedState->GetVersion();
                DecoratedState->LogChange(version, change.ChangeData);
                try {
                    DecoratedState->ApplyChange(change.ChangeData);
                } catch (const std::exception& ex) {
                    LOG_DEBUG("Failed to apply the change during recovery at version %s\n%s",
                        ~version.ToString(),
                        ex.what());
                }
                break;
            }

            case TPostponedChange::EType::SegmentAdvance:
                DecoratedState->RotateChangeLog();
                break;

            default:
                YUNREACHABLE();
        }
    }
   
    LOG_INFO("Finished applying postponed changes");

    return FromMethod(
               &TFollowerRecovery::CapturePostponedChanges,
               MakeStrong(this))
           ->AsyncVia(~EpochControlInvoker)
           ->Do();
}

TRecovery::EResult TFollowerRecovery::PostponeSegmentAdvance(
    const TMetaVersion& version)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (PostponedVersion > version) {
        LOG_DEBUG("Late segment advance received during recovery, ignored: expected %s but received %s",
            ~PostponedVersion.ToString(),
            ~version.ToString());
        return EResult::OK;
    }

    if (PostponedVersion < version) {
        LOG_WARNING("Out-of-order segment advance received during recovery: expected %s but received %s",
            ~PostponedVersion.ToString(),
            ~version.ToString());
        return EResult::Failed;
    }

    PostponedChanges.push_back(TPostponedChange::CreateSegmentAdvance());
    
    LOG_DEBUG("Postponing segment advance at version %s", ~PostponedVersion.ToString());

    ++PostponedVersion.SegmentId;
    PostponedVersion.RecordCount = 0;
    
    return EResult::OK;
}

TRecovery::EResult TFollowerRecovery::PostponeChanges(
    const TMetaVersion& version,
    const yvector<TSharedRef>& changes)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (PostponedVersion > version) {
        LOG_WARNING("Late changes received during recovery, ignored: expected %s but received %s",
            ~PostponedVersion.ToString(),
            ~version.ToString());
        return EResult::OK;
    }

    if (PostponedVersion != version) {
        LOG_WARNING("Out-of-order changes received during recovery: expected %s but received %s",
            ~PostponedVersion.ToString(),
            ~version.ToString());
        return EResult::Failed;
    }

    LOG_DEBUG("Postponing %d changes at version %s",
        changes.ysize(),
        ~PostponedVersion.ToString());

    FOREACH (const auto& change, changes) {
        PostponedChanges.push_back(TPostponedChange::CreateChange(change));
    }
    
    PostponedVersion.RecordCount += changes.ysize();

    return EResult::OK;
}

bool TFollowerRecovery::IsLeader() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return false;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
