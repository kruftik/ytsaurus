#include "chunk_meta_fetcher.h"

#include "config.h"
#include "input_chunk.h"
#include "data_node_service_proxy.h"
#include "helpers.h"

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/rpc/helpers.h>

#include <yt/yt/ytlib/chunk_client/data_node_service_proxy.h>

#include <yt/yt/core/rpc/public.h>

#include <util/generic/cast.h>

namespace NYT::NChunkClient {

using namespace NNodeTrackerClient;
using namespace NRpc;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TChunkMetaFetcher::TChunkMetaFetcher(
    TFetcherConfigPtr config,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    IFetcherChunkScraperPtr chunkScraper,
    NApi::NNative::IClientPtr client,
    const NLogging::TLogger& logger,
    TWorkloadDescriptor workloadDescriptor,
    std::function<void(TReqGetChunkMeta&)> initializeRequest)
    : TFetcherBase(
        std::move(config),
        std::move(nodeDirectory),
        std::move(invoker),
        std::move(chunkScraper),
        std::move(client),
        logger)
    , WorkloadDescriptor_(std::move(workloadDescriptor))
    , InitializeRequest_(std::move(initializeRequest))
{ }

void TChunkMetaFetcher::ProcessDynamicStore(int /*chunkIndex*/)
{
    YT_ABORT();
}

TFuture<void> TChunkMetaFetcher::FetchFromNode(TNodeId nodeId, std::vector<int> chunkIndexes)
{
    YT_LOG_DEBUG("Fetching chunk metas from node (NodeId: %v, ChunkIndexes: %v)",
        nodeId,
        chunkIndexes);

    TDataNodeServiceProxy proxy(GetNodeChannel(nodeId));
    proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

    // TODO(max42): optimize it.

    std::vector<TFuture<TDataNodeServiceProxy::TRspGetChunkMetaPtr>> asyncResults;

    for (int index : chunkIndexes) {
        const auto& chunk = Chunks_[index];

        auto chunkId = EncodeChunkId(chunk, nodeId);

        auto req = proxy.GetChunkMeta();
        SetRequestWorkloadDescriptor(req, WorkloadDescriptor_);
        // TODO(babenko): consider using light band instead when all metas become thin
        // CC: psushin@
        req->SetMultiplexingBand(EMultiplexingBand::Heavy);
        req->set_enable_throttling(true);
        ToProto(req->mutable_chunk_id(), chunkId);
        req->set_supported_chunk_features(ToUnderlying(GetSupportedChunkFeatures()));
        InitializeRequest_(*req);

        asyncResults.push_back(req->Invoke());
    }

    return AllSucceeded(std::move(asyncResults))
        .Apply(BIND(&TChunkMetaFetcher::OnResponse, MakeStrong(this), nodeId, chunkIndexes)
            .AsyncVia(Invoker_));
}

void TChunkMetaFetcher::OnFetchingStarted()
{
    ChunkMetas_.resize(Chunks_.size());
}

void TChunkMetaFetcher::OnResponse(
    NYT::NNodeTrackerClient::TNodeId nodeId,
    std::vector<int> requestedChunkIndexes,
    const TErrorOr<std::vector<TDataNodeServiceProxy::TRspGetChunkMetaPtr>>& rspOrError)
{
    YT_LOG_DEBUG("Node response received (NodeId: %v, ChunkIndexes: %v)",
        nodeId,
        requestedChunkIndexes);

    if (!rspOrError.IsOK()) {
        YT_LOG_INFO("Failed to get chunk slices meta from node (Address: %v, NodeId: %v)",
            NodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress(),
            nodeId);

        OnNodeFailed(nodeId, requestedChunkIndexes);
        return;
    }

    const auto& responses = rspOrError.Value();

    YT_VERIFY(responses.size() == requestedChunkIndexes.size());

    std::vector<int> throttledChunkIndexes;

    for (int index = 0; index < std::ssize(requestedChunkIndexes); ++index) {
        int chunkIndex = requestedChunkIndexes[index];
        auto& rsp = responses[index];
        if (rsp->net_throttling()) {
            throttledChunkIndexes.push_back(chunkIndex);
            continue;
        }
        YT_VERIFY(chunkIndex < std::ssize(ChunkMetas_));
        ChunkMetas_[chunkIndex] = New<TRefCountedChunkMeta>(std::move(rsp->chunk_meta()));
    }

    if (!throttledChunkIndexes.empty()) {
        OnRequestThrottled(nodeId, throttledChunkIndexes);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
