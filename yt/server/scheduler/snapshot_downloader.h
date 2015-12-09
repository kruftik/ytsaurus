#pragma once

#include "private.h"

#include <yt/server/cell_scheduler/public.h>

#include <yt/core/logging/log.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotDownloader
    : public TRefCounted
{
public:
    TSnapshotDownloader(
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap,
        TOperationPtr operation);

    void Run();

private:
    const TSchedulerConfigPtr Config_;
    NCellScheduler::TBootstrap* const Bootstrap_;
    const TOperationPtr Operation_;

    NLogging::TLogger Logger;

};

DEFINE_REFCOUNTED_TYPE(TSnapshotDownloader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
