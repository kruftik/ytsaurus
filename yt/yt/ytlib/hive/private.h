#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NHiveClient {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, HiveClientLogger, "HiveClient");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveClient
