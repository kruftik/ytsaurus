#include "stdafx.h"
#include "peer_block_updater.h"
#include "common.h"
#include "block_store.h"
#include "chunk_holder_service_proxy.h"
#include "config.h"

#include <ytlib/rpc/channel_cache.h>

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;
static NRpc::TChannelCache ChannelCache;

////////////////////////////////////////////////////////////////////////////////

TPeerBlockUpdater::TPeerBlockUpdater(
    TChunkHolderConfig* config,
    TBlockStore* blockStore,
    IInvoker* invoker)
    : Config(config)
    , BlockStore(blockStore)
{
    PeriodicInvoker = New<TPeriodicInvoker>(
        // TODO(babenko): use AsStrong
        FromMethod(&TPeerBlockUpdater::Update, TIntrusivePtr<TPeerBlockUpdater>(this))
        ->Via(invoker),
        Config->PeerUpdatePeriod);
}

void TPeerBlockUpdater::Start()
{
    PeriodicInvoker->Start();
}

void TPeerBlockUpdater::Stop()
{
    PeriodicInvoker->Stop();
}

void TPeerBlockUpdater::Update()
{
    LOG_INFO("Updating peer blocks");

    auto expirationTime = Config->PeerUpdateExpirationTimeout.ToDeadLine();

    yhash_map<Stroka, TProxy::TReqUpdatePeer::TPtr> requests;

    auto blocks = BlockStore->GetAllBlocks();
    FOREACH (auto block, blocks) {
        const auto& source = block->Source();
        if (!source.Empty()) {
            TProxy::TReqUpdatePeer::TPtr request;
            auto it = requests.find(source);
            if (it != requests.end()) {
                request = it->second;
            } else {
                TProxy proxy(~ChannelCache.GetChannel(source));
                request = proxy.UpdatePeer();
                request->set_peer_address(Config->MasterConnector->PeerAddress);
                request->set_peer_expiration_time(expirationTime.GetValue());
                requests.insert(MakePair(source, request));
            }
            auto* block_id = request->add_block_ids();
            const auto& blockId = block->GetKey();
            block_id->set_chunk_id(blockId.ChunkId.ToProto());
            block_id->set_block_index(blockId.BlockIndex);
        }
    }

    FOREACH (const auto& pair, requests) {
        LOG_DEBUG("Sending peer block update request (Address: %s, ExpirationTime: %s)",
            ~pair.first,
            ~expirationTime.ToString());
        pair.second->Invoke();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
