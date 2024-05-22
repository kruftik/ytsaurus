#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NFileClient {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, FileClientLogger, "FileClient");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFileClient

