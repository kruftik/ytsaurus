#pragma once

#include "public.h"

#include <yt/server/node/exec_agent/public.h>

#include <yt/server/node/data_node/public.h>

#include <yt/server/node/job_agent/public.h>

#include <yt/server/node/query_agent/public.h>

#include <yt/server/node/tablet_node/public.h>

#include <yt/server/lib/job_proxy/public.h>

#include <yt/ytlib/api/native/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/misc/public.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/query_client/public.h>

#include <yt/ytlib/monitoring/public.h>

#include <yt/core/bus/public.h>

#include <yt/core/http/public.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/throughput_throttler.h>
#include <yt/core/concurrency/two_level_fair_share_thread_pool.h>

#include <yt/core/rpc/public.h>

#include <yt/core/ytree/public.h>

#include <yt/core/misc/public.h>
#include <yt/core/misc/lazy_ptr.h>

namespace NYT::NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    TBootstrap(TCellNodeConfigPtr config, NYTree::INodePtr configNode);
    ~TBootstrap();

    const TCellNodeConfigPtr& GetConfig() const;
    const IInvokerPtr& GetControlInvoker() const;
    IInvokerPtr GetQueryPoolInvoker(
        const TString& poolName,
        double weight,
        const NConcurrency::TFairShareThreadPoolTag& tag) const;
    const IInvokerPtr& GetLookupPoolInvoker() const;
    const IInvokerPtr& GetTableReplicatorPoolInvoker() const;
    const IInvokerPtr& GetTransactionTrackerInvoker() const;
    const IInvokerPtr& GetStorageHeavyInvoker() const;
    const IInvokerPtr& GetStorageLightInvoker() const;
    const NApi::NNative::IClientPtr& GetMasterClient() const;
    const NApi::NNative::IConnectionPtr& GetMasterConnection() const;
    const NRpc::IServerPtr& GetRpcServer() const;
    const NYTree::IMapNodePtr& GetOrchidRoot() const;
    const NJobAgent::TJobControllerPtr& GetJobController() const;
    const NJobAgent::TStatisticsReporterPtr& GetStatisticsReporter() const;
    const NTabletNode::TSlotManagerPtr& GetTabletSlotManager() const;
    const NTabletNode::TSecurityManagerPtr& GetSecurityManager() const;
    const NTabletNode::IInMemoryManagerPtr& GetInMemoryManager() const;
    const NTabletNode::TVersionedChunkMetaManagerPtr& GetVersionedChunkMetaManager() const;
    const NExecAgent::TSlotManagerPtr& GetExecSlotManager() const;
    const NJobAgent::TGpuManagerPtr& GetGpuManager() const;
    TNodeMemoryTracker* GetMemoryUsageTracker() const;
    const NDataNode::TChunkStorePtr& GetChunkStore() const;
    const NDataNode::TChunkCachePtr& GetChunkCache() const;
    const NDataNode::TChunkRegistryPtr& GetChunkRegistry() const;
    const NDataNode::TSessionManagerPtr& GetSessionManager() const;
    const NDataNode::TChunkMetaManagerPtr& GetChunkMetaManager() const;
    const NDataNode::TChunkBlockManagerPtr& GetChunkBlockManager() const;
    const NDataNode::TNetworkStatisticsPtr& GetNetworkStatistics() const;
    const NChunkClient::IBlockCachePtr& GetBlockCache() const;
    const NTableClient::TBlockMetaCachePtr& GetBlockMetaCache() const;
    const NDataNode::TPeerBlockDistributorPtr& GetPeerBlockDistributor() const;
    const NDataNode::TPeerBlockTablePtr& GetPeerBlockTable() const;
    const NDataNode::TPeerBlockUpdaterPtr& GetPeerBlockUpdater() const;
    const NDataNode::TBlobReaderCachePtr& GetBlobReaderCache() const;
    const NDataNode::TJournalDispatcherPtr& GetJournalDispatcher() const;
    const NDataNode::TMasterConnectorPtr& GetMasterConnector() const;
    const NQueryClient::TColumnEvaluatorCachePtr& GetColumnEvaluatorCache() const;
    const NQueryAgent::IQuerySubexecutorPtr& GetQueryExecutor() const;
    const NNodeTrackerClient::TNodeDirectoryPtr& GetNodeDirectory() const;

    const NConcurrency::IThroughputThrottlerPtr& GetReplicationInThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetReplicationOutThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetRepairInThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetRepairOutThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetArtifactCacheInThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetArtifactCacheOutThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetSkynetOutThrottler() const;

    const NConcurrency::IThroughputThrottlerPtr& GetInThrottler(const TWorkloadDescriptor& descriptor) const;
    const NConcurrency::IThroughputThrottlerPtr& GetOutThrottler(const TWorkloadDescriptor& descriptor) const;

    const NConcurrency::IThroughputThrottlerPtr& GetTabletNodeInThrottler(EWorkloadCategory category) const;
    const NConcurrency::IThroughputThrottlerPtr& GetTabletNodeOutThrottler(EWorkloadCategory category) const;

    const NConcurrency::IThroughputThrottlerPtr& GetReadRpsOutThrottler() const;

    NObjectClient::TCellId GetCellId() const;
    NObjectClient::TCellId GetCellId(NObjectClient::TCellTag cellTag) const;
    NNodeTrackerClient::TNetworkPreferenceList GetLocalNetworks();
    std::optional<TString> GetDefaultNetworkName();

    NJobProxy::TJobProxyConfigPtr BuildJobProxyConfig() const;

    NTransactionClient::TTimestamp GetLatestTimestamp() const;

    void Run();

private:
    const TCellNodeConfigPtr Config;
    const NYTree::INodePtr ConfigNode;

    NConcurrency::TActionQueuePtr ControlQueue;
    TLazyIntrusivePtr<NConcurrency::ITwoLevelFairShareThreadPool> QueryThreadPool;
    NConcurrency::TThreadPoolPtr LookupThreadPool;
    NConcurrency::TThreadPoolPtr TableReplicatorThreadPool;
    NConcurrency::TActionQueuePtr TransactionTrackerQueue;
    NConcurrency::TThreadPoolPtr StorageHeavyThreadPool;
    NConcurrency::TThreadPoolPtr StorageLightThreadPool;

    NMonitoring::TMonitoringManagerPtr MonitoringManager_;
    NBus::IBusServerPtr BusServer;
    NApi::NNative::IConnectionPtr MasterConnection;
    NApi::NNative::IClientPtr MasterClient;
    NRpc::IServerPtr RpcServer;
    std::vector<NRpc::IServicePtr> MasterCacheServices;
    NHttp::IServerPtr HttpServer;
    NHttp::IServerPtr SkynetHttpServer;
    NYTree::IMapNodePtr OrchidRoot;
    NJobAgent::TJobControllerPtr JobController;
    NJobAgent::TStatisticsReporterPtr StatisticsReporter;
    NExecAgent::TSlotManagerPtr ExecSlotManager;
    NJobAgent::TGpuManagerPtr GpuManager;
    NJobProxy::TJobProxyConfigPtr JobProxyConfigTemplate;
    NNodeTrackerClient::TNodeMemoryTrackerPtr MemoryUsageTracker;
    NExecAgent::TSchedulerConnectorPtr SchedulerConnector;
    NDataNode::TChunkStorePtr ChunkStore;
    NDataNode::TChunkCachePtr ChunkCache;
    NDataNode::TChunkRegistryPtr ChunkRegistry;
    NDataNode::TSessionManagerPtr SessionManager;
    NDataNode::TChunkMetaManagerPtr ChunkMetaManager;
    NDataNode::TChunkBlockManagerPtr ChunkBlockManager;
    NDataNode::TNetworkStatisticsPtr NetworkStatistics;
    NChunkClient::IBlockCachePtr BlockCache;
    NTableClient::TBlockMetaCachePtr BlockMetaCache;
    NDataNode::TPeerBlockTablePtr PeerBlockTable;
    NDataNode::TPeerBlockUpdaterPtr PeerBlockUpdater;
    NDataNode::TPeerBlockDistributorPtr PeerBlockDistributor;
    NDataNode::TBlobReaderCachePtr BlobReaderCache;
    NDataNode::TJournalDispatcherPtr JournalDispatcher;
    NDataNode::TMasterConnectorPtr MasterConnector;
    ICoreDumperPtr CoreDumper;

    NConcurrency::IThroughputThrottlerPtr TotalInThrottler;
    NConcurrency::IThroughputThrottlerPtr TotalOutThrottler;
    NConcurrency::IThroughputThrottlerPtr ReplicationInThrottler;
    NConcurrency::IThroughputThrottlerPtr ReplicationOutThrottler;
    NConcurrency::IThroughputThrottlerPtr RepairInThrottler;
    NConcurrency::IThroughputThrottlerPtr RepairOutThrottler;
    NConcurrency::IThroughputThrottlerPtr ArtifactCacheInThrottler;
    NConcurrency::IThroughputThrottlerPtr ArtifactCacheOutThrottler;
    NConcurrency::IThroughputThrottlerPtr SkynetOutThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletCompactionAndPartitioningInThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletCompactionAndPartitioningOutThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletStoreFlushInThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletLoggingInThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletPreloadOutThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletSnapshotInThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletRecoveryOutThrottler;
    NConcurrency::IThroughputThrottlerPtr DataNodeTabletReplicationOutThrottler;

    NConcurrency::IThroughputThrottlerPtr TabletNodeCompactionAndPartitioningInThrottler;
    NConcurrency::IThroughputThrottlerPtr TabletNodeCompactionAndPartitioningOutThrottler;
    NConcurrency::IThroughputThrottlerPtr TabletNodeStoreFlushOutThrottler;
    NConcurrency::IThroughputThrottlerPtr TabletNodePreloadInThrottler;
    NConcurrency::IThroughputThrottlerPtr TabletNodeTabletReplicationInThrottler;
    NConcurrency::IThroughputThrottlerPtr TabletNodeTabletReplicationOutThrottler;

    NConcurrency::IThroughputThrottlerPtr ReadRpsOutThrottler;

    NTabletNode::TSlotManagerPtr TabletSlotManager;
    NTabletNode::TSecurityManagerPtr SecurityManager;
    NTabletNode::IInMemoryManagerPtr InMemoryManager;
    NTabletNode::TVersionedChunkMetaManagerPtr VersionedChunkMetaManager;

    NQueryClient::TColumnEvaluatorCachePtr ColumnEvaluatorCache;
    NQueryAgent::IQuerySubexecutorPtr QueryExecutor;

    NConcurrency::TPeriodicExecutorPtr FootprintUpdateExecutor;

    void DoRun();
    void PopulateAlerts(std::vector<TError>* alerts);

    void OnMasterConnected();
    void OnMasterDisconnected();

    void UpdateFootprintMemoryUsage();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
