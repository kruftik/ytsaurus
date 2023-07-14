#include "operation_controller_host.h"

#include "bootstrap.h"
#include "controller_agent.h"
#include "job_tracker.h"
#include "master_connector.h"
#include "operation.h"
#include "private.h"

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

namespace NYT::NControllerAgent {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NScheduler;
using namespace NYTree;
using namespace NCoreDump;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ControllerAgentLogger;

////////////////////////////////////////////////////////////////////////////////

TAgentToSchedulerOperationEvent::TAgentToSchedulerOperationEvent(
    EAgentToSchedulerOperationEventType eventType,
    TOperationId operationId,
    NScheduler::TControllerEpoch controllerEpoch,
    TError error)
    : EventType(eventType)
    , OperationId(operationId)
    , ControllerEpoch(controllerEpoch)
    , Error(error)
{ }

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateCompletedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch)
{
    return TAgentToSchedulerOperationEvent(
        EAgentToSchedulerOperationEventType::Completed,
        operationId,
        controllerEpoch
    );
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateSuspendedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error)
{
    return TAgentToSchedulerOperationEvent(
        EAgentToSchedulerOperationEventType::Suspended,
        operationId,
        controllerEpoch,
        std::move(error)
    );
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateFailedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error)
{
    return TAgentToSchedulerOperationEvent(
        EAgentToSchedulerOperationEventType::Failed,
        operationId,
        controllerEpoch,
        std::move(error)
    );
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateAbortedEvent(
    TOperationId operationId,
    int controllerEpoch,
    TError error)
{
    return TAgentToSchedulerOperationEvent(
        EAgentToSchedulerOperationEventType::Aborted,
        operationId,
        controllerEpoch,
        std::move(error)
    );
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateBannedInTentativeTreeEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TString treeId,
    std::vector<TJobId> jobIds)
{
    TAgentToSchedulerOperationEvent event(
        EAgentToSchedulerOperationEventType::BannedInTentativeTree,
        operationId,
        controllerEpoch
    );
    event.TentativeTreeId = std::move(treeId);
    event.TentativeTreeJobIds = std::move(jobIds);
    return event;
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateHeavyControllerActionFinishedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error,
    std::optional<TOperationControllerInitializeResult> maybeResult)
{
    TAgentToSchedulerOperationEvent event(
        EAgentToSchedulerOperationEventType::InitializationFinished,
        operationId,
        controllerEpoch,
        std::move(error)
    );
    event.InitializeResult = maybeResult;
    return event;
}


TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateHeavyControllerActionFinishedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error,
    std::optional<TOperationControllerPrepareResult> maybeResult)
{
    TAgentToSchedulerOperationEvent event(
        EAgentToSchedulerOperationEventType::PreparationFinished,
        operationId,
        controllerEpoch,
        std::move(error)
    );
    event.PrepareResult = maybeResult;
    return event;
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateHeavyControllerActionFinishedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error,
    std::optional<TOperationControllerMaterializeResult> maybeResult)
{
    TAgentToSchedulerOperationEvent event(
        EAgentToSchedulerOperationEventType::MaterializationFinished,
        operationId,
        controllerEpoch,
        std::move(error)
    );
    event.MaterializeResult = maybeResult;
    return event;
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateHeavyControllerActionFinishedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error,
    std::optional<TOperationControllerReviveResult> maybeResult)
{
    TAgentToSchedulerOperationEvent event(
        EAgentToSchedulerOperationEventType::RevivalFinished,
        operationId,
        controllerEpoch,
        std::move(error)
    );
    event.ReviveResult = maybeResult;
    return event;
}

TAgentToSchedulerOperationEvent TAgentToSchedulerOperationEvent::CreateHeavyControllerActionFinishedEvent(
    TOperationId operationId,
    TControllerEpoch controllerEpoch,
    TError error,
    std::optional<TOperationControllerCommitResult> maybeResult)
{
    TAgentToSchedulerOperationEvent event(
        EAgentToSchedulerOperationEventType::CommitFinished,
        operationId,
        controllerEpoch,
        std::move(error)
    );
    event.CommitResult = maybeResult;
    return event;
}

////////////////////////////////////////////////////////////////////////////////

