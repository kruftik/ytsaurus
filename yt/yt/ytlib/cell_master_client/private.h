#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NCellMasterClient {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, CellMasterClientLogger, "CellMasterClient");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMasterClient
