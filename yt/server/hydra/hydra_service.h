#pragma once

#include "public.h"

#include <yt/core/rpc/service_detail.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class THydraServiceBase
    : public NRpc::TServiceBase
{
protected:
    THydraServiceBase(
        IInvokerPtr invoker,
        const NRpc::TServiceId& serviceId,
        const NLogging::TLogger& logger,
        int protocolVersion = NRpc::TProxyBase::DefaultProtocolVersion);

    void ValidatePeer(EPeerKind kind);
    void SyncWithUpstream();

    virtual IHydraManagerPtr GetHydraManager() = 0;
    
private:
    virtual bool IsUp(TCtxDiscoverPtr context) override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