TOperationControllerHost::TOperationControllerHost(
    TOperation* operation,
    IInvokerPtr cancelableControlInvoker,
    IInvokerPtr uncancelableControlInvoker,
    TAgentToSchedulerOperationEventOutboxPtr operationEventsOutbox,
    TAgentToSchedulerRunningJobStatisticsOutboxPtr runningJobStatisticsUpdatesOutbox,
    TBootstrap* bootstrap)
    : OperationId_(operation->GetId())
    , CancelableControlInvoker_(std::move(cancelableControlInvoker))
    , UncancelableControlInvoker_(std::move(uncancelableControlInvoker))
    , OperationEventsOutbox_(std::move(operationEventsOutbox))
    , RunningJobStatisticsUpdatesOutbox_(std::move(runningJobStatisticsUpdatesOutbox))
    , Bootstrap_(bootstrap)
    , IncarnationId_(Bootstrap_->GetControllerAgent()->GetIncarnationId())
    , ControllerEpoch_(operation->GetControllerEpoch())
{ }

void TOperationControllerHost::SetJobTrackerOperationHandler(TJobTrackerOperationHandlerPtr jobTrackerOperationHandler)
{
    YT_VERIFY(!std::exchange(JobTrackerOperationHandler_, jobTrackerOperationHandler));
}

void TOperationControllerHost::Disconnect(const TError& error)
{
    Bootstrap_->GetControlInvoker()->Invoke(BIND(&TControllerAgent::Disconnect, Bootstrap_->GetControllerAgent(), error));
}

void TOperationControllerHost::InterruptJob(TJobId jobId, EInterruptReason reason, TDuration timeout)
{
    JobTrackerOperationHandler_->RequestJobInterruption(jobId, reason, timeout);

    YT_LOG_DEBUG("Job interrupt request enqueued (OperationId: %v, JobId: %v)",
        OperationId_,
        jobId);
}

void TOperationControllerHost::FailJob(TJobId jobId)
{
    JobTrackerOperationHandler_->RequestJobFailure(jobId);

    YT_LOG_DEBUG("Job failure request enqueued (OperationId: %v, JobId: %v)",
        OperationId_,
        jobId);
}

void TOperationControllerHost::UpdateRunningJobsStatistics(
    std::vector<TAgentToSchedulerRunningJobStatistics> runningJobStatisticsUpdates)
{
    if (std::empty(runningJobStatisticsUpdates)) {
        return;
    }

    RunningJobStatisticsUpdatesOutbox_->Enqueue(std::move(runningJobStatisticsUpdates));

    YT_LOG_DEBUG("Jobs statistics update request enqueued (OperationId: %v, JobCount: %v)",
        OperationId_,
        runningJobStatisticsUpdates.size());
}

void TOperationControllerHost::RegisterJob(TStartedJobInfo jobInfo)
{
    JobTrackerOperationHandler_->RegisterJob(std::move(jobInfo));
}

void TOperationControllerHost::ReviveJobs(std::vector<TStartedJobInfo> jobs)
{
    JobTrackerOperationHandler_->ReviveJobs(std::move(jobs));
}

void TOperationControllerHost::ReleaseJobs(std::vector<TJobToRelease> jobsToRelease)
{
    if (std::empty(jobsToRelease)) {
        return;
    }

    JobTrackerOperationHandler_->ReleaseJobs(std::move(jobsToRelease));

    YT_LOG_DEBUG("Jobs release request enqueued (OperationId: %v, JobCount: %v)",
        OperationId_,
        jobsToRelease.size());
}

void TOperationControllerHost::AbortJob(
    TJobId jobId,
    NScheduler::EAbortReason abortReason)
{
    JobTrackerOperationHandler_->RequestJobAbortion(jobId, abortReason);
}

std::optional<TString> TOperationControllerHost::RegisterJobForMonitoring(TOperationId operationId, TJobId jobId)
{
    return Bootstrap_->GetControllerAgent()->RegisterJobForMonitoring(operationId, jobId);
}

bool TOperationControllerHost::UnregisterJobForMonitoring(TOperationId operationId, TJobId jobId)
{
    return Bootstrap_->GetControllerAgent()->UnregisterJobForMonitoring(operationId, jobId);
}

TFuture<TOperationSnapshot> TOperationControllerHost::DownloadSnapshot()
{
    return BIND(&NControllerAgent::TMasterConnector::DownloadSnapshot, Bootstrap_->GetControllerAgent()->GetMasterConnector())
        .AsyncVia(CancelableControlInvoker_)
        .Run(OperationId_);
}

TFuture<void> TOperationControllerHost::RemoveSnapshot()
{
    return BIND(&NControllerAgent::TMasterConnector::RemoveSnapshot, Bootstrap_->GetControllerAgent()->GetMasterConnector())
        .AsyncVia(CancelableControlInvoker_)
        .Run(OperationId_);
}

TFuture<void> TOperationControllerHost::FlushOperationNode()
{
    return BIND(&NControllerAgent::TMasterConnector::FlushOperationNode, Bootstrap_->GetControllerAgent()->GetMasterConnector())
        .AsyncVia(CancelableControlInvoker_)
        .Run(OperationId_);
}

TFuture<void> TOperationControllerHost::UpdateInitializedOperationNode(bool isClean)
{
    return BIND(&NControllerAgent::TMasterConnector::UpdateInitializedOperationNode, Bootstrap_->GetControllerAgent()->GetMasterConnector())
        .AsyncVia(CancelableControlInvoker_)
        .Run(OperationId_, isClean);
}

TFuture<void> TOperationControllerHost::AttachChunkTreesToLivePreview(
    NTransactionClient::TTransactionId transactionId,
    NCypressClient::TNodeId tableId,
    const std::vector<TChunkTreeId>& childIds)
{
    return BIND(&NControllerAgent::TMasterConnector::AttachToLivePreview, Bootstrap_->GetControllerAgent()->GetMasterConnector())
        .AsyncVia(CancelableControlInvoker_)
        .Run(
            OperationId_,
            transactionId,
            tableId,
            childIds);
}

void TOperationControllerHost::AddChunkTreesToUnstageList(const std::vector<TChunkId>& chunkTreeIds, bool recursive)
{
    CancelableControlInvoker_->Invoke(BIND(
        &NControllerAgent::TMasterConnector::AddChunkTreesToUnstageList,
        Bootstrap_->GetControllerAgent()->GetMasterConnector(),
        chunkTreeIds,
        recursive));
}

const NApi::NNative::IClientPtr& TOperationControllerHost::GetClient()
{
    return Bootstrap_->GetClient();
}

const NNodeTrackerClient::TNodeDirectoryPtr& TOperationControllerHost::GetNodeDirectory()
{
    return Bootstrap_->GetNodeDirectory();
}

const TThrottlerManagerPtr& TOperationControllerHost::GetChunkLocationThrottlerManager()
{
    return Bootstrap_->GetControllerAgent()->GetChunkLocationThrottlerManager();
}

const IInvokerPtr& TOperationControllerHost::GetControllerThreadPoolInvoker()
{
    return Bootstrap_->GetControllerAgent()->GetControllerThreadPoolInvoker();
}

const IInvokerPtr& TOperationControllerHost::GetJobSpecBuildPoolInvoker()
{
    return Bootstrap_->GetControllerAgent()->GetJobSpecBuildPoolInvoker();
}

const IInvokerPtr& TOperationControllerHost::GetStatisticsOffloadInvoker()
{
    return Bootstrap_->GetControllerAgent()->GetStatisticsOffloadInvoker();
}

const IInvokerPtr& TOperationControllerHost::GetExecNodesUpdateInvoker()
{
    return Bootstrap_->GetControllerAgent()->GetExecNodesUpdateInvoker();
}

const IInvokerPtr& TOperationControllerHost::GetConnectionInvoker()
{
    return Bootstrap_->GetConnectionInvoker();
}

const NEventLog::IEventLogWriterPtr& TOperationControllerHost::GetEventLogWriter()
{
    return Bootstrap_->GetControllerAgent()->GetEventLogWriter();
}

const ICoreDumperPtr& TOperationControllerHost::GetCoreDumper()
{
    return Bootstrap_->GetControllerAgent()->GetCoreDumper();
}

const TAsyncSemaphorePtr& TOperationControllerHost::GetCoreSemaphore()
{
    return Bootstrap_->GetControllerAgent()->GetCoreSemaphore();
}

TMemoryTagQueue* TOperationControllerHost::GetMemoryTagQueue()
{
    return Bootstrap_->GetControllerAgent()->GetMemoryTagQueue();
}

TJobProfiler* TOperationControllerHost::GetJobProfiler() const
{
    return Bootstrap_->GetControllerAgent()->GetJobProfiler();
}

