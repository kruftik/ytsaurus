#include "stdafx.h"
#include "chunk_manager.h"
#include "config.h"
#include "holder.h"
#include "chunk.h"
#include "chunk_list.h"
#include "job.h"
#include "job_list.h"
#include "chunk_placement.h"
#include "chunk_replicator.h"
#include "holder_lease_tracker.h"
#include "holder_statistics.h"
#include "chunk_service_proxy.h"
#include "holder_authority.h"
#include "holder_statistics.h"
#include "chunk_tree_balancer.h"

#include <ytlib/misc/foreach.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/guid.h>
#include <ytlib/misc/id_generator.h>
#include <ytlib/misc/address.h>
#include <ytlib/misc/string.h>
#include <ytlib/misc/codec.h>

#include <ytlib/chunk_holder/chunk_meta_extensions.h>

#include <ytlib/chunk_server/chunk_manager.pb.h>
#include <ytlib/chunk_server/chunk_ypath.pb.h>
#include <ytlib/chunk_server/chunk_list_ypath.pb.h>
#include <ytlib/chunk_server/chunk_manager.pb.h>

#include <ytlib/cypress_server/cypress_manager.h>

#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/table_client/schema.h>
#include <ytlib/table_server/table_ypath.pb.h>

#include <ytlib/cell_master/load_context.h>
#include <ytlib/cell_master/bootstrap.h>

#include <ytlib/transaction_server/transaction_manager.h>
#include <ytlib/transaction_server/transaction.h>

#include <ytlib/meta_state/meta_state_manager.h>
#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/map.h>

#include <ytlib/object_server/type_handler_detail.h>

#include <ytlib/ytree/fluent.h>

#include <ytlib/logging/log.h>

namespace NYT {
namespace NChunkServer {

using namespace NProto;
using namespace NMetaState;
using namespace NTransactionServer;
using namespace NObjectServer;
using namespace NYTree;
using namespace NCellMaster;
using namespace NChunkHolder::NProto;
using namespace NCypressServer;
using ::ToString;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkTypeHandler
    : public TObjectTypeHandlerBase<TChunk>
{
public:
    explicit TChunkTypeHandler(TImpl* owner);

    virtual EObjectType GetType()
    {
        return EObjectType::Chunk;
    }

    virtual TObjectId Create(
        TTransaction* transaction,
        TReqCreateObject* request,
        TRspCreateObject* response);

    virtual IObjectProxyPtr GetProxy(
        const TObjectId& id,
        TTransaction* transaction);

private:
    TImpl* Owner;

    void OnObjectDestroyed(TChunk* chunk) OVERRIDE;

};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkListTypeHandler
    : public TObjectTypeHandlerBase<TChunkList>
{
public:
    explicit TChunkListTypeHandler(TImpl* owner);

    virtual EObjectType GetType()
    {
        return EObjectType::ChunkList;
    }

    virtual TObjectId Create(
        TTransaction* transaction,
        TReqCreateObject* request,
        TRspCreateObject* response);

    virtual IObjectProxyPtr GetProxy(
        const TObjectId& id,
        TTransaction* transaction);

private:
    TImpl* Owner;

    void OnObjectDestroyed(TChunkList* chunkList) OVERRIDE;
};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TImpl
    : public NMetaState::TMetaStatePart
{
public:
    TImpl(
        TChunkManagerConfigPtr config,
        TBootstrap* bootstrap)
        : TMetaStatePart(
            bootstrap->GetMetaStateManager(),
            bootstrap->GetMetaState())
        , Config(config)
        , Bootstrap(bootstrap)
        , ChunkTreeBalancer(Bootstrap, Config->ChunkTreeBalancer)
        , ChunkReplicaCount(0)
        , Profiler(ChunkServerProfiler)
        , AddChunkCounter("/add_chunk_rate")
        , RemoveChunkCounter("/remove_chunk_rate")
        , AddChunkReplicaCounter("/add_chunk_replica_rate")
        , RemoveChunkReplicaCounter("/remove_chunk_replica_rate")
        , StartJobCounter("/start_job_rate")
        , StopJobCounter("/stop_job_rate")
    {
        YCHECK(config);
        YCHECK(bootstrap);

        RegisterMethod(BIND(&TImpl::FullHeartbeat, Unretained(this)));
        RegisterMethod(BIND(&TImpl::IncrementalHeartbeat, Unretained(this)));
        RegisterMethod(BIND(&TImpl::UpdateJobs, Unretained(this)));
        RegisterMethod(BIND(&TImpl::RegisterHolder, Unretained(this)));
        RegisterMethod(BIND(&TImpl::UnregisterHolder, Unretained(this)));
        RegisterMethod(BIND(&TImpl::RebalanceChunkTree, Unretained(this)));

        TLoadContext context(bootstrap);

        auto metaState = bootstrap->GetMetaState();
        metaState->RegisterLoader(
            "ChunkManager.Keys.1",
            BIND(&TImpl::LoadKeys, MakeStrong(this)));
        metaState->RegisterLoader(
            "ChunkManager.Values.1",
            BIND(&TImpl::LoadValues, MakeStrong(this), context));
        metaState->RegisterSaver(
            "ChunkManager.Keys.1",
            BIND(&TImpl::SaveKeys, MakeStrong(this)),
            ESavePhase::Keys);
        metaState->RegisterSaver(
            "ChunkManager.Values.1",
            BIND(&TImpl::SaveValues, MakeStrong(this)),
            ESavePhase::Values);

        metaState->RegisterPart(this);

        auto objectManager = bootstrap->GetObjectManager();
        objectManager->RegisterHandler(New<TChunkTypeHandler>(this));
        objectManager->RegisterHandler(New<TChunkListTypeHandler>(this));
    }

    TMutation<THolderId>::TPtr InitiateRegisterHolder(
        const TMsgRegisterHolder& message)
    {
        return CreateMutation(
            MetaStateManager,
            message,
            BIND(&TThis::RegisterHolder, Unretained(this), message));
    }

    TMutation<TVoid>::TPtr InitiateUnregisterHolder(
        const TMsgUnregisterHolder& message)
    {
        return CreateMutation(
            MetaStateManager,
            message,
            BIND(&TThis::UnregisterHolder, Unretained(this), message));
    }

    TMutation<TVoid>::TPtr InitiateFullHeartbeat(
        TCtxFullHeartbeat::TPtr context)
    {
        return CreateMutation(
            MetaStateManager,
            context->Request(),
            context->GetUntypedContext()->GetRequestBody(),
            BIND(&TThis::FullHeartbeatWithContext, Unretained(this), context));
    }

    TMutation<TVoid>::TPtr InitiateIncrementalHeartbeat(
        const TMsgIncrementalHeartbeat& message)
    {
        return CreateMutation(
            MetaStateManager,
            message,
            BIND(&TThis::IncrementalHeartbeat, Unretained(this), message));
    }

    TMutation<TVoid>::TPtr InitiateUpdateJobs(
        const TMsgUpdateJobs& message)
    {
        return CreateMutation(
            MetaStateManager,
            message,
            BIND(&TThis::UpdateJobs, Unretained(this), message));
    }

    TMutation<TVoid>::TPtr InitiateRebalanceChunkTree(
        const TMsgRebalanceChunkTree& message)
    {
        return CreateMutation(
            MetaStateManager,
            message,
            BIND(&TThis::RebalanceChunkTree, Unretained(this), message));
    }


    DECLARE_METAMAP_ACCESSORS(Chunk, TChunk, TChunkId);
    DECLARE_METAMAP_ACCESSORS(ChunkList, TChunkList, TChunkListId);
    DECLARE_METAMAP_ACCESSORS(Holder, THolder, THolderId);
    DECLARE_METAMAP_ACCESSORS(JobList, TJobList, TChunkId);
    DECLARE_METAMAP_ACCESSORS(Job, TJob, TJobId);

    DEFINE_SIGNAL(void(const THolder*), HolderRegistered);
    DEFINE_SIGNAL(void(const THolder*), HolderUnregistered);


    THolder* FindHolderByAddresss(const Stroka& address)
    {
        auto it = HolderAddressMap.find(address);
        return it == HolderAddressMap.end() ? NULL : it->second;
    }

    THolder* FindHolderByHostName(const Stroka& hostName)
    {
        auto it = HolderHostNameMap.find(hostName);
        return it == HolderAddressMap.end() ? NULL : it->second;
    }

    const TReplicationSink* FindReplicationSink(const Stroka& address)
    {
        auto it = ReplicationSinkMap.find(address);
        return it == ReplicationSinkMap.end() ? NULL : &it->second;
    }

    std::vector<THolder*> AllocateUploadTargets(
        int nodeCount,
        TNullable<Stroka> preferredHostName)
    {
        auto holders = ChunkPlacement->GetUploadTargets(
            nodeCount,
            NULL,
            preferredHostName.GetPtr());
        FOREACH (auto holder, holders) {
            ChunkPlacement->OnSessionHinted(holder);
        }
        return holders;
    }

    TChunk* CreateChunk()
    {
        Profiler.Increment(AddChunkCounter);
        auto id = Bootstrap->GetObjectManager()->GenerateId(EObjectType::Chunk);
        auto* chunk = new TChunk(id);
        ChunkMap.Insert(id, chunk);
        return chunk;
    }

    TChunkList* CreateChunkList()
    {
        auto id = Bootstrap->GetObjectManager()->GenerateId(EObjectType::ChunkList);
        auto* chunkList = new TChunkList(id);
        ChunkListMap.Insert(id, chunkList);
        return chunkList;
    }


    void AttachToChunkList(
        TChunkList* chunkList,
        const TChunkTreeRef* childrenBegin,
        const TChunkTreeRef* childrenEnd)
    {
        auto objectManager = Bootstrap->GetObjectManager();

        TChunkTreeStatistics accumulatedDelta;
        for (auto it = childrenBegin; it != childrenEnd; ++it) {
            auto childRef = *it;
            if (!chunkList->Children().empty()) {
                chunkList->RowCountSums().push_back(
                    chunkList->Statistics().RowCount +
                    accumulatedDelta.RowCount);
            }
            chunkList->Children().push_back(childRef);
            SetChunkTreeParent(chunkList, childRef);
            objectManager->RefObject(childRef.GetId());

            TChunkTreeStatistics delta;
            switch (childRef.GetType()) {
                case EObjectType::Chunk:
                    delta = childRef.AsChunk()->GetStatistics();
                    break;
                case EObjectType::ChunkList:
                    delta = childRef.AsChunkList()->Statistics();
                    break;
                default:
                    YUNREACHABLE();
            }
            accumulatedDelta.Accumulate(delta);
        }

        UpdateStatistics(chunkList, accumulatedDelta);
        ScheduleChunkTreeRebalanceIfNeeded(chunkList);
    }

    void ScheduleChunkTreeRebalanceIfNeeded(TChunkList* chunkList)
    {
        TMsgRebalanceChunkTree message;
        if (IsLeader() &&
            ChunkTreeBalancer.CheckRebalanceNeeded(chunkList, &message))
        {
            // Don't retry in case of failure.
            // Balancing will happen eventually.
            InitiateRebalanceChunkTree(message)->PostCommit();
        }
    }

    void AttachToChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTreeRef>& children)
    {
        AttachToChunkList(
            chunkList,
            &*children.begin(),
            &*children.begin() + children.size());
    }

    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTreeRef childRef)
    {
        AttachToChunkList(chunkList, &childRef, &childRef + 1);
    }


    void ClearChunkList(TChunkList* chunkList)
    {
        // TODO(babenko): currently we only support clearing a chunklist with no parents.
        YCHECK(chunkList->Parents().empty());

        auto objectManager = Bootstrap->GetObjectManager();
        FOREACH (auto childRef, chunkList->Children()) {
            ResetChunkTreeParent(chunkList, childRef);
            objectManager->UnrefObject(childRef.GetId());
        }
        chunkList->Children().clear();
        chunkList->RowCountSums().clear();
        chunkList->Statistics() = TChunkTreeStatistics();

        // TODO(babenko): IsRecovery
        LOG_DEBUG("Chunk list cleared (ChunkListId: %s)", ~chunkList->GetId().ToString());
    }

    void SetChunkTreeParent(TChunkList* parent, TChunkTreeRef childRef)
    {
        switch (childRef.GetType()) {
            case EObjectType::Chunk:
                childRef.AsChunk()->Parents().push_back(parent);
                break;
            case EObjectType::ChunkList:
                childRef.AsChunkList()->Parents().insert(parent);
                break;
            default:
                YUNREACHABLE();
        }
    }

    void ResetChunkTreeParent(TChunkList* parent, TChunkTreeRef childRef)
    {
        switch (childRef.GetType()) {
            case EObjectType::Chunk: {
                auto& parents = childRef.AsChunk()->Parents();
                auto it = std::find(parents.begin(), parents.end(), parent);
                YASSERT(it != parents.end());
                parents.erase(it);
                break;
            }
            case EObjectType::ChunkList: {
                auto& parents = childRef.AsChunkList()->Parents();
                auto it = parents.find(parent);
                YASSERT(it != parents.end());
                parents.erase(it);
                break;
            }
            default:
                YUNREACHABLE();
        }
    }



    void ScheduleJobs(
        THolder* holder,
        const std::vector<TJobInfo>& runningJobs,
        std::vector<TJobStartInfo>* jobsToStart,
        std::vector<TJobStopInfo>* jobsToStop)
    {
        ChunkReplicator->ScheduleJobs(
            holder,
            runningJobs,
            jobsToStart,
            jobsToStop);
    }

    const yhash_set<TChunkId>& LostChunkIds() const;
    const yhash_set<TChunkId>& OverreplicatedChunkIds() const;
    const yhash_set<TChunkId>& UnderreplicatedChunkIds() const;

    void FillHolderAddresses(
        ::google::protobuf::RepeatedPtrField<TProtoStringType>* addresses,
        const TChunk* chunk)
    {
        FOREACH (auto holderId, chunk->StoredLocations()) {
            const auto* holder = GetHolder(holderId);
            addresses->Add()->assign(holder->GetAddress());
        }

        if (~chunk->CachedLocations()) {
            FOREACH (auto holderId, *chunk->CachedLocations()) {
                const auto* holder = GetHolder(holderId);
                addresses->Add()->assign(holder->GetAddress());
            }
        }
    }

    TTotalHolderStatistics GetTotalHolderStatistics()
    {
        TTotalHolderStatistics result;
        auto keys = HolderMap.GetKeys();
        FOREACH (const auto& key, keys) {
            const auto* holder = HolderMap.Get(key);
            const auto& statistics = holder->Statistics();
            result.AvailbaleSpace += statistics.available_space();
            result.UsedSpace += statistics.used_space();
            result.ChunkCount += statistics.chunk_count();
            result.SessionCount += statistics.session_count();
            result.OnlineHolderCount++;
        }
        return result;
    }

    bool IsHolderConfirmed(const THolder* holder)
    {
        return HolderLeaseTracker->IsHolderConfirmed(holder);
    }

    i32 GetChunkReplicaCount()
    {
        return ChunkReplicaCount;
    }

    bool IsReplicatorEnabled()
    {
        return ChunkReplicator->IsEnabled();
    }

    TChunkTreeRef GetChunkTree(const TChunkTreeId& id)
    {
        auto type = TypeFromId(id);
        if (type == EObjectType::Chunk) {
            auto* chunk = FindChunk(id);
            if (!chunk) {
                ythrow yexception() << Sprintf("No such chunk %s", ~id.ToString());
            }
            return TChunkTreeRef(chunk);
        } else if (type == EObjectType::ChunkList) {
            auto* chunkList = FindChunkList(id);
            if (!chunkList) {
                ythrow yexception() << Sprintf("No such chunkList %s", ~id.ToString());
            }
            return TChunkTreeRef(chunkList);
        } else {
            ythrow yexception() << Sprintf("Invalid type of object %s", ~id.ToString());
        }
    }


private:
    typedef TImpl TThis;
    friend class TChunkTypeHandler;
    friend class TChunkProxy;
    friend class TChunkListTypeHandler;
    friend class TChunkListProxy;

    TChunkManagerConfigPtr Config;
    TBootstrap* Bootstrap;

    TChunkTreeBalancer ChunkTreeBalancer;
    
    i32 ChunkReplicaCount;

    NProfiling::TProfiler& Profiler;
    NProfiling::TRateCounter AddChunkCounter;
    NProfiling::TRateCounter RemoveChunkCounter;
    NProfiling::TRateCounter AddChunkReplicaCounter;
    NProfiling::TRateCounter RemoveChunkReplicaCounter;
    NProfiling::TRateCounter StartJobCounter;
    NProfiling::TRateCounter StopJobCounter;

    TChunkPlacementPtr ChunkPlacement;
    TChunkReplicatorPtr ChunkReplicator;
    THolderLeaseTrackerPtr HolderLeaseTracker;
    
    TIdGenerator<THolderId> HolderIdGenerator;

    TMetaStateMap<TChunkId, TChunk> ChunkMap;
    TMetaStateMap<TChunkListId, TChunkList> ChunkListMap;

    TMetaStateMap<THolderId, THolder> HolderMap;
    yhash_map<Stroka, THolder*> HolderAddressMap;
    yhash_multimap<Stroka, THolder*> HolderHostNameMap;

    TMetaStateMap<TChunkId, TJobList> JobListMap;
    TMetaStateMap<TJobId, TJob> JobMap;

    yhash_map<Stroka, TReplicationSink> ReplicationSinkMap;


    void UpdateStatistics(TChunkList* chunkList, const TChunkTreeStatistics& statisticsDelta)
    {
        // Go upwards and apply delta.
        // Also reset Sorted flags.
        // Check that parents are unique along the way.
        auto statisticsCopy = statisticsDelta;
        while (true) {
            ++statisticsCopy.Rank;

            chunkList->Statistics().Accumulate(statisticsCopy);
            chunkList->ResetSorted();

            const auto& parents = chunkList->Parents();
            if (parents.empty()) {
                break;
            }

            YCHECK(parents.size() == 1);
            chunkList = *parents.begin();
        }
    }


    void OnChunkDestroyed(TChunk* chunk)
    {
        auto chunkId = chunk->GetId();

        // Unregister chunk replicas from all known locations.
        FOREACH (auto holderId, chunk->StoredLocations()) {
            ScheduleChunkReplicaRemoval(holderId, chunk, false);
        }
        if (~chunk->CachedLocations()) {
            FOREACH (auto holderId, *chunk->CachedLocations()) {
                ScheduleChunkReplicaRemoval(holderId, chunk, true);
            }
        }

        // Remove all associated jobs.
        auto* jobList = FindJobList(chunkId);
        if (jobList) {
            FOREACH (auto job, jobList->Jobs()) {
                // Suppress removal from job list.
                RemoveJob(job, true, false);
            }
            JobListMap.Remove(chunkId);
        }

        // Notify the balancer about chunk's death.
        if (ChunkReplicator) {
            ChunkReplicator->OnChunkRemoved(chunk);
        }

        Profiler.Increment(RemoveChunkCounter);
    }

    void OnChunkListDestroyed(TChunkList* chunkList)
    {
        auto objectManager = Bootstrap->GetObjectManager();
        // Drop references to children.
        FOREACH (auto childRef, chunkList->Children()) {
            ResetChunkTreeParent(chunkList, childRef);
            objectManager->UnrefObject(childRef.GetId());
        }
    }


    THolderId RegisterHolder(const TMsgRegisterHolder& message)
    {
        Stroka address = message.address();
        auto incarnationId = TIncarnationId::FromProto(message.incarnation_id());
        const auto& statistics = message.statistics();
    
        THolderId holderId = HolderIdGenerator.Next();
    
        auto* existingHolder = FindHolderByAddresss(address);
        if (existingHolder) {
            LOG_INFO_UNLESS(IsRecovery(), "Holder kicked out due to address conflict (Address: %s, HolderId: %d)",
                ~address,
                existingHolder->GetId());
            DoUnregisterHolder(existingHolder);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Holder registered (Address: %s, HolderId: %d, IncarnationId: %s, %s)",
            ~address,
            holderId,
            ~incarnationId.ToString(),
            ~ToString(statistics));

        auto* newHolder = new THolder(
            holderId,
            address,
            incarnationId);
        newHolder->SetState(EHolderState::Registered);
        newHolder->Statistics() = statistics;

        HolderMap.Insert(holderId, newHolder);
        HolderAddressMap.insert(MakePair(address, newHolder));
        HolderHostNameMap.insert(MakePair(GetServiceHostName(address), newHolder));

        if (IsLeader()) {
            StartHolderTracking(newHolder, false);
        }

        return holderId;
    }

    TVoid UnregisterHolder(const TMsgUnregisterHolder& message)
    { 
        auto holderId = message.holder_id();

        // Allow holderId to be invalid, just ignore such obsolete requests.
        auto* holder = FindHolder(holderId);
        if (holder) {
            DoUnregisterHolder(holder);
        }

        return TVoid();
    }

    TVoid FullHeartbeatWithContext(TCtxFullHeartbeat::TPtr context)
    {
        return FullHeartbeat(context->Request());
    }

    TVoid FullHeartbeat(const TMsgFullHeartbeat& message)
    {
        PROFILE_TIMING ("/full_heartbeat_time") {
            Profiler.Enqueue("/full_heartbeat_chunks", message.chunks_size());

            auto holderId = message.holder_id();
            const auto& statistics = message.statistics();

            auto* holder = GetHolder(holderId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Full heartbeat received (Address: %s, HolderId: %d, State: %s, %s, Chunks: %d)",
                ~holder->GetAddress(),
                holderId,
                ~holder->GetState().ToString(),
                ~ToString(statistics),
                static_cast<int>(message.chunks_size()));

            YCHECK(holder->GetState() == EHolderState::Registered);
            holder->SetState(EHolderState::Online);
            holder->Statistics() = statistics;

            if (IsLeader()) {
                HolderLeaseTracker->OnHolderOnline(holder, false);
                ChunkPlacement->OnHolderUpdated(holder);
            }

            LOG_INFO_UNLESS(IsRecovery(), "Holder online (Address: %s, HolderId: %d)",
                ~holder->GetAddress(),
                holderId);

            YCHECK(holder->StoredChunks().empty());
            YCHECK(holder->CachedChunks().empty());

            FOREACH (const auto& chunkInfo, message.chunks()) {
                ProcessAddedChunk(holder, chunkInfo, false);
            }
        }
        return TVoid();
    }

    TVoid IncrementalHeartbeat(const TMsgIncrementalHeartbeat& message)
    {
        Profiler.Enqueue("/incremental_heartbeat_chunks_added", message.added_chunks_size());
        Profiler.Enqueue("/incremental_heartbeat_chunks_removed", message.removed_chunks_size());
        PROFILE_TIMING ("/incremental_heartbeat_time") {
            auto holderId = message.holder_id();
            const auto& statistics = message.statistics();

            auto* holder = GetHolder(holderId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Incremental heartbeat received (Address: %s, HolderId: %d, State: %s, %s, ChunksAdded: %d, ChunksRemoved: %d)",
                ~holder->GetAddress(),
                holderId,
                ~holder->GetState().ToString(),
                ~ToString(statistics),
                static_cast<int>(message.added_chunks_size()),
                static_cast<int>(message.removed_chunks_size()));

            YCHECK(holder->GetState() == EHolderState::Online);
            holder->Statistics() = statistics;

            if (IsLeader()) {
                HolderLeaseTracker->OnHolderHeartbeat(holder);
                ChunkPlacement->OnHolderUpdated(holder);
            }

            FOREACH (const auto& chunkInfo, message.added_chunks()) {
                ProcessAddedChunk(holder, chunkInfo, true);
            }

            FOREACH (const auto& chunkInfo, message.removed_chunks()) {
                ProcessRemovedChunk(holder, chunkInfo);
            }

            std::vector<TChunk*> UnapprovedChunks(holder->UnapprovedChunks().begin(), holder->UnapprovedChunks().end());
            FOREACH (auto* chunk, UnapprovedChunks) {
                RemoveChunkReplica(holder, chunk, false, ERemoveReplicaReason::Unapproved);
            }
            holder->UnapprovedChunks().clear();
        }
        return TVoid();
    }

    TVoid UpdateJobs(const TMsgUpdateJobs& message)
    {
        PROFILE_TIMING ("/update_jobs_time") {
            auto holderId = message.holder_id();
            auto* holder = GetHolder(holderId);

            FOREACH (const auto& startInfo, message.started_jobs()) {
                AddJob(holder, startInfo);
            }

            FOREACH (const auto& stopInfo, message.stopped_jobs()) {
                auto jobId = TJobId::FromProto(stopInfo.job_id());
                auto* job = FindJob(jobId);
                if (job) {
                    // Remove from both job list and holder.
                    RemoveJob(job, true, true);
                }
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Holder jobs updated (Address: %s, HolderId: %d, JobsStarted: %d, JobsStopped: %d)",
                ~holder->GetAddress(),
                holderId,
                static_cast<int>(message.started_jobs_size()),
                static_cast<int>(message.stopped_jobs_size()));
        }
        return TVoid();
    }

    TVoid RebalanceChunkTree(const TMsgRebalanceChunkTree& message)
    {
        PROFILE_TIMING ("/chunk_tree_rebalance_time") {
            auto rootId = TChunkListId::FromProto(message.root_id());
            auto* root = FindChunkList(rootId);
            if (!root) {
                return TVoid();
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk tree rebalancing started (RootId: %s)",
                ~rootId.ToString());

            if (ChunkTreeBalancer.RebalanceChunkTree(root, message)) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Chunk tree rebalancing completed");
            } else {
                LOG_DEBUG_UNLESS(IsRecovery(), "Chunk tree rebalancing canceled");
            }
        }
        return TVoid();
    }


    void SaveKeys(TOutputStream* output) const
    {
        ChunkMap.SaveKeys(output);
        ChunkListMap.SaveKeys(output);
        HolderMap.SaveKeys(output);
        JobMap.SaveKeys(output);
        JobListMap.SaveKeys(output);
    }

    void SaveValues(TOutputStream* output) const
    {
        ::Save(output, HolderIdGenerator);

        ChunkMap.SaveValues(output);
        ChunkListMap.SaveValues(output);
        HolderMap.SaveValues(output);
        JobMap.SaveValues(output);
        JobListMap.SaveValues(output);
    }

    void LoadKeys(TInputStream* input)
    {
        ChunkMap.LoadKeys(input);
        ChunkListMap.LoadKeys(input);
        HolderMap.LoadKeys(input);
        JobMap.LoadKeys(input);
        JobListMap.LoadKeys(input);
    }

    void LoadValues(TLoadContext context, TInputStream* input)
    {
        ::Load(input, HolderIdGenerator);

        ChunkMap.LoadValues(context, input);
        ChunkListMap.LoadValues(context, input);
        HolderMap.LoadValues(context, input);
        JobMap.LoadValues(context, input);
        JobListMap.LoadValues(context, input);

        // Compute chunk replica count.
        ChunkReplicaCount = 0;
        FOREACH (const auto& pair, HolderMap) {
            const auto* holder = pair.second;
            ChunkReplicaCount += holder->StoredChunks().size();
            ChunkReplicaCount += holder->CachedChunks().size();
        }

        // Reconstruct HolderAddressMap.
        HolderAddressMap.clear();
        HolderHostNameMap.clear();
        FOREACH (const auto& pair, HolderMap) {
            auto* holder = pair.second;
            const auto& address = holder->GetAddress();
            YCHECK(HolderAddressMap.insert(MakePair(address, holder)).second);
            HolderHostNameMap.insert(MakePair(GetServiceHostName(address), holder));
        }

        // Reconstruct ReplicationSinkMap.
        ReplicationSinkMap.clear();
        FOREACH (auto& pair, JobMap) {
            RegisterReplicationSinks(pair.second);
        }
    }

    virtual void Clear()
    {
        HolderIdGenerator.Reset();
        ChunkMap.Clear();
        ChunkReplicaCount = 0;
        ChunkListMap.Clear();
        HolderMap.Clear();
        JobMap.Clear();
        JobListMap.Clear();

        HolderAddressMap.clear();
        HolderHostNameMap.clear();

        ReplicationSinkMap.clear();
    }


    virtual void OnStartRecovery()
    {
        Profiler.SetEnabled(false);
    }

    virtual void OnStopRecovery()
    {
        Profiler.SetEnabled(true);
    }

    virtual void OnLeaderRecoveryComplete()
    {
        ChunkPlacement = New<TChunkPlacement>(Config, Bootstrap);

        HolderLeaseTracker = New<THolderLeaseTracker>(Config, Bootstrap);

        ChunkReplicator = New<TChunkReplicator>(Config, Bootstrap, ChunkPlacement, HolderLeaseTracker);

        // Assign initial leases to holders.
        // NB: Holders will remain unconfirmed until the first heartbeat.
        FOREACH (const auto& pair, HolderMap) { 
            StartHolderTracking(pair.second, true);
        }

        PROFILE_TIMING ("/full_chunk_refresh_time") {
            LOG_INFO("Starting full chunk refresh");
            ChunkReplicator->RefreshAllChunks();
            LOG_INFO("Full chunk refresh completed");
        }
    }

    virtual void OnStopLeading()
    {
        ChunkPlacement.Reset();
        ChunkReplicator.Reset();
        HolderLeaseTracker.Reset();
    }


    void StartHolderTracking(THolder* holder, bool recovery)
    {
        HolderLeaseTracker->OnHolderRegistered(holder, recovery);
        if (holder->GetState() == EHolderState::Online) {
            HolderLeaseTracker->OnHolderOnline(holder, recovery);
        }

        ChunkPlacement->OnHolderRegistered(holder);
        
        ChunkReplicator->OnHolderRegistered(holder);

        HolderRegistered_.Fire(holder);
    }

    void StopHolderTracking(THolder* holder)
    {
        HolderLeaseTracker->OnHolderUnregistered(holder);
        ChunkPlacement->OnHolderUnregistered(holder);
        ChunkReplicator->OnHolderUnregistered(holder);

        HolderUnregistered_.Fire(holder);
    }


    void DoUnregisterHolder(THolder* holder)
    { 
        PROFILE_TIMING ("/holder_unregistration_time") {
            auto holderId = holder->GetId();

            LOG_INFO_UNLESS(IsRecovery(), "Holder unregistered (Address: %s, HolderId: %d)",
                ~holder->GetAddress(),
                holderId);

            if (IsLeader()) {
                StopHolderTracking(holder);
            }

            FOREACH (auto& chunk, holder->StoredChunks()) {
                RemoveChunkReplica(holder, chunk, false, ERemoveReplicaReason::Reset);
            }

            FOREACH (auto& chunk, holder->CachedChunks()) {
                RemoveChunkReplica(holder, chunk, true, ERemoveReplicaReason::Reset);
            }

            FOREACH (auto& job, holder->Jobs()) {
                // Suppress removal of job from holder.
                RemoveJob(job, false, true);
            }

            const auto& address = holder->GetAddress();
            YCHECK(HolderAddressMap.erase(address) == 1);
            {
                auto hostNameRange = HolderHostNameMap.equal_range(Stroka(GetServiceHostName(address)));
                for (auto it = hostNameRange.first; it != hostNameRange.second; ++it) {
                    if (it->second == holder) {
                        HolderHostNameMap.erase(it);
                        break;
                    }
                }
            }
            HolderMap.Remove(holderId);
        }
    }


    DECLARE_ENUM(EAddReplicaReason,
        (IncrementalHeartbeat)
        (FullHeartbeat)
        (Confirmation)
    );

    void AddChunkReplica(THolder* holder, TChunk* chunk, bool cached, EAddReplicaReason reason)
    {
        auto chunkId = chunk->GetId();
        auto holderId = holder->GetId();

        if (holder->HasChunk(chunk, cached)) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk replica is already added (ChunkId: %s, Cached: %s, Reason: %s, Address: %s, HolderId: %d)",
                ~chunkId.ToString(),
                ~FormatBool(cached),
                ~reason.ToString(),
                ~holder->GetAddress(),
                holderId);
            return;
        }

        holder->AddChunk(chunk, cached);
        chunk->AddLocation(holderId, cached);

        if (!IsRecovery()) {
            LOG_EVENT(
                Logger,
                reason == EAddReplicaReason::FullHeartbeat ? NLog::ELogLevel::Trace : NLog::ELogLevel::Debug,
                "Chunk replica added (ChunkId: %s, Cached: %s, Address: %s, HolderId: %d)",
                ~chunkId.ToString(),
                ~FormatBool(cached),
                ~holder->GetAddress(),
                holderId);
        }

        if (!cached && IsLeader()) {
            ChunkReplicator->ScheduleChunkRefresh(chunk->GetId());
        }

        if (reason == EAddReplicaReason::IncrementalHeartbeat || reason == EAddReplicaReason::Confirmation) {
            Profiler.Increment(AddChunkReplicaCounter);
        }
    }

    void ScheduleChunkReplicaRemoval(THolderId holderId, TChunk* chunk, bool cached)
    {
        auto chunkId = chunk->GetId();
        auto* holder = GetHolder(holderId);
        holder->RemoveChunk(chunk, cached);

        if (!cached && IsLeader()) {
            ChunkReplicator->ScheduleChunkRemoval(holder, chunkId);
        }
    }

    DECLARE_ENUM(ERemoveReplicaReason,
        (IncrementalHeartbeat)
        (Unapproved)
        (Reset)
    );

    void RemoveChunkReplica(THolder* holder, TChunk* chunk, bool cached, ERemoveReplicaReason reason)
    {
        auto chunkId = chunk->GetId();
        auto holderId = holder->GetId();

        if (reason == ERemoveReplicaReason::IncrementalHeartbeat && !holder->HasChunk(chunk, cached)) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk replica is already removed (ChunkId: %s, Cached: %s, Reason: %s, Address: %s, HolderId: %d)",
                ~chunkId.ToString(),
                ~FormatBool(cached),
                ~reason.ToString(),
                ~holder->GetAddress(),
                holderId);
            return;
        }

        switch (reason) {
            case ERemoveReplicaReason::IncrementalHeartbeat:
            case ERemoveReplicaReason::Unapproved:
                holder->RemoveChunk(chunk, cached);
                break;
            case ERemoveReplicaReason::Reset:
                // Do nothing.
                break;
            default:
                YUNREACHABLE();
        }
        chunk->RemoveLocation(holder->GetId(), cached);

        if (!IsRecovery()) {
            LOG_EVENT(
                Logger,
                reason == ERemoveReplicaReason::Reset ? NLog::ELogLevel::Trace : NLog::ELogLevel::Debug,
                "Chunk replica removed (ChunkId: %s, Cached: %s, Reason: %s, Address: %s, HolderId: %d)",
                ~chunkId.ToString(),
                ~FormatBool(cached),
                ~reason.ToString(),
                ~holder->GetAddress(),
                holderId);
        }

        if (!cached && IsLeader()) {
            ChunkReplicator->ScheduleChunkRefresh(chunkId);
        }

        Profiler.Increment(RemoveChunkReplicaCounter);
    }


    void AddJob(THolder* holder, const TJobStartInfo& jobInfo)
    {
        auto metaStateManager = Bootstrap->GetMetaStateManager();
        auto* mutationContext = metaStateManager->GetMutationContext();

        auto chunkId = TChunkId::FromProto(jobInfo.chunk_id());
        auto jobId = TJobId::FromProto(jobInfo.job_id());
        auto targetAddresses = FromProto<Stroka>(jobInfo.target_addresses());
        auto jobType = EJobType(jobInfo.type());

        auto* job = new TJob(
            jobType,
            jobId,
            chunkId,
            holder->GetAddress(),
            targetAddresses,
            mutationContext->GetTimestamp());
        JobMap.Insert(jobId, job);

        auto* jobList = GetOrCreateJobList(chunkId);
        jobList->AddJob(job);

        holder->AddJob(job);

        RegisterReplicationSinks(job);

        LOG_INFO_UNLESS(IsRecovery(), "Job added (JobId: %s, Address: %s, HolderId: %d, JobType: %s, ChunkId: %s)",
            ~jobId.ToString(),
            ~holder->GetAddress(),
            holder->GetId(),
            ~jobType.ToString(),
            ~chunkId.ToString());
    }

    void RemoveJob(
        TJob* job,
        bool removeFromHolder,
        bool removeFromJobList)
    {
        auto jobId = job->GetId();

        if (removeFromJobList) {
            auto* jobList = GetJobList(job->GetChunkId());
            jobList->RemoveJob(job);
            DropJobListIfEmpty(jobList);
        }

        if (removeFromHolder) {
            auto* holder = FindHolderByAddresss(job->GetAddress());
            if (holder) {
                holder->RemoveJob(job);
            }
        }

        if (IsLeader()) {
            ChunkReplicator->ScheduleChunkRefresh(job->GetChunkId());
        }

        UnregisterReplicationSinks(job);

        JobMap.Remove(jobId);

        LOG_INFO_UNLESS(IsRecovery(), "Job removed (JobId: %s)", ~jobId.ToString());
    }


    void ProcessAddedChunk(
        THolder* holder,
        const TChunkAddInfo& chunkAddInfo,
        bool incremental)
    {
        auto holderId = holder->GetId();
        auto chunkId = TChunkId::FromProto(chunkAddInfo.chunk_id());
        bool cached = chunkAddInfo.cached();

        auto* chunk = FindChunk(chunkId);
        if (!chunk) {
            // Holders may still contain cached replicas of chunks that no longer exist.
            // Here we just silently ignore this case.
            if (cached) {
                return;
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Unknown chunk added, removal scheduled (Address: %s, HolderId: %d, ChunkId: %s, Cached: %s)",
                ~holder->GetAddress(),
                holderId,
                ~chunkId.ToString(),
                ~FormatBool(cached));

            if (IsLeader()) {
                ChunkReplicator->ScheduleChunkRemoval(holder, chunkId);
            }

            return;
        }

        if (!cached && holder->HasUnapprovedChunk(chunk)) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk approved (Address: %s, HolderId: %d, ChunkId: %s)",
                ~holder->GetAddress(),
                holderId,
                ~chunkId.ToString());

            holder->ApproveChunk(chunk);
            return;
        }

        // Use the size reported by the holder, but check it for consistency first.
        if (!chunk->ValidateChunkInfo(chunkAddInfo.chunk_info())) {
            auto message = Sprintf("Mismatched chunk info reported by node (ChunkId: %s, Cached: %s, ExpectedInfo: {%s}, ReceivedInfo: {%s}, Address: %s, HolderId: %d)",
                ~chunkId.ToString(),
                ~ToString(cached),
                ~chunk->ChunkInfo().DebugString(),
                ~chunkAddInfo.chunk_info().DebugString(),
                ~holder->GetAddress(),
                holder->GetId());
            LOG_ERROR("%s", ~message);
            return;
        }
        chunk->ChunkInfo() = chunkAddInfo.chunk_info();

        AddChunkReplica(
            holder,
            chunk,
            cached,
            incremental ? EAddReplicaReason::IncrementalHeartbeat : EAddReplicaReason::FullHeartbeat);
    }

    void ProcessRemovedChunk(
        THolder* holder,
        const TChunkRemoveInfo& chunkInfo)
    {
        auto holderId = holder->GetId();
        auto chunkId = TChunkId::FromProto(chunkInfo.chunk_id());
        bool cached = chunkInfo.cached();

        auto* chunk = FindChunk(chunkId);
        if (!chunk) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Unknown chunk replica removed (ChunkId: %s, Cached: %s, Address: %s, HolderId: %d)",
                 ~chunkId.ToString(),
                 ~FormatBool(cached),
                 ~holder->GetAddress(),
                 holderId);
            return;
        }

        RemoveChunkReplica(
            holder,
            chunk,
            cached,
            ERemoveReplicaReason::IncrementalHeartbeat);
    }


    TJobList* GetOrCreateJobList(const TChunkId& id)
    {
        auto* jobList = FindJobList(id);
        if (!jobList) {
            jobList = new TJobList(id);
            JobListMap.Insert(id, jobList);
        }
        return jobList;
    }

    void DropJobListIfEmpty(const TJobList* jobList)
    {
        if (jobList->Jobs().empty()) {
            JobListMap.Remove(jobList->GetChunkId());
        }
    }


    void RegisterReplicationSinks(TJob* job)
    {
        switch (job->GetType()) {
            case EJobType::Replicate: {
                FOREACH (const auto& address, job->TargetAddresses()) {
                    auto& sink = GetOrCreateReplicationSink(address);
                    YCHECK(sink.Jobs().insert(job).second);
                }
                break;
            }

            case EJobType::Remove:
                break;

            default:
                YUNREACHABLE();
        }
    }

    void UnregisterReplicationSinks(TJob* job)
    {
        switch (job->GetType()) {
            case EJobType::Replicate: {
                FOREACH (const auto& address, job->TargetAddresses()) {
                    auto& sink = GetOrCreateReplicationSink(address);
                    YCHECK(sink.Jobs().erase(job) == 1);
                    DropReplicationSinkIfEmpty(sink);
                }
                break;
            }

            case EJobType::Remove:
                break;

            default:
                YUNREACHABLE();
        }
    }

    TReplicationSink& GetOrCreateReplicationSink(const Stroka& address)
    {
        auto it = ReplicationSinkMap.find(address);
        if (it != ReplicationSinkMap.end()) {
            return it->second;
        }

        auto pair = ReplicationSinkMap.insert(MakePair(address, TReplicationSink(address)));
        YCHECK(pair.second);

        return pair.first->second;
    }

    void DropReplicationSinkIfEmpty(const TReplicationSink& sink)
    {
        if (sink.Jobs().empty()) {
            // NB: Do not try to inline this variable! erase() will destroy the object
            // and will access the key afterwards.
            auto address = sink.GetAddress();
            YCHECK(ReplicationSinkMap.erase(address) == 1);
        }
    }

    static void GetOwningNodes(
        TChunkTreeRef chunkRef,
        yhash_set<TChunkTreeRef>& visitedRefs,
        yhash_set<ICypressNode*>* owningNodes)
    {
        if (!visitedRefs.insert(chunkRef).second) {
            return;
        }
        switch (chunkRef.GetType()) {
            case EObjectType::Chunk: {
                FOREACH (auto* parent, chunkRef.AsChunk()->Parents()) {
                    GetOwningNodes(parent, visitedRefs, owningNodes);
                }
                break;
            }
            case EObjectType::ChunkList: {
                auto* chunkList = chunkRef.AsChunkList();
                owningNodes->insert(chunkList->OwningNodes().begin(), chunkList->OwningNodes().end());
                FOREACH (auto* parent, chunkList->Parents()) {
                    GetOwningNodes(parent, visitedRefs, owningNodes);
                }
                break;
            }
            default:
                YUNREACHABLE();
        }
    }

    void GetOwningNodes(TChunkTreeRef chunkRef, IYsonConsumer* consumer)
    {
        auto cypressManager = Bootstrap->GetCypressManager();

        yhash_set<ICypressNode*> owningNodes;
        yhash_set<TChunkTreeRef> visitedRefs;
        GetOwningNodes(chunkRef, visitedRefs, &owningNodes);

        std::vector<TYPath> paths;
        FOREACH (auto* node, owningNodes) {
            auto proxy = cypressManager->GetVersionedNodeProxy(node->GetId());
            auto path = proxy->GetPath();
            paths.push_back(path);
        }

        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

        BuildYsonFluently(consumer)
            .DoListFor(paths, [] (TFluentList fluent, const TYPath& path) {
                fluent.Item().Scalar(path);
            });
    }

};

DEFINE_METAMAP_ACCESSORS(TChunkManager::TImpl, Chunk, TChunk, TChunkId, ChunkMap)
DEFINE_METAMAP_ACCESSORS(TChunkManager::TImpl, ChunkList, TChunkList, TChunkListId, ChunkListMap)
DEFINE_METAMAP_ACCESSORS(TChunkManager::TImpl, Holder, THolder, THolderId, HolderMap)
DEFINE_METAMAP_ACCESSORS(TChunkManager::TImpl, JobList, TJobList, TChunkId, JobListMap)
DEFINE_METAMAP_ACCESSORS(TChunkManager::TImpl, Job, TJob, TJobId, JobMap)

DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunkId>, LostChunkIds, *ChunkReplicator);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunkId>, OverreplicatedChunkIds, *ChunkReplicator);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunkId>, UnderreplicatedChunkIds, *ChunkReplicator);

///////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkProxy
    : public NObjectServer::TUnversionedObjectProxyBase<TChunk>
{
public:
    TChunkProxy(TIntrusivePtr<TImpl> owner, const TChunkId& id)
        : TBase(owner->Bootstrap, id, &owner->ChunkMap)
        , Owner(owner)
    {
        Logger = ChunkServerLogger;
    }

    virtual bool IsWriteRequest(NRpc::IServiceContextPtr context) const
    {
        DECLARE_YPATH_SERVICE_WRITE_METHOD(Confirm);
        return TBase::IsWriteRequest(context);
    }

private:
    typedef TUnversionedObjectProxyBase<TChunk> TBase;

    TIntrusivePtr<TImpl> Owner;

    virtual void GetSystemAttributes(std::vector<TAttributeInfo>* attributes)
    {
        const auto* chunk = GetTypedImpl();
        auto miscExt = GetProtoExtension<TMiscExt>(chunk->ChunkMeta().extensions());

        attributes->push_back("confirmed");
        attributes->push_back("cached_locations");
        attributes->push_back("stored_locations");
        attributes->push_back("replication_factor");
        attributes->push_back("master_meta_size");
        attributes->push_back(TAttributeInfo("meta_size", miscExt.has_meta_size()));
        attributes->push_back(TAttributeInfo("compressed_data_size", miscExt.has_compressed_data_size()));
        attributes->push_back(TAttributeInfo("uncompressed_data_size", miscExt.has_uncompressed_data_size()));
        attributes->push_back(TAttributeInfo("data_weight", miscExt.has_data_weight()));
        attributes->push_back(TAttributeInfo("codec_id", miscExt.has_codec_id()));
        attributes->push_back(TAttributeInfo("row_count", miscExt.has_row_count()));
        attributes->push_back(TAttributeInfo("value_count", miscExt.has_value_count()));
        attributes->push_back(TAttributeInfo("sorted", miscExt.has_sorted()));
        attributes->push_back(TAttributeInfo("size", chunk->IsConfirmed()));
        attributes->push_back(TAttributeInfo("chunk_type", chunk->IsConfirmed()));
        attributes->push_back(TAttributeInfo("owning_nodes", true, true));
        TBase::GetSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& name, IYsonConsumer* consumer)
    {
        const auto* chunk = GetTypedImpl();
        auto miscExt = GetProtoExtension<TMiscExt>(chunk->ChunkMeta().extensions());

        if (name == "confirmed") {
            BuildYsonFluently(consumer)
                .Scalar(FormatBool(chunk->IsConfirmed()));
            return true;
        }

        if (name == "cached_locations") {
            if (~chunk->CachedLocations()) {
                BuildYsonFluently(consumer)
                    .DoListFor(*chunk->CachedLocations(), [=] (TFluentList fluent, THolderId holderId) {
                        const auto& holder = Owner->GetHolder(holderId);
                        fluent.Item().Scalar(holder->GetAddress());
                    });
            } else {
                BuildYsonFluently(consumer)
                    .BeginList()
                    .EndList();
            }
            return true;
        }

        if (name == "stored_locations") {
            BuildYsonFluently(consumer)
                .DoListFor(chunk->StoredLocations(), [=] (TFluentList fluent, THolderId holderId) {
                    const auto& holder = Owner->GetHolder(holderId);
                    fluent.Item().Scalar(holder->GetAddress());
                });
            return true;
        }

        if (name == "replication_factor") {
            BuildYsonFluently(consumer)
                .Scalar(chunk->GetReplicationFactor());
            return true;
        }

        if (name == "meta_size") {
            BuildYsonFluently(consumer)
                .Scalar(miscExt.meta_size());
            return true;
        }

        if (name == "compressed_data_size") {
            BuildYsonFluently(consumer)
                .Scalar(miscExt.compressed_data_size());
            return true;
        }

        if (name == "uncompressed_data_size") {
            BuildYsonFluently(consumer)
                .Scalar(miscExt.uncompressed_data_size());
            return true;
        }

        if (name == "data_weight") {
            BuildYsonFluently(consumer)
                .Scalar(miscExt.data_weight());
            return true;
        }

        if (name == "row_count") {
            BuildYsonFluently(consumer)
                .Scalar(miscExt.row_count());
            return true;
        }

        if (name == "value_count") {
            BuildYsonFluently(consumer)
                .Scalar(miscExt.value_count());
            return true;
        }

        if (name == "codec_id") {
            BuildYsonFluently(consumer)
                .Scalar(CamelCaseToUnderscoreCase(ECodecId(miscExt.codec_id()).ToString()));
            return true;
        }

        if (name == "sorted") {
            BuildYsonFluently(consumer)
                .Scalar(FormatBool(miscExt.sorted()));
            return true;
        }

        if (name == "master_meta_size") {
            BuildYsonFluently(consumer)
                .Scalar(chunk->ChunkMeta().ByteSize());
            return true;
        }

        if (chunk->IsConfirmed()) {
            if (name == "size") {
                BuildYsonFluently(consumer)
                    .Scalar(chunk->ChunkInfo().size());
                return true;
            }

            if (name == "chunk_type") {
                auto type = EChunkType(chunk->ChunkMeta().type());
                BuildYsonFluently(consumer)
                    .Scalar(CamelCaseToUnderscoreCase(type.ToString()));
                return true;
            }
        }

        if (name == "owning_nodes") {
            Owner->GetOwningNodes(TChunkTreeRef(const_cast<TChunk*>(chunk)), consumer);
            return true;
        }

        return TBase::GetSystemAttribute(name, consumer);
    }

    virtual void DoInvoke(NRpc::IServiceContextPtr context)
    {
        DISPATCH_YPATH_SERVICE_METHOD(Locate);
        DISPATCH_YPATH_SERVICE_METHOD(Fetch);
        DISPATCH_YPATH_SERVICE_METHOD(Confirm);
        TBase::DoInvoke(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Locate)
    {
        UNUSED(request);

        const auto* chunk = GetTypedImpl();
        Owner->FillHolderAddresses(response->mutable_node_addresses(), chunk);

        context->SetResponseInfo("NodeAddresses: [%s]",
            ~JoinToString(response->node_addresses()));

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NTableServer::NProto, Fetch)
    {
        UNUSED(request);

        const auto* chunk = GetTypedImpl();

        if (chunk->ChunkMeta().type() != EChunkType::Table) {
            ythrow yexception() << Sprintf("Unable to execute Fetch verb for non-table chunk.");
        }

        auto* inputChunk = response->add_chunks();
        *inputChunk->mutable_slice()->mutable_chunk_id() = chunk->GetId().ToProto();
        *inputChunk->mutable_channel() = NTableClient::TChannel::CreateUniversal().ToProto();
        inputChunk->mutable_extensions()->CopyFrom(chunk->ChunkMeta().extensions());

        auto miscExt = GetProtoExtension<TMiscExt>(chunk->ChunkMeta().extensions());
        inputChunk->set_row_count(miscExt.row_count());
        inputChunk->set_data_weight(miscExt.data_weight());

        if (request->fetch_node_addresses()) {
            Owner->FillHolderAddresses(inputChunk->mutable_node_addresses(), chunk);
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Confirm)
    {
        UNUSED(response);

        auto& holderAddresses = request->node_addresses();
        YCHECK(holderAddresses.size() != 0);

        context->SetRequestInfo("Size: %" PRId64 ", HolderAddresses: [%s]",
            request->chunk_info().size(),
            ~JoinToString(holderAddresses));

        auto* chunk = GetTypedImpl();

        // Skip chunks that are already confirmed.
        if (chunk->IsConfirmed()) {
            context->SetResponseInfo("Chunk is already confirmed");
            context->Reply();
            return;
        }

        // Use the size reported by the client, but check it for consistency first.
        if (!chunk->ValidateChunkInfo(request->chunk_info())) {
            LOG_FATAL("Invalid chunk info reported by client (ChunkId: %s, ExpectedInfo: {%s}, ReceivedInfo: {%s})",
                ~Id.ToString(),
                ~chunk->ChunkInfo().DebugString(),
                ~request->chunk_info().DebugString());
        }
        chunk->ChunkInfo().Swap(request->mutable_chunk_info());

        FOREACH (const auto& address, holderAddresses) {
            auto* holder = Owner->FindHolderByAddresss(address);
            if (!holder) {
                LOG_DEBUG_UNLESS(Owner->IsRecovery(), "Tried to confirm chunk %s at an unknown node %s",
                    ~Id.ToString(),
                    ~address);
                continue;
            }

            if (holder->GetState() != EHolderState::Online) {
                LOG_DEBUG_UNLESS(Owner->IsRecovery(), "Tried to confirm chunk %s at node %s with invalid state %s",
                    ~Id.ToString(),
                    ~address,
                    ~FormatEnum(holder->GetState()));
                continue;
            }

            if (!holder->HasChunk(chunk, false)) {
                Owner->AddChunkReplica(
                    holder,
                    chunk,
                    false,
                    TImpl::EAddReplicaReason::Confirmation);
                holder->MarkChunkUnapproved(chunk);
            }
        }

        chunk->ChunkMeta().Swap(request->mutable_chunk_meta());
        LOG_INFO_UNLESS(Owner->IsRecovery(), "Chunk confirmed (ChunkId: %s)", ~Id.ToString());

        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

TChunkManager::TChunkTypeHandler::TChunkTypeHandler(TImpl* owner)
    : TObjectTypeHandlerBase(owner->Bootstrap, &owner->ChunkMap)
    , Owner(owner)
{ }

IObjectProxyPtr TChunkManager::TChunkTypeHandler::GetProxy(
    const TObjectId& id,
    TTransaction* transaction)
{
    UNUSED(transaction);
    return New<TChunkProxy>(Owner, id);
}

TObjectId TChunkManager::TChunkTypeHandler::Create(
    TTransaction* transaction,
    TReqCreateObject* request,
    TRspCreateObject* response)
{
    UNUSED(transaction);

    const auto* requestExt = &request->GetExtension(TReqCreateChunkExt::create_chunk);
    auto* responseExt = response->MutableExtension(TRspCreateChunkExt::create_chunk);

    auto* chunk = Owner->CreateChunk();
    chunk->SetReplicationFactor(requestExt->replication_factor());
    chunk->SetMovable(requestExt->movable());

    if (Owner->IsLeader()) {
        auto preferredHostName =
            requestExt->has_preferred_host_name()
            ? TNullable<Stroka>(requestExt->preferred_host_name())
            : Null;

        auto nodes = Owner->AllocateUploadTargets(
            requestExt->upload_replication_factor(),
            preferredHostName);

        FOREACH (auto* node, nodes) {
            responseExt->add_node_addresses(node->GetAddress());
        }

        LOG_DEBUG_UNLESS(Owner->IsRecovery(), "Allocated nodes [%s] for chunk %s (PreferredHostName: %s, ReplicationFactor: %d, UploadReplicationFactor: %d, Movable: %s)",
            ~JoinToString(responseExt->node_addresses()),
            ~chunk->GetId().ToString(),
            ~ToString(preferredHostName),
            requestExt->replication_factor(),
            requestExt->upload_replication_factor(),
            ~FormatBool(requestExt->movable()));
    }

    return chunk->GetId();
}

void TChunkManager::TChunkTypeHandler::OnObjectDestroyed(TChunk* chunk)
{
    Owner->OnChunkDestroyed(chunk);
}

///////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkListProxy
    : public NObjectServer::TUnversionedObjectProxyBase<TChunkList>
{
public:
    TChunkListProxy(TIntrusivePtr<TImpl> owner, const TChunkListId& id)
        : TBase(owner->Bootstrap, id, &owner->ChunkListMap)
        , Owner(owner)
    {
        Logger = ChunkServerLogger;
    }

    virtual bool IsWriteRequest(NRpc::IServiceContextPtr context) const
    {
        DECLARE_YPATH_SERVICE_WRITE_METHOD(Attach);
        return TBase::IsWriteRequest(context);
    }

private:
    typedef TUnversionedObjectProxyBase<TChunkList> TBase;

    TIntrusivePtr<TImpl> Owner;

    virtual void GetSystemAttributes(std::vector<TAttributeInfo>* attributes)
    {
        attributes->push_back("children_ids");
        attributes->push_back("parent_ids");
        attributes->push_back("row_count");
        attributes->push_back("uncompressed_data_size");
        attributes->push_back("compressed_size");
        attributes->push_back("chunk_count");
        attributes->push_back("rank");
        attributes->push_back("rigid");
        attributes->push_back(TAttributeInfo("tree", true, true));
        attributes->push_back(TAttributeInfo("owning_nodes", true, true));
        TBase::GetSystemAttributes(attributes);
    }

    void TraverseTree(TChunkTreeRef ref, IYsonConsumer* consumer)
    {
        switch (ref.GetType()) {
            case EObjectType::Chunk:
                consumer->OnStringScalar(ref.GetId().ToString());
                break;
            case EObjectType::ChunkList: {
                const auto* chunkList = ref.AsChunkList();
                consumer->OnBeginAttributes();
                consumer->OnKeyedItem("id");
                consumer->OnStringScalar(chunkList->GetId().ToString());
                consumer->OnKeyedItem("rank");
                consumer->OnIntegerScalar(chunkList->Statistics().Rank);
                consumer->OnEndAttributes();

                consumer->OnBeginList();
                FOREACH (auto childRef, chunkList->Children()) {
                    consumer->OnListItem();
                    TraverseTree(childRef, consumer);
                }
                consumer->OnEndList();
                break;
            }
            default:
                YUNREACHABLE();
        }
    }

    virtual bool GetSystemAttribute(const Stroka& name, IYsonConsumer* consumer)
    {
        const auto* chunkList = GetTypedImpl();

        if (name == "children_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(chunkList->Children(), [=] (TFluentList fluent, TChunkTreeRef chunkRef) {
                        fluent.Item().Scalar(chunkRef.GetId());
                });
            return true;
        }

        if (name == "parent_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(chunkList->Parents(), [=] (TFluentList fluent, TChunkList* chunkList) {
                    fluent.Item().Scalar(chunkList->GetId());
                });
            return true;
        }

        const auto& statistics = chunkList->Statistics();

        if (name == "row_count") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.RowCount);
            return true;
        }

        if (name == "uncompressed_data_size") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.UncompressedSize);
            return true;
        }

        if (name == "compressed_size") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.CompressedSize);
            return true;
        }

        if (name == "chunk_count") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.ChunkCount);
            return true;
        }

        if (name == "rank") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.Rank);
            return true;
        }

        if (name == "rigid") {
            BuildYsonFluently(consumer)
                .Scalar(chunkList->GetRigid());
            return true;
        }

        if (name == "tree") {
            TraverseTree(const_cast<TChunkList*>(chunkList), consumer);
            return true;
        }

        if (name == "owning_nodes") {
            Owner->GetOwningNodes(const_cast<TChunkList*>(chunkList), consumer);
            return true;
        }

        return TBase::GetSystemAttribute(name, consumer);
    }

    virtual void DoInvoke(NRpc::IServiceContextPtr context)
    {
        DISPATCH_YPATH_SERVICE_METHOD(Attach);
        TBase::DoInvoke(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Attach)
    {
        UNUSED(response);

        auto childrenIds = FromProto<TChunkTreeId>(request->children_ids());

        context->SetRequestInfo("Children: [%s]", ~JoinToString(childrenIds));

        auto objectManager = Bootstrap->GetObjectManager();
        std::vector<TChunkTreeRef> childrenRefs;
        FOREACH (const auto& childId, childrenIds) {
            if (!objectManager->ObjectExists(childId)) {
                ythrow yexception() << Sprintf("Chunk tree %s does not exist", ~childId.ToString());
            }
            auto chunkRef = Owner->GetChunkTree(childId);
            childrenRefs.push_back(chunkRef);
        }

        auto* chunkList = GetTypedImpl();
        Owner->AttachToChunkList(chunkList, childrenRefs);

        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

TChunkManager::TChunkListTypeHandler::TChunkListTypeHandler(TImpl* owner)
    : TObjectTypeHandlerBase(owner->Bootstrap, &owner->ChunkListMap)
    , Owner(owner)
{ }

IObjectProxyPtr TChunkManager::TChunkListTypeHandler::GetProxy(
    const TObjectId& id,
    TTransaction* transaction)
{
    UNUSED(transaction);
    return New<TChunkListProxy>(Owner, id);
}

TObjectId TChunkManager::TChunkListTypeHandler::Create(
    TTransaction* transaction,
    TReqCreateObject* request,
    TRspCreateObject* response)
{
    UNUSED(transaction);
    UNUSED(request);
    UNUSED(response);

    auto* chunkList = Owner->CreateChunkList();
    return chunkList->GetId();
}

void TChunkManager::TChunkListTypeHandler::OnObjectDestroyed(TChunkList* chunkList)
{
    Owner->OnChunkListDestroyed(chunkList);
}

////////////////////////////////////////////////////////////////////////////////

TChunkManager::TChunkManager(
    TChunkManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

TChunkManager::~TChunkManager()
{ }

THolder* TChunkManager::FindHolderByAddress(const Stroka& address)
{
    return Impl->FindHolderByAddresss(address);
}

THolder* TChunkManager::FindHolderByHostName(const Stroka& hostName)
{
    return Impl->FindHolderByHostName(hostName);
}

const TReplicationSink* TChunkManager::FindReplicationSink(const Stroka& address)
{
    return Impl->FindReplicationSink(address);
}

std::vector<THolder*> TChunkManager::AllocateUploadTargets(
    int nodeCount,
    TNullable<Stroka> preferredHostName)
{
    return Impl->AllocateUploadTargets(nodeCount, preferredHostName);
}

TMutation<THolderId>::TPtr TChunkManager::InitiateRegisterHolder(
    const TMsgRegisterHolder& message)
{
    return Impl->InitiateRegisterHolder(message);
}

TMutation<TVoid>::TPtr TChunkManager::InitiateUnregisterHolder(
    const TMsgUnregisterHolder& message)
{
    return Impl->InitiateUnregisterHolder(message);
}

TMutation<TVoid>::TPtr TChunkManager::InitiateFullHeartbeat(
    TCtxFullHeartbeat::TPtr context)
{
    return Impl->InitiateFullHeartbeat(context);
}

TMutation<TVoid>::TPtr TChunkManager::InitiateIncrementalHeartbeat(
    const TMsgIncrementalHeartbeat& message)
{
    return Impl->InitiateIncrementalHeartbeat(message);
}

TMutation<TVoid>::TPtr TChunkManager::InitiateUpdateJobs(
    const TMsgUpdateJobs& message)
{
    return Impl->InitiateUpdateJobs(message);
}

TChunk* TChunkManager::CreateChunk()
{
    return Impl->CreateChunk();
}

TChunkList* TChunkManager::CreateChunkList()
{
    return Impl->CreateChunkList();
}

void TChunkManager::AttachToChunkList(
    TChunkList* chunkList,
    const TChunkTreeRef* childrenBegin,
    const TChunkTreeRef* childrenEnd)
{
    Impl->AttachToChunkList(chunkList, childrenBegin, childrenEnd);
}

void TChunkManager::AttachToChunkList(
    TChunkList* chunkList,
    const std::vector<TChunkTreeRef>& children)
{
    Impl->AttachToChunkList(chunkList, children);
}

void TChunkManager::AttachToChunkList(
    TChunkList* chunkList,
    const TChunkTreeRef childRef)
{
    Impl->AttachToChunkList(chunkList, childRef);
}

void TChunkManager::ClearChunkList(TChunkList* chunkList)
{
    Impl->ClearChunkList(chunkList);
}

void TChunkManager::SetChunkTreeParent(TChunkList* parent, TChunkTreeRef childRef)
{
    return Impl->SetChunkTreeParent(parent, childRef);
}

void TChunkManager::ResetChunkTreeParent(TChunkList* parent, TChunkTreeRef childRef)
{
    return Impl->ResetChunkTreeParent(parent, childRef);
}

void TChunkManager::ScheduleJobs(
    THolder* holder,
    const std::vector<TJobInfo>& runningJobs,
    std::vector<TJobStartInfo>* jobsToStart,
    std::vector<TJobStopInfo>* jobsToStop)
{
    Impl->ScheduleJobs(
        holder,
        runningJobs,
        jobsToStart,
        jobsToStop);
}

bool TChunkManager::IsReplicatorEnabled()
{
    return Impl->IsReplicatorEnabled();
}

void TChunkManager::FillNodeAddresses(
    ::google::protobuf::RepeatedPtrField< TProtoStringType>* addresses,
    const TChunk* chunk)
{
    Impl->FillHolderAddresses(addresses, chunk);
}

TTotalHolderStatistics TChunkManager::GetTotalHolderStatistics()
{
    return Impl->GetTotalHolderStatistics();
}

bool TChunkManager::IsHolderConfirmed(const THolder* holder)
{
    return Impl->IsHolderConfirmed(holder);
}

i32 TChunkManager::GetChunkReplicaCount()
{
    return Impl->GetChunkReplicaCount();
}

DELEGATE_METAMAP_ACCESSORS(TChunkManager, Chunk, TChunk, TChunkId, *Impl)
DELEGATE_METAMAP_ACCESSORS(TChunkManager, ChunkList, TChunkList, TChunkListId, *Impl)
DELEGATE_METAMAP_ACCESSORS(TChunkManager, Holder, THolder, THolderId, *Impl)
DELEGATE_METAMAP_ACCESSORS(TChunkManager, JobList, TJobList, TChunkId, *Impl)
DELEGATE_METAMAP_ACCESSORS(TChunkManager, Job, TJob, TJobId, *Impl)

DELEGATE_SIGNAL(TChunkManager, void(const THolder*), HolderRegistered, *Impl);
DELEGATE_SIGNAL(TChunkManager, void(const THolder*), HolderUnregistered, *Impl);

DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunkId>, LostChunkIds, *Impl);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunkId>, OverreplicatedChunkIds, *Impl);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunkId>, UnderreplicatedChunkIds, *Impl);

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
