#pragma once

#include <yt/ytlib/scheduler/public.h>

#include <yt/core/misc/enum.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TUserJobIO;

DECLARE_REFCOUNTED_CLASS(TJobProxyConfig)

DECLARE_REFCOUNTED_STRUCT(IJob)
DECLARE_REFCOUNTED_STRUCT(IJobHost)

DECLARE_REFCOUNTED_CLASS(TJobProxy)

DECLARE_REFCOUNTED_CLASS(TJobSatelliteConnectionConfig)

DECLARE_REFCOUNTED_STRUCT(IResourceController)

DECLARE_REFCOUNTED_STRUCT(IUserJobSynchronizer)
DECLARE_REFCOUNTED_STRUCT(IUserJobSynchronizerClient)

DEFINE_ENUM(EJobProxyExitCode,
    ((HeartbeatFailed)        (20))
    ((ResultReportFailed)     (21))
    ((ResourcesUpdateFailed)  (22))
    ((SetRLimitFailed)        (23))
    ((ExecFailed)             (24))
    ((UncaughtException)      (25))
    ((GetJobSpecFailed)       (26))
    ((JobProxyPrepareFailed)  (27))
    ((InvalidSpecVersion)     (28))
    ((ResourceOverdraft)      (29))
    ((PortoManagmentFailed)   (30))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
