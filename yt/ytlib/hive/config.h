#pragma once

#include "public.h"

#include <ytlib/hydra/config.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

class TRemoteTimestampProviderConfig
    : public NHydra::TPeerDiscoveryConfig
{ };

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
