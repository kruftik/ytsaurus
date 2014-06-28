#include "stdafx.h"
#include "master_connector.h"
#include "private.h"
#include "config.h"
#include "location.h"
#include "block_store.h"
#include "chunk.h"
#include "chunk_store.h"
#include "chunk_cache.h"
#include "session_manager.h"

#include <core/rpc/client.h>

#include <core/concurrency/delayed_executor.h>

#include <core/misc/serialize.h>
#include <core/misc/string.h>

#include <ytlib/hydra/peer_channel.h>

#include <ytlib/hive/cell_directory.h>

#include <ytlib/node_tracker_client/node_statistics.h>
#include <ytlib/node_tracker_client/helpers.h>

#include <ytlib/api/connection.h>
#include <ytlib/api/client.h>

#include <server/job_agent/job_controller.h>

#include <server/tablet_node/tablet_slot_manager.h>
#include <server/tablet_node/tablet_slot.h>

#include <server/data_node/journal_dispatcher.h>

#include <server/cell_node/bootstrap.h>

#include <util/random/random.h>

namespace NYT {
namespace NDataNode {

using namespace NRpc;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NTabletNode;
using namespace NHydra;
using namespace NObjectClient;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TMasterConnector::TMasterConnector(TDataNodeConfigPtr config, TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , Started_(false)
    , ControlInvoker_(bootstrap->GetControlInvoker())
    , State_(EState::Offline)
    , NodeId_(InvalidNodeId)
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_, ControlThread);
    YCHECK(Config_);
    YCHECK(Bootstrap_);
}

void TMasterConnector::Start()
{
    YCHECK(!Started_);

    // Chunk store callbacks are always called in Control thread.
    Bootstrap_->GetChunkStore()->SubscribeChunkAdded(
        BIND(&TMasterConnector::OnChunkAdded, MakeWeak(this)));
    Bootstrap_->GetChunkStore()->SubscribeChunkRemoved(
        BIND(&TMasterConnector::OnChunkRemoved, MakeWeak(this)));

    Bootstrap_->GetChunkCache()->SubscribeChunkAdded(
        BIND(&TMasterConnector::OnChunkAdded, MakeWeak(this))
            .Via(ControlInvoker_));
    Bootstrap_->GetChunkCache()->SubscribeChunkRemoved(
        BIND(&TMasterConnector::OnChunkRemoved, MakeWeak(this))
            .Via(ControlInvoker_));

    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::StartHeartbeats, MakeStrong(this))
            .Via(ControlInvoker_),
        RandomDuration(Config_->IncrementalHeartbeatPeriod));

    Started_ = true;
}

void TMasterConnector::ForceRegister()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!Started_)
        return;

    ControlInvoker_->Invoke(BIND(
        &TMasterConnector::StartHeartbeats,
        MakeStrong(this)));
}

void TMasterConnector::StartHeartbeats()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    Reset();
    SendRegister();
}

bool TMasterConnector::IsConnected() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NodeId_ != InvalidNodeId;
}

TNodeId TMasterConnector::GetNodeId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NodeId_;
}

void TMasterConnector::RegisterAlert(const Stroka& alert)
{
    VERIFY_THREAD_AFFINITY_ANY();
    
    TGuard<TSpinLock> guard(AlertsSpinLock_);
    Alerts_.push_back(alert);
}

void TMasterConnector::ScheduleNodeHeartbeat()
{
    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::OnNodeHeartbeat, MakeStrong(this))
            .Via(HeartbeatInvoker_),
        Config_->IncrementalHeartbeatPeriod);
}

void TMasterConnector::ScheduleJobHeartbeat()
{
    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::OnJobHeartbeat, MakeStrong(this))
            .Via(HeartbeatInvoker_),
        Config_->IncrementalHeartbeatPeriod);
}

void TMasterConnector::ResetAndScheduleRegister()
{
    Reset();

    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::SendRegister, MakeStrong(this))
            .Via(HeartbeatInvoker_),
        Config_->IncrementalHeartbeatPeriod);
}