TJobTracker* TOperationControllerHost::GetJobTracker() const
{
    return Bootstrap_->GetControllerAgent()->GetJobTracker();
}

int TOperationControllerHost::GetOnlineExecNodeCount()
{
    return Bootstrap_->GetControllerAgent()->GetOnlineExecNodeCount();
}

TRefCountedExecNodeDescriptorMapPtr TOperationControllerHost::GetExecNodeDescriptors(const TSchedulingTagFilter& filter, bool onlineOnly)
{
    return Bootstrap_->GetControllerAgent()->GetExecNodeDescriptors(filter, onlineOnly);
}

TJobResources TOperationControllerHost::GetMaxAvailableResources(const TSchedulingTagFilter& filter)
{
    return Bootstrap_->GetControllerAgent()->GetMaxAvailableResources(filter);
}

TInstant TOperationControllerHost::GetConnectionTime()
{
    return Bootstrap_->GetControllerAgent()->GetConnectionTime();
}

TIncarnationId TOperationControllerHost::GetIncarnationId()
{
    return IncarnationId_;
}

const NConcurrency::IThroughputThrottlerPtr& TOperationControllerHost::GetJobSpecSliceThrottler()
{
    return Bootstrap_->GetControllerAgent()->GetJobSpecSliceThrottler();
}

const TJobReporterPtr& TOperationControllerHost::GetJobReporter()
{
    return Bootstrap_->GetControllerAgent()->GetJobReporter();
}

const NChunkClient::TMediumDirectoryPtr& TOperationControllerHost::GetMediumDirectory()
{
    return Bootstrap_
        ->GetClient()
        ->GetNativeConnection()
        ->GetMediumDirectory();
}

void TOperationControllerHost::OnOperationCompleted()
{
    OperationEventsOutbox_->Enqueue(TAgentToSchedulerOperationEvent::CreateCompletedEvent(OperationId_, ControllerEpoch_));
    YT_LOG_DEBUG("Operation completion notification enqueued (OperationId: %v)",
        OperationId_);
}

void TOperationControllerHost::OnOperationAborted(const TError& error)
{
    OperationEventsOutbox_->Enqueue(TAgentToSchedulerOperationEvent::CreateAbortedEvent(OperationId_, ControllerEpoch_, error));
    YT_LOG_DEBUG(error, "Operation abort notification enqueued (OperationId: %v)",
        OperationId_,
        error);
}

void TOperationControllerHost::OnOperationFailed(const TError& error)
{
    OperationEventsOutbox_->Enqueue(TAgentToSchedulerOperationEvent::CreateFailedEvent(OperationId_, ControllerEpoch_, error));
    YT_LOG_DEBUG(error, "Operation failure notification enqueued (OperationId: %v)",
        OperationId_);
}

void TOperationControllerHost::OnOperationSuspended(const TError& error)
{
    OperationEventsOutbox_->Enqueue(TAgentToSchedulerOperationEvent::CreateSuspendedEvent(OperationId_, ControllerEpoch_, error));
    YT_LOG_DEBUG(error, "Operation suspension notification enqueued (OperationId: %v)",
        OperationId_);
}

void TOperationControllerHost::OnOperationBannedInTentativeTree(const TString& treeId, const std::vector<TJobId>& jobIds)
{
    OperationEventsOutbox_->Enqueue(TAgentToSchedulerOperationEvent::CreateBannedInTentativeTreeEvent(
        OperationId_,
        ControllerEpoch_,
        treeId,
        jobIds));
    YT_LOG_DEBUG("Operation tentative tree ban notification enqueued (OperationId: %v, TreeId: %v)",
        OperationId_,
        treeId);
}

void TOperationControllerHost::ValidateOperationAccess(
    const TString& user,
    EPermission permission)
{
    WaitFor(BIND(&TControllerAgent::ValidateOperationAccess, Bootstrap_->GetControllerAgent())
        .AsyncVia(UncancelableControlInvoker_)
        .Run(
            user,
            OperationId_,
            permission))
        .ThrowOnError();
}

TFuture<void> TOperationControllerHost::UpdateAccountResourceUsageLease(
    NSecurityClient::TAccountResourceUsageLeaseId leaseId,
    const TDiskQuota& diskQuota)
{
    return Bootstrap_->GetControllerAgent()->GetMasterConnector()->UpdateAccountResourceUsageLease(
        leaseId,
        diskQuota);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
