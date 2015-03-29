#pragma once

#include "public.h"

#include <core/rpc/public.h>

#include <core/actions/callback.h>

#include <ytlib/chunk_client/public.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/hive/public.h>

#include <ytlib/tablet_client/public.h>

#include <ytlib/query_client/public.h>

#include <ytlib/security_client/public.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

struct TAdminOptions
{ };

struct TClientOptions
{
    Stroka User = NSecurityClient::GuestUserName;
};

TClientOptions GetRootClientOptions();

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMasterChannelKind,
    (Leader)
    (LeaderOrFollower)
    (Cache)
);

//! Represents an established connection with a YT cluster.
/*
 *  IConnection instance caches most of the stuff needed for fast interaction
 *  with the cluster (e.g. connection channels, mount info etc).
 *  
 *  Thread affinity: any
 */
struct IConnection
    : public virtual TRefCounted
{
    virtual TConnectionConfigPtr GetConfig() = 0;
    virtual NRpc::IChannelPtr GetMasterChannel(EMasterChannelKind kind) = 0;
    virtual NRpc::IChannelPtr GetSchedulerChannel() = 0;
    virtual NRpc::IChannelFactoryPtr GetNodeChannelFactory() = 0;
    virtual NChunkClient::IBlockCachePtr GetBlockCache() = 0;
    virtual NTabletClient::TTableMountCachePtr GetTableMountCache() = 0;
    virtual NTransactionClient::ITimestampProviderPtr GetTimestampProvider() = 0;
    virtual NHive::TCellDirectoryPtr GetCellDirectory() = 0;
    virtual NQueryClient::IFunctionRegistryPtr GetFunctionRegistry() = 0;
    virtual NQueryClient::TEvaluatorPtr GetQueryEvaluator() = 0;
    virtual NQueryClient::TColumnEvaluatorCachePtr GetColumnEvaluatorCache() = 0;

    virtual IAdminPtr CreateAdmin(const TAdminOptions& options = TAdminOptions()) = 0;

    virtual IClientPtr CreateClient(const TClientOptions& options = TClientOptions()) = 0;

    virtual void ClearMetadataCaches() = 0;

};

DEFINE_REFCOUNTED_TYPE(IConnection)

IConnectionPtr CreateConnection(
    TConnectionConfigPtr config,
    TCallback<bool(const TError&)> isRetriableError = BIND(&NRpc::IsRetriableError));

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