void TMasterConnector::OnNodeHeartbeat()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    switch (State_) {
        case EState::Registered:
            SendFullNodeHeartbeat();
            break;
        case EState::Online:
            SendIncrementalNodeHeartbeat();
            break;
        default:
            YUNREACHABLE();
    }
}

void TMasterConnector::SendRegister()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    TNodeTrackerServiceProxy proxy(Bootstrap_->GetMasterClient()->GetMasterChannel());
    auto req = proxy.RegisterNode();
    *req->mutable_statistics() = ComputeStatistics();
    ToProto(req->mutable_node_descriptor(), Bootstrap_->GetLocalDescriptor());
    ToProto(req->mutable_cell_guid(), Bootstrap_->GetCellGuid());
    req->Invoke().Subscribe(
        BIND(&TMasterConnector::OnRegisterResponse, MakeStrong(this))
            .Via(HeartbeatInvoker_));

    State_ = EState::Registering;

    LOG_INFO("Node register request sent to master (%s)",
        ~ToString(*req->mutable_statistics()));
}

TNodeStatistics TMasterConnector::ComputeStatistics()
{
    TNodeStatistics result;

    i64 totalAvailableSpace = 0;
    i64 totalUsedSpace = 0;
    int totalChunkCount = 0;
    int totalSessionCount = 0;
    bool full = true;

    auto chunkStore = Bootstrap_->GetChunkStore();
    for (auto location : chunkStore->Locations()) {
        auto* locationStatistics = result.add_locations();

        locationStatistics->set_available_space(location->GetAvailableSpace());
        locationStatistics->set_used_space(location->GetUsedSpace());
        locationStatistics->set_chunk_count(location->GetChunkCount());
        locationStatistics->set_session_count(location->GetSessionCount());
        locationStatistics->set_full(location->IsFull());
        locationStatistics->set_enabled(location->IsEnabled());

        if (location->IsEnabled()) {
            totalAvailableSpace += location->GetAvailableSpace();
            full &= location->IsFull();
        }

        totalUsedSpace += location->GetUsedSpace();
        totalChunkCount += location->GetChunkCount();
        totalSessionCount += location->GetSessionCount();
    }

    result.set_total_available_space(totalAvailableSpace);
    result.set_total_used_space(totalUsedSpace);
    result.set_total_chunk_count(totalChunkCount);
    result.set_full(full);

    auto sessionManager = Bootstrap_->GetSessionManager();
    result.set_total_user_session_count(sessionManager->GetSessionCount(EWriteSessionType::User));
    result.set_total_replication_session_count(sessionManager->GetSessionCount(EWriteSessionType::Replication));
    result.set_total_repair_session_count(sessionManager->GetSessionCount(EWriteSessionType::Repair));

    auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
    result.set_available_tablet_slots(tabletSlotManager->GetAvailableTabletSlotCount());
    result.set_used_tablet_slots(tabletSlotManager->GetUsedTableSlotCount());
    
    result.add_accepted_chunk_types(EObjectType::Chunk);
    result.add_accepted_chunk_types(EObjectType::ErasureChunk);
    
    auto journalDispatcher = Bootstrap_->GetJournalDispatcher();
    if (journalDispatcher->AcceptsChunks()) {
        result.add_accepted_chunk_types(EObjectType::JournalChunk);
    }

    return result;
}

void TMasterConnector::OnRegisterResponse(TNodeTrackerServiceProxy::TRspRegisterNodePtr rsp)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!rsp->IsOK()) {
        LOG_WARNING(*rsp, "Error registering node");
        ResetAndScheduleRegister();
        return;
    }

    auto cellGuid = FromProto<TGuid>(rsp->cell_guid());
    YCHECK(cellGuid == Bootstrap_->GetCellGuid());

    NodeId_ = rsp->node_id();
    YCHECK(State_ == EState::Registering);
    State_ = EState::Registered;

    LOG_INFO("Successfully registered node at master (NodeId: %d)",
        NodeId_);

    SendFullNodeHeartbeat();
}

