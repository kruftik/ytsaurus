#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NFileServer {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, FileServerLogger, "FileServer");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFileServer

