#pragma once

#include "public.h"

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateSortedMergeJob(IJobHost* host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
