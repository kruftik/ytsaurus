#pragma once

#include <yt/yt/library/profiling/sensor.h>

#include <yt/yt/core/logging/log.h>

namespace NYT::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, RpcProxyLogger, "RpcProxy");
inline const NProfiling::TProfiler RpcProxyProfiler("/rpc_proxy");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
