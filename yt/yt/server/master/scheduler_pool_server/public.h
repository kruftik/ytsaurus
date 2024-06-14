#pragma once

#include <yt/yt/client/object_client/public.h>

#include <yt/yt/server/lib/hydra/public.h>

namespace NYT::NSchedulerPoolServer {

////////////////////////////////////////////////////////////////////////////////

using TSchedulerPoolId = NObjectClient::TObjectId;

DECLARE_ENTITY_TYPE(TSchedulerPool, TSchedulerPoolId, NObjectClient::TObjectIdEntropyHash)
DECLARE_ENTITY_TYPE(TSchedulerPoolTree, TSchedulerPoolId, NObjectClient::TObjectIdEntropyHash)

DECLARE_REFCOUNTED_STRUCT(ISchedulerPoolManager)

DECLARE_REFCOUNTED_CLASS(TDynamicSchedulerPoolManagerConfig)

DECLARE_REFCOUNTED_CLASS(TPoolResources)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSchedulerPoolServer
