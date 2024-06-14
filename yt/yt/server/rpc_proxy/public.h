#pragma once

#include <yt/yt/server/lib/rpc_proxy/public.h>

namespace NYT::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TProxyConfig)
DECLARE_REFCOUNTED_CLASS(TProxyDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TProxyMemoryLimits)
DECLARE_REFCOUNTED_CLASS(TAccessCheckerConfig)
DECLARE_REFCOUNTED_CLASS(TAccessCheckerDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TDiscoveryServiceConfig)
DECLARE_REFCOUNTED_CLASS(TBundleProxyDynamicConfig)
DECLARE_REFCOUNTED_CLASS(IBundleDynamicConfigManager)

DECLARE_REFCOUNTED_STRUCT(IDynamicConfigManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
