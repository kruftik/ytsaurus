#pragma once

#include "public.h"
#include "profiler.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NNbd {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, NbdLogger, "Nbd");

inline const NProfiling::TProfiler NbdProfiler("/nbd");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNbd
