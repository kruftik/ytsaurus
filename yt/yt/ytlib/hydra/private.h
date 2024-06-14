#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, HydraLogger, "Hydra");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