void TMasterConnector::SendFullNodeHeartbeat()
{
    TNodeTrackerServiceProxy proxy(Bootstrap_->GetMasterClient()->GetMasterChannel());
    auto request = proxy.FullHeartbeat()
        ->SetCodec(NCompression::ECodec::Lz4)
        ->SetTimeout(Config_->FullHeartbeatTimeout);

    YCHECK(NodeId_ != InvalidNodeId);
    request->set_node_id(NodeId_);

    *request->mutable_statistics() = ComputeStatistics();

    for (const auto& chunk : Bootstrap_->GetChunkStore()->GetChunks()) {
        *request->add_chunks() = BuildAddChunkInfo(chunk);
    }

    for (const auto& chunk : Bootstrap_->GetChunkCache()->GetChunks()) {
        *request->add_chunks() = BuildAddChunkInfo(chunk);
    }

    AddedSinceLastSuccess_.clear();
    RemovedSinceLastSuccess_.clear();

    request->Invoke().Subscribe(
        BIND(&TMasterConnector::OnFullNodeHeartbeatResponse, MakeStrong(this))
            .Via(HeartbeatInvoker_));

    LOG_INFO("Full node heartbeat sent to master (%s)", ~ToString(request->statistics()));
}

void TMasterConnector::SendIncrementalNodeHeartbeat()
{
    TNodeTrackerServiceProxy proxy(Bootstrap_->GetMasterClient()->GetMasterChannel());
    auto request = proxy.IncrementalHeartbeat()
        ->SetCodec(NCompression::ECodec::Lz4);

    YCHECK(NodeId_ != InvalidNodeId);
    request->set_node_id(NodeId_);

    *request->mutable_statistics() = ComputeStatistics();

    {
        TGuard<TSpinLock> guard(AlertsSpinLock_);
        ToProto(request->mutable_alerts(), Alerts_);
    }

    ReportedAdded_ = AddedSinceLastSuccess_;
    ReportedRemoved_ = RemovedSinceLastSuccess_;

    for (auto chunk : ReportedAdded_) {
        *request->add_added_chunks() = BuildAddChunkInfo(chunk);
    }

    for (auto chunk : ReportedRemoved_) {
        *request->add_removed_chunks() = BuildRemoveChunkInfo(chunk);
    }

    auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
    for (auto slot : tabletSlotManager->Slots()) {
        auto* info = request->add_tablet_slots();
        ToProto(info->mutable_cell_guid(), slot->GetCellGuid());
        info->set_peer_state(slot->GetControlState());
        info->set_peer_id(slot->GetPeerId());
        info->set_config_version(slot->GetCellConfig().version());
    }

    auto cellDirectory = Bootstrap_->GetMasterClient()->GetConnection()->GetCellDirectory();
    auto cellMap = cellDirectory->GetRegisteredCells();
    for (const auto& pair : cellMap) {
        auto* info = request->add_hive_cells();
        ToProto(info->mutable_cell_guid(), pair.first);
        info->set_config_version(pair.second.version());
    }

    request->Invoke().Subscribe(
        BIND(&TMasterConnector::OnIncrementalNodeHeartbeatResponse, MakeStrong(this))
            .Via(HeartbeatInvoker_));

    LOG_INFO("Incremental node heartbeat sent to master (%s, AddedChunks: %d, RemovedChunks: %d)",
        ~ToString(request->statistics()),
        static_cast<int>(request->added_chunks_size()),
        static_cast<int>(request->removed_chunks_size()));
}

TChunkAddInfo TMasterConnector::BuildAddChunkInfo(IChunkPtr chunk)
{
    TChunkAddInfo result;
    ToProto(result.mutable_chunk_id(), chunk->GetId());
    *result.mutable_chunk_info() = chunk->GetInfo();
    result.set_cached(chunk->GetLocation()->GetType() == ELocationType::Cache);
    result.set_active(chunk->IsActive());
    return result;
}

TChunkRemoveInfo TMasterConnector::BuildRemoveChunkInfo(IChunkPtr chunk)
{
    TChunkRemoveInfo result;
    ToProto(result.mutable_chunk_id(), chunk->GetId());
    result.set_cached(chunk->GetLocation()->GetType() == ELocationType::Cache);
    return result;
}

void TMasterConnector::OnFullNodeHeartbeatResponse(TNodeTrackerServiceProxy::TRspFullHeartbeatPtr rsp)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!rsp->IsOK()) {
        auto error = rsp->GetError();
        LOG_WARNING(error, "Error reporting full node heartbeat to master");
        if (IsRetriableError(error)) {
            ScheduleNodeHeartbeat();
        } else {
            ResetAndScheduleRegister();
        }
        return;
    }

    LOG_INFO("Successfully reported full node heartbeat to master");

    // Schedule another full heartbeat.
    if (Config_->FullHeartbeatPeriod) {
        TDelayedExecutor::Submit(
            BIND(&TMasterConnector::StartHeartbeats, MakeStrong(this))
                .Via(HeartbeatInvoker_),
            RandomDuration(*Config_->FullHeartbeatPeriod));
    }

    State_ = EState::Online;

    SendJobHeartbeat();
    ScheduleNodeHeartbeat();
}

void TMasterConnector::OnIncrementalNodeHeartbeatResponse(TNodeTrackerServiceProxy::TRspIncrementalHeartbeatPtr rsp)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!rsp->IsOK()) {
        auto error = rsp->GetError();
        LOG_WARNING(error, "Error reporting incremental node heartbeat to master");
        if (IsRetriableError(error)) {
            ScheduleNodeHeartbeat();
        } else {
            ResetAndScheduleRegister();
        }
        return;
    }

    LOG_INFO("Successfully reported incremental node heartbeat to master");

    TChunkSet newAddedSinceLastSuccess;
    for (const auto& id : AddedSinceLastSuccess_) {
        if (ReportedAdded_.find(id) == ReportedAdded_.end()) {
            newAddedSinceLastSuccess.insert(id);
        }
    }
    AddedSinceLastSuccess_.swap(newAddedSinceLastSuccess);

    TChunkSet newRemovedSinceLastSuccess;
    for (const auto& id : RemovedSinceLastSuccess_) {
        if (ReportedRemoved_.find(id) == ReportedRemoved_.end()) {
            newRemovedSinceLastSuccess.insert(id);
        }
    }
    RemovedSinceLastSuccess_.swap(newRemovedSinceLastSuccess);

    auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
    
    for (const auto& info : rsp->tablet_slots_to_remove()) {
        auto cellGuid = FromProto<TCellGuid>(info.cell_guid());
        YCHECK(cellGuid != NullCellGuid);
        auto slot = tabletSlotManager->FindSlot(cellGuid);
        if (!slot) {
            LOG_WARNING("Requested to remove a non-existing slot %s, ignored",
                ~ToString(cellGuid));
            continue;
        }
        tabletSlotManager->RemoveSlot(slot);
    }

    for (const auto& info : rsp->tablet_slots_to_create()) {
        auto cellGuid = FromProto<TCellGuid>(info.cell_guid());
        YCHECK(cellGuid != NullCellGuid);
        if (tabletSlotManager->GetAvailableTabletSlotCount() == 0) {
            LOG_WARNING("Requested to start cell %s when all slots are used, ignored",
                ~ToString(cellGuid));
            continue;
        }
        if (tabletSlotManager->FindSlot(cellGuid)) {
            LOG_WARNING("Requested to start cell %s when this cell is already being served by the node, ignored",
                ~ToString(cellGuid));
            continue;
        }
        tabletSlotManager->CreateSlot(info);
    }

    for (const auto& info : rsp->tablet_slots_configure()) {
        auto cellGuid = FromProto<TCellGuid>(info.cell_guid());
        YCHECK(cellGuid != NullCellGuid);
        auto slot = tabletSlotManager->FindSlot(cellGuid);
        if (!slot) {
            LOG_WARNING("Requested to configure a non-existing slot %s, ignored",
                ~ToString(cellGuid));
            continue;
        }
        tabletSlotManager->ConfigureSlot(slot, info);
    }

    auto cellDirectory = Bootstrap_->GetMasterClient()->GetConnection()->GetCellDirectory();

    for (const auto& info : rsp->hive_cells_to_unregister()) {
        auto cellGuid = FromProto<TCellGuid>(info.cell_guid());
        if (cellDirectory->UnregisterCell(cellGuid)) {
            LOG_DEBUG("Hive cell unregistered (CellGuid: %s)",
                ~ToString(cellGuid));
        }
    }

    for (const auto& info : rsp->hive_cells_to_reconfigure()) {
        auto cellGuid = FromProto<TCellGuid>(info.cell_guid());
        if (cellDirectory->RegisterCell(cellGuid, info.config())) {
            LOG_DEBUG("Hive cell reconfigured (CellGuid: %s, ConfigVersion: %d)",
                ~ToString(cellGuid),
                info.config().version());
        }
    }

    ScheduleNodeHeartbeat();
}

void TMasterConnector::OnJobHeartbeat()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    
    SendJobHeartbeat();
}

void TMasterConnector::SendJobHeartbeat()
{
    YCHECK(NodeId_ != InvalidNodeId);
    YCHECK(State_ == EState::Online);

    TJobTrackerServiceProxy proxy(Bootstrap_->GetMasterClient()->GetMasterChannel());
    auto req = proxy.Heartbeat();

    auto jobController = Bootstrap_->GetJobController();
    jobController->PrepareHeartbeat(req.Get());

    req->Invoke().Subscribe(
        BIND(&TMasterConnector::OnJobHeartbeatResponse, MakeStrong(this))
            .Via(HeartbeatInvoker_));

    LOG_INFO("Job heartbeat sent to master (ResourceUsage: {%s})",
        ~FormatResourceUsage(req->resource_usage(), req->resource_limits()));
}

void TMasterConnector::OnJobHeartbeatResponse(TJobTrackerServiceProxy::TRspHeartbeatPtr rsp)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!rsp->IsOK()) {
        auto error = rsp->GetError();
        LOG_WARNING(error, "Error reporting job heartbeat to master");
        if (IsRetriableError(error)) {
            ScheduleJobHeartbeat();
        } else {
            ResetAndScheduleRegister();
        }
        return;
    }

    LOG_INFO("Successfully reported job heartbeat to master");
    
    auto jobController = Bootstrap_->GetJobController();
    jobController->ProcessHeartbeat(rsp.Get());

    ScheduleJobHeartbeat();
}

void TMasterConnector::Reset()
{
    if (HeartbeatContext_) {
        HeartbeatContext_->Cancel();
    }

    HeartbeatContext_ = New<TCancelableContext>();
    HeartbeatInvoker_ = HeartbeatContext_->CreateInvoker(ControlInvoker_);

    State_ = EState::Offline;
    NodeId_ = InvalidNodeId;

    ReportedAdded_.clear();
    ReportedRemoved_.clear();
    AddedSinceLastSuccess_.clear();
    RemovedSinceLastSuccess_.clear();
}

void TMasterConnector::OnChunkAdded(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (State_ == EState::Offline)
        return;

    RemovedSinceLastSuccess_.erase(chunk);
    AddedSinceLastSuccess_.insert(chunk);

    LOG_DEBUG("Chunk addition registered (ChunkId: %s)",
        ~ToString(chunk->GetId()));
}

void TMasterConnector::OnChunkRemoved(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (State_ == EState::Offline)
        return;

    AddedSinceLastSuccess_.erase(chunk);
    RemovedSinceLastSuccess_.insert(chunk);

    LOG_DEBUG("Chunk removal registered (ChunkId: %s)",
        ~ToString(chunk->GetId()));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
