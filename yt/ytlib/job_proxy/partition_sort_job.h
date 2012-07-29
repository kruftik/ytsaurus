#pragma once 

#include "public.h"
#include "job.h"

#include <ytlib/table_client/public.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

TAutoPtr<IJob> CreatePartitionSortJob(IJobHost* host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
