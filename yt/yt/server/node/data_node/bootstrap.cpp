#include "bootstrap.h"

#include "ally_replica_manager.h"
#include "blob_reader_cache.h"
#include "chunk_meta_manager.h"
#include "chunk_registry.h"
#include "chunk_store.h"
#include "data_node_service.h"
#include "io_throughput_meter.h"
#include "medium_directory_manager.h"
#include "job.h"
#include "job_controller.h"
#include "journal_dispatcher.h"
#include "master_connector.h"
#include "medium_updater.h"
#include "p2p.h"
#include "private.h"
#include "session_manager.h"
#include "skynet_http_handler.h"
#include "table_schema_cache.h"
#include "ytree_integration.h"
#include "chunk_detail.h"
#include "location.h"

#include <yt/yt/server/node/cluster_node/config.h>
#include <yt/yt/server/node/cluster_node/dynamic_config_manager.h>


#include <yt/yt/ytlib/tablet_client/row_comparer_generator.h>

#include <yt/yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/yt/core/concurrency/fair_share_thread_pool.h>

#include <yt/yt/core/http/server.h>

#include <yt/yt/core/ytree/virtual.h>

namespace NYT::NDataNode {

using namespace NClusterNode;
using namespace NCypressClient;
using namespace NConcurrency;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

// COMPAT(gritukan): Throttlers that were moved out of Data Node during node split.
static const THashSet<EDataNodeThrottlerKind> DataNodeCompatThrottlers = {
    // Cluster Node throttlers.
    EDataNodeThrottlerKind::TotalIn,
    EDataNodeThrottlerKind::TotalOut,
    // Exec Node throttlers.
    EDataNodeThrottlerKind::ArtifactCacheIn,
    EDataNodeThrottlerKind::JobIn,
    EDataNodeThrottlerKind::JobOut,
};

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
    : public IBootstrap
    , public TBootstrapBase
{
public:
    explicit TBootstrap(NClusterNode::IBootstrap* bootstrap)
        : TBootstrapBase(bootstrap)
        , ClusterNodeBootstrap_(bootstrap)
    { }

    void Initialize() override
    {
        YT_LOG_INFO("Initializing data node");

        GetDynamicConfigManager()
            ->SubscribeConfigChanged(BIND(&TBootstrap::OnDynamicConfigChanged, this));

        const auto& dynamicConfig = GetDynamicConfigManager()->GetConfig()->DataNode;

        JournalDispatcher_ = CreateJournalDispatcher(GetConfig()->DataNode, GetDynamicConfigManager());

        ChunkStore_ = New<TChunkStore>(
            GetConfig()->DataNode,
            GetDynamicConfigManager(),
            GetControlInvoker(),
            TChunkContext::Create(this),
            CreateChunkStoreHost(this));

        SessionManager_ = New<TSessionManager>(GetConfig()->DataNode, this);

        JobController_ = CreateJobController(this);

        MasterConnector_ = CreateMasterConnector(this);

        MediumDirectoryManager_ = New<TMediumDirectoryManager>(
            this,
            DataNodeLogger);

        MediumUpdater_ = New<TMediumUpdater>(
            this,
            MediumDirectoryManager_);

        ChunkStore_->Initialize();

        SessionManager_->Initialize();

        if (GetConfig()->EnableFairThrottler) {
            for (auto kind : {
                EDataNodeThrottlerKind::ReplicationIn,
                EDataNodeThrottlerKind::RepairIn,
                EDataNodeThrottlerKind::MergeIn,
                EDataNodeThrottlerKind::AutotomyIn,
                EDataNodeThrottlerKind::ArtifactCacheIn,
                EDataNodeThrottlerKind::TabletCompactionAndPartitioningIn,
                EDataNodeThrottlerKind::TabletLoggingIn,
                EDataNodeThrottlerKind::TabletSnapshotIn,
                EDataNodeThrottlerKind::TabletStoreFlushIn,
                EDataNodeThrottlerKind::JobIn,
            }) {
                Throttlers_[kind] = ClusterNodeBootstrap_->GetInThrottler(FormatEnum(kind));
            }

            for (auto kind : {
                EDataNodeThrottlerKind::ReplicationOut,
                EDataNodeThrottlerKind::RepairOut,
                EDataNodeThrottlerKind::MergeOut,
                EDataNodeThrottlerKind::AutotomyOut,
                EDataNodeThrottlerKind::ArtifactCacheOut,
                EDataNodeThrottlerKind::TabletCompactionAndPartitioningOut,
                EDataNodeThrottlerKind::SkynetOut,
                EDataNodeThrottlerKind::TabletPreloadOut,
                EDataNodeThrottlerKind::TabletRecoveryOut,
                EDataNodeThrottlerKind::TabletReplicationOut,
                EDataNodeThrottlerKind::JobOut,
                EDataNodeThrottlerKind::TabletStoreFlushOut,
            }) {
                Throttlers_[kind] = ClusterNodeBootstrap_->GetOutThrottler(FormatEnum(kind));
            }
        } else {
            for (auto kind : TEnumTraits<EDataNodeThrottlerKind>::GetDomainValues()) {
                if (DataNodeCompatThrottlers.contains(kind)) {
                    continue;
                }

                const auto& throttlerConfig = ClusterNodeBootstrap_->PatchRelativeNetworkThrottlerConfig(
                    GetConfig()->DataNode->Throttlers[kind]);
                LegacyRawThrottlers_[kind] = CreateNamedReconfigurableThroughputThrottler(
                    std::move(throttlerConfig),
                    ToString(kind),
                    DataNodeLogger,
                    DataNodeProfiler.WithPrefix("/throttlers"));
            }

            static const THashSet<EDataNodeThrottlerKind> InCombinedDataNodeThrottlerKinds = {
                EDataNodeThrottlerKind::ReplicationIn,
                EDataNodeThrottlerKind::RepairIn,
                EDataNodeThrottlerKind::MergeIn,
                EDataNodeThrottlerKind::AutotomyIn,
                EDataNodeThrottlerKind::ArtifactCacheIn,
                EDataNodeThrottlerKind::TabletCompactionAndPartitioningIn,
                EDataNodeThrottlerKind::TabletLoggingIn,
                EDataNodeThrottlerKind::TabletSnapshotIn,
                EDataNodeThrottlerKind::TabletStoreFlushIn,
                EDataNodeThrottlerKind::JobIn,
            };
            static const THashSet<EDataNodeThrottlerKind> OutCombinedDataNodeThrottlerKinds = {
                EDataNodeThrottlerKind::ReplicationOut,
                EDataNodeThrottlerKind::RepairOut,
                EDataNodeThrottlerKind::MergeOut,
                EDataNodeThrottlerKind::AutotomyOut,
                EDataNodeThrottlerKind::ArtifactCacheOut,
                EDataNodeThrottlerKind::TabletCompactionAndPartitioningOut,
                EDataNodeThrottlerKind::SkynetOut,
                EDataNodeThrottlerKind::TabletPreloadOut,
                EDataNodeThrottlerKind::TabletRecoveryOut,
                EDataNodeThrottlerKind::TabletReplicationOut,
                EDataNodeThrottlerKind::JobOut,
                EDataNodeThrottlerKind::TabletStoreFlushOut,
            };

            for (auto kind : TEnumTraits<EDataNodeThrottlerKind>::GetDomainValues()) {
                if (DataNodeCompatThrottlers.contains(kind)) {
                    continue;
                }

                auto throttler = IThroughputThrottlerPtr(LegacyRawThrottlers_[kind]);
                if (InCombinedDataNodeThrottlerKinds.contains(kind)) {
                    throttler = CreateCombinedThrottler({GetDefaultInThrottler(), throttler});
                }
                if (OutCombinedDataNodeThrottlerKinds.contains(kind)) {
                    throttler = CreateCombinedThrottler({GetDefaultOutThrottler(), throttler});
                }
                Throttlers_[kind] = throttler;
            }
        }

        // Should be created after throttlers.
        AllyReplicaManager_ = CreateAllyReplicaManager(this);

        StorageLookupThreadPool_ = New<TThreadPool>(
            GetConfig()->DataNode->StorageLookupThreadCount,
            "StorageLookup");
        MasterJobThreadPool_ = New<TThreadPool>(
            dynamicConfig->MasterJobThreadCount,
            "MasterJob");

        P2PActionQueue_ = New<TActionQueue>("P2P");
        P2PBlockCache_ = New<TP2PBlockCache>(
            GetConfig()->DataNode->P2P,
            P2PActionQueue_->GetInvoker(),
            GetMemoryUsageTracker()->WithCategory(EMemoryCategory::P2P));
        P2PSnooper_ = New<TP2PSnooper>(GetConfig()->DataNode->P2P);
        P2PDistributor_ = New<TP2PDistributor>(
            GetConfig()->DataNode->P2P,
            P2PActionQueue_->GetInvoker(),
            this);

        TableSchemaCache_ = New<TTableSchemaCache>(GetConfig()->DataNode->TableSchemaCache);

        RowComparerProvider_ = NTabletClient::CreateRowComparerProvider(GetConfig()->TabletNode->ColumnEvaluatorCache->CGCache);

        GetRpcServer()->RegisterService(CreateDataNodeService(GetConfig()->DataNode, this));

        auto jobsProfiler = DataNodeProfiler.WithPrefix("/master_jobs");
        MasterJobSensors_.AdaptivelyRepairedChunksCounter = jobsProfiler.Counter("/adaptively_repaired_chunks");
        MasterJobSensors_.TotalRepairedChunksCounter = jobsProfiler.Counter("/total_repaired_chunks");
        MasterJobSensors_.FailedRepairChunksCounter = jobsProfiler.Counter("/failed_repair_chunks");

        auto createMasterJob = BIND([this] (
            NJobAgent::TJobId jobId,
            NJobAgent::TOperationId /*operationId*/,
            const TString& jobTrackerAddress,
            const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
            NJobTrackerClient::NProto::TJobSpec&& jobSpec) -> TMasterJobBasePtr
        {
            return CreateMasterJob(
                jobId,
                std::move(jobSpec),
                jobTrackerAddress,
                resourceLimits,
                GetConfig()->DataNode,
                this,
                MasterJobSensors_);
        });
        GetJobController()->RegisterJobFactory(NJobAgent::EJobType::RemoveChunk, createMasterJob);
        GetJobController()->RegisterJobFactory(NJobAgent::EJobType::ReplicateChunk, createMasterJob);
        GetJobController()->RegisterJobFactory(NJobAgent::EJobType::RepairChunk, createMasterJob);
        GetJobController()->RegisterJobFactory(NJobAgent::EJobType::SealChunk, createMasterJob);
        GetJobController()->RegisterJobFactory(NJobAgent::EJobType::MergeChunks, createMasterJob);
        GetJobController()->RegisterJobFactory(NJobAgent::EJobType::AutotomizeChunk, createMasterJob);

        IOThroughputMeter_ = CreateIOThroughputMeter(
            GetDynamicConfigManager(),
            ChunkStore_,
            DataNodeLogger.WithTag("IOMeter"));
        JobController_->Initialize();
    }

    void Run() override
    {
        SkynetHttpServer_ = NHttp::CreateServer(GetConfig()->CreateSkynetHttpServerConfig());
        SkynetHttpServer_->AddHandler(
            "/read_skynet_part",
            MakeSkynetHttpHandler(this));

        SetNodeByYPath(
            GetOrchidRoot(),
            "/stored_chunks",
            CreateVirtualNode(CreateStoredChunkMapService(ChunkStore_, GetAllyReplicaManager())
                ->Via(GetControlInvoker())));

        SetNodeByYPath(
            GetOrchidRoot(),
            "/ally_replica_manager",
            CreateVirtualNode(AllyReplicaManager_->GetOrchidService()));

        MasterConnector_->Initialize();

        P2PDistributor_->Start();

        SkynetHttpServer_->Start();

        AllyReplicaManager_->Start();
    }

    const TChunkStorePtr& GetChunkStore() const override
    {
        return ChunkStore_;
    }

    const IAllyReplicaManagerPtr& GetAllyReplicaManager() const override
    {
        return AllyReplicaManager_;
    }

    const TSessionManagerPtr& GetSessionManager() const override
    {
        return SessionManager_;
    }

    const IJobControllerPtr& GetJobController() const override
    {
        return JobController_;
    }

    const IMasterConnectorPtr& GetMasterConnector() const override
    {
        return MasterConnector_;
    }

    const TMediumDirectoryManagerPtr& GetMediumDirectoryManager() const override
    {
        return MediumDirectoryManager_;
    }

    const TMediumUpdaterPtr& GetMediumUpdater() const override
    {
        return MediumUpdater_;
    }

    const IThroughputThrottlerPtr& GetThrottler(EDataNodeThrottlerKind kind) const override
    {
        return Throttlers_[kind];
    }

    const IThroughputThrottlerPtr& GetInThrottler(const TWorkloadDescriptor& descriptor) const override
    {
        static const THashMap<EWorkloadCategory, EDataNodeThrottlerKind> WorkloadCategoryToThrottlerKind = {
            {EWorkloadCategory::SystemRepair,                EDataNodeThrottlerKind::RepairIn},
            {EWorkloadCategory::SystemReplication,           EDataNodeThrottlerKind::ReplicationIn},
            {EWorkloadCategory::SystemArtifactCacheDownload, EDataNodeThrottlerKind::ArtifactCacheIn},
            {EWorkloadCategory::SystemTabletCompaction,      EDataNodeThrottlerKind::TabletCompactionAndPartitioningIn},
            {EWorkloadCategory::SystemTabletPartitioning,    EDataNodeThrottlerKind::TabletCompactionAndPartitioningIn},
            {EWorkloadCategory::SystemTabletLogging,         EDataNodeThrottlerKind::TabletLoggingIn},
            {EWorkloadCategory::SystemTabletSnapshot,        EDataNodeThrottlerKind::TabletSnapshotIn},
            {EWorkloadCategory::SystemTabletStoreFlush,      EDataNodeThrottlerKind::TabletStoreFlushIn}
        };
        auto it = WorkloadCategoryToThrottlerKind.find(descriptor.Category);
        return it == WorkloadCategoryToThrottlerKind.end()
            ? GetDefaultInThrottler()
            : Throttlers_[it->second];
    }

    const IThroughputThrottlerPtr& GetOutThrottler(const TWorkloadDescriptor& descriptor) const override
    {
        static const THashMap<EWorkloadCategory, EDataNodeThrottlerKind> WorkloadCategoryToThrottlerKind = {
            {EWorkloadCategory::SystemRepair,                EDataNodeThrottlerKind::RepairOut},
            {EWorkloadCategory::SystemReplication,           EDataNodeThrottlerKind::ReplicationOut},
            {EWorkloadCategory::SystemArtifactCacheDownload, EDataNodeThrottlerKind::ArtifactCacheOut},
            {EWorkloadCategory::SystemTabletCompaction,      EDataNodeThrottlerKind::TabletCompactionAndPartitioningOut},
            {EWorkloadCategory::SystemTabletPartitioning,    EDataNodeThrottlerKind::TabletCompactionAndPartitioningOut},
            {EWorkloadCategory::SystemTabletPreload,         EDataNodeThrottlerKind::TabletPreloadOut},
            {EWorkloadCategory::SystemTabletRecovery,        EDataNodeThrottlerKind::TabletRecoveryOut},
            {EWorkloadCategory::SystemTabletReplication,     EDataNodeThrottlerKind::TabletReplicationOut},
            {EWorkloadCategory::SystemTabletStoreFlush,      EDataNodeThrottlerKind::TabletStoreFlushOut}
        };
        auto it = WorkloadCategoryToThrottlerKind.find(descriptor.Category);
        return it == WorkloadCategoryToThrottlerKind.end()
            ? GetDefaultOutThrottler()
            : Throttlers_[it->second];
    }

    const IJournalDispatcherPtr& GetJournalDispatcher() const override
    {
        return JournalDispatcher_;
    }

    const IInvokerPtr& GetStorageLookupInvoker() const override
    {
        return StorageLookupThreadPool_->GetInvoker();
    }

    const IInvokerPtr& GetMasterJobInvoker() const override
    {
        return MasterJobThreadPool_->GetInvoker();
    }

    const TP2PBlockCachePtr& GetP2PBlockCache() const override
    {
        return P2PBlockCache_;
    }

    const TP2PSnooperPtr& GetP2PSnooper() const override
    {
        return P2PSnooper_;
    }

    const TTableSchemaCachePtr& GetTableSchemaCache() const override
    {
        return TableSchemaCache_;
    }

    const NTabletClient::IRowComparerProviderPtr& GetRowComparerProvider() const override
    {
        return RowComparerProvider_;
    }

    const IIOThroughputMeterPtr& GetIOThroughputMeter() const override
    {
        return IOThroughputMeter_;
    }

private:
    NClusterNode::IBootstrap* const ClusterNodeBootstrap_;

    TChunkStorePtr ChunkStore_;
    IAllyReplicaManagerPtr AllyReplicaManager_;

    TSessionManagerPtr SessionManager_;

    IJobControllerPtr JobController_;

    IMasterConnectorPtr MasterConnector_;
    TMediumDirectoryManagerPtr MediumDirectoryManager_;
    TMediumUpdaterPtr MediumUpdater_;

    TEnumIndexedVector<EDataNodeThrottlerKind, IReconfigurableThroughputThrottlerPtr> LegacyRawThrottlers_;
    TEnumIndexedVector<EDataNodeThrottlerKind, IThroughputThrottlerPtr> Throttlers_;

    IJournalDispatcherPtr JournalDispatcher_;

    TThreadPoolPtr StorageLookupThreadPool_;
    TThreadPoolPtr MasterJobThreadPool_;

    TActionQueuePtr P2PActionQueue_;
    TP2PBlockCachePtr P2PBlockCache_;
    TP2PSnooperPtr P2PSnooper_;
    TP2PDistributorPtr P2PDistributor_;

    TTableSchemaCachePtr TableSchemaCache_;

    NTabletClient::IRowComparerProviderPtr RowComparerProvider_;

    NHttp::IServerPtr SkynetHttpServer_;

    IIOThroughputMeterPtr IOThroughputMeter_;

    TMasterJobSensors MasterJobSensors_;

    void OnDynamicConfigChanged(
        const TClusterNodeDynamicConfigPtr& /*oldConfig*/,
        const TClusterNodeDynamicConfigPtr& newConfig)
    {
        if (!GetConfig()->EnableFairThrottler) {
            for (auto kind : TEnumTraits<NDataNode::EDataNodeThrottlerKind>::GetDomainValues()) {
                if (DataNodeCompatThrottlers.contains(kind)) {
                    continue;
                }

                const auto& throttlerConfig = newConfig->DataNode->Throttlers[kind]
                    ? newConfig->DataNode->Throttlers[kind]
                    : GetConfig()->DataNode->Throttlers[kind];
                auto patchedThrottlerConfig = ClusterNodeBootstrap_->PatchRelativeNetworkThrottlerConfig(throttlerConfig);
                LegacyRawThrottlers_[kind]->Reconfigure(std::move(patchedThrottlerConfig));
            }
        }

        StorageLookupThreadPool_->Configure(
            newConfig->DataNode->StorageLookupThreadCount.value_or(GetConfig()->DataNode->StorageLookupThreadCount));
        MasterJobThreadPool_->Configure(newConfig->DataNode->MasterJobThreadCount);

        TableSchemaCache_->Configure(newConfig->DataNode->TableSchemaCache);

        P2PBlockCache_->UpdateConfig(newConfig->DataNode->P2P);
        P2PSnooper_->UpdateConfig(newConfig->DataNode->P2P);
        P2PDistributor_->UpdateConfig(newConfig->DataNode->P2P);

        ChunkStore_->UpdateConfig(newConfig->DataNode);
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IBootstrap> CreateBootstrap(NClusterNode::IBootstrap* bootstrap)
{
    return std::make_unique<TBootstrap>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
