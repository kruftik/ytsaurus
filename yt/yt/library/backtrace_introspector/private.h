#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NBacktraceIntrospector {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, BacktraceIntrospectorLogger, "BacktraceIntrospector");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NBacktraceIntrospector

