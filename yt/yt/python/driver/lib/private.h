#pragma once

#include <yt/yt/core/logging/log.h>

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, DriverLogger, "PythonDriver");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
