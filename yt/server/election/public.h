#pragma once

#include <core/misc/public.h>

#include <ytlib/election/public.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IElectionCallbacks)
DECLARE_REFCOUNTED_STRUCT(IElectionManager)

DECLARE_REFCOUNTED_STRUCT(TEpochContext)

DECLARE_REFCOUNTED_CLASS(TDistributedElectionManager)
DECLARE_REFCOUNTED_CLASS(TElectionManagerThunk)

DECLARE_REFCOUNTED_CLASS(TDistributedElectionManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
