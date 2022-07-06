#pragma once

#include "persistence.h"
#include "helpers.h"

#include <yt/yt/server/lib/scheduler/public.h>
#include <yt/yt/server/lib/scheduler/structs.h>
#include <yt/yt/server/lib/scheduler/proto/controller_agent_tracker_service.pb.h>

#include <yt/yt/server/lib/job_agent/job_report.h>

#include <yt/yt/ytlib/scheduler/proto/job.pb.h>

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/core/misc/phoenix.h>
#include <yt/yt/core/misc/statistics.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

// NB: This particular summary does not inherit from TJobSummary.
struct TStartedJobSummary
{
    TOperationId OperationId;
    TJobId Id;
    TInstant StartTime;
};

// TODO(max42): does this need to belong to server/lib?
// TODO(max42): make this structure non-copyable.
struct TJobSummary
{
    TJobSummary() = default;
    TJobSummary(TJobId id, EJobState state);
    explicit TJobSummary(NJobTrackerClient::NProto::TJobStatus* status);

    virtual ~TJobSummary() = default;

    void Persist(const TPersistenceContext& context);

    //! Crashes if job result is not combined yet.
    NJobTrackerClient::NProto::TJobResult& GetJobResult();
    const NJobTrackerClient::NProto::TJobResult& GetJobResult() const;

    //! Crashes if job result is not combined yet or it misses a scheduler job result extension.
    NScheduler::NProto::TSchedulerJobResultExt& GetSchedulerJobResult();
    const NScheduler::NProto::TSchedulerJobResultExt& GetSchedulerJobResult() const;

    //! Crashes if job result is not combined yet, and returns nullptr if scheduler job
    //! result extension is missing.
    const NScheduler::NProto::TSchedulerJobResultExt* FindSchedulerJobResult() const;

    // NB: may be nullopt or may miss scheduler job result extension while job
    // result is being combined from scheduler and node parts.
    // Prefer using GetJobResult() and GetSchedulerJobResult() helpers.
    std::optional<NJobTrackerClient::NProto::TJobResult> Result;

    TJobId Id;
    EJobState State = EJobState::None;
    EJobPhase Phase = EJobPhase::Missing;

    std::optional<TInstant> FinishTime;
    NJobAgent::TTimeStatistics TimeStatistics;

    // NB: The Statistics field will be set inside the controller in ParseStatistics().
    std::optional<TStatistics> Statistics;
    NYson::TYsonString StatisticsYson;

    NJobTrackerClient::TReleaseJobFlags ReleaseFlags;

    TInstant LastStatusUpdateTime;
    bool JobExecutionCompleted = false;
};

struct TCompletedJobSummary
    : public TJobSummary
{
    TCompletedJobSummary() = default;
    explicit TCompletedJobSummary(NJobTrackerClient::NProto::TJobStatus* status);

    void Persist(const TPersistenceContext& context);

    bool Abandoned = false;
    EInterruptReason InterruptReason = EInterruptReason::None;

    // These fields are for controller's use only.
    std::vector<NChunkClient::TLegacyDataSlicePtr> UnreadInputDataSlices;
    std::vector<NChunkClient::TLegacyDataSlicePtr> ReadInputDataSlices;
    int SplitJobCount = 1;

    inline static constexpr EJobState ExpectedState = EJobState::Completed;
};

std::unique_ptr<TCompletedJobSummary> CreateAbandonedJobSummary(TJobId jobId);

struct TAbortedJobSummary
    : public TJobSummary
{
    TAbortedJobSummary(TJobId id, EAbortReason abortReason);
    TAbortedJobSummary(const TJobSummary& other, EAbortReason abortReason);
    explicit TAbortedJobSummary(NJobTrackerClient::NProto::TJobStatus* status);

    EAbortReason AbortReason = EAbortReason::None;
    std::optional<NScheduler::TPreemptedFor> PreemptedFor;
    bool AbortedByScheduler = false;

    bool Scheduled = true;

    inline static constexpr EJobState ExpectedState = EJobState::Aborted;
};

std::unique_ptr<TAbortedJobSummary> CreateAbortedJobSummary(TAbortedBySchedulerJobSummary&& eventSummary, const NLogging::TLogger& Logger);

struct TFailedJobSummary
    : public TJobSummary
{
    explicit TFailedJobSummary(NScheduler::NProto::TSchedulerToAgentJobEvent* event);
    explicit TFailedJobSummary(NJobTrackerClient::NProto::TJobStatus* status);

    inline static constexpr EJobState ExpectedState = EJobState::Failed;
};

struct TRunningJobSummary
    : public TJobSummary
{
    explicit TRunningJobSummary(NScheduler::NProto::TSchedulerToAgentJobEvent* event);
    explicit TRunningJobSummary(NJobTrackerClient::NProto::TJobStatus* status);

    double Progress = 0;
    i64 StderrSize = 0;

    inline static constexpr EJobState ExpectedState = EJobState::Running;
};

struct TFinishedJobSummary
{
    TOperationId OperationId;
    TJobId Id;
    TInstant FinishTime;
    bool JobExecutionCompleted;
    std::optional<EInterruptReason> InterruptReason;
    std::optional<NScheduler::TPreemptedFor> PreemptedFor;
    bool Preempted;
    std::optional<TString> PreemptionReason;
};

struct TAbortedBySchedulerJobSummary
{
    TOperationId OperationId;
    TJobId Id;
    TInstant FinishTime;
    std::optional<EAbortReason> AbortReason;
    TError Error;
    bool Scheduled;
};

struct TSchedulerToAgentJobEvent
{
    std::variant<TStartedJobSummary, TFinishedJobSummary, TAbortedBySchedulerJobSummary> EventSummary;
};

void ToProto(NScheduler::NProto::TSchedulerToAgentStartedJobEvent* proto, const TStartedJobSummary& summary);
void FromProto(TStartedJobSummary* summary, NScheduler::NProto::TSchedulerToAgentStartedJobEvent* protoEvent);
void ToProto(NScheduler::NProto::TSchedulerToAgentFinishedJobEvent* protoEvent, const TFinishedJobSummary& summary);
void FromProto(TFinishedJobSummary* summary, NScheduler::NProto::TSchedulerToAgentFinishedJobEvent* protoEvent);
void ToProto(NScheduler::NProto::TSchedulerToAgentAbortedJobEvent* proto, const TAbortedBySchedulerJobSummary& summary);
void FromProto(TAbortedBySchedulerJobSummary* summary, NScheduler::NProto::TSchedulerToAgentAbortedJobEvent* protoEvent);

void ToProto(NScheduler::NProto::TSchedulerToAgentJobEvent* proto, const TSchedulerToAgentJobEvent& event);
void FromProto(TSchedulerToAgentJobEvent* event, NScheduler::NProto::TSchedulerToAgentJobEvent* proto);

std::unique_ptr<TJobSummary> ParseJobSummary(
    NJobTrackerClient::NProto::TJobStatus* const status,
    const NLogging::TLogger& Logger);

std::unique_ptr<TFailedJobSummary> MergeJobSummaries(
    std::unique_ptr<TFailedJobSummary> nodeJobSummary,
    TFinishedJobSummary&& schedulerJobSummary,
    const NLogging::TLogger& Logger);

std::unique_ptr<TAbortedJobSummary> MergeJobSummaries(
    std::unique_ptr<TAbortedJobSummary> nodeJobSummary,
    TFinishedJobSummary&& schedulerJobSummary,
    const NLogging::TLogger& Logger);

std::unique_ptr<TCompletedJobSummary> MergeJobSummaries(
    std::unique_ptr<TCompletedJobSummary> nodeJobSummary,
    TFinishedJobSummary&& schedulerJobSummary,
    const NLogging::TLogger& Logger);

std::unique_ptr<TJobSummary> MergeJobSummaries(
    std::unique_ptr<TJobSummary> nodeJobSummary,
    TFinishedJobSummary&& schedulerJobSummary,
    const NLogging::TLogger& Logger);

template <class TJobSummaryType>
std::unique_ptr<TJobSummaryType> SummaryCast(std::unique_ptr<TJobSummary> jobSummary) noexcept
{
    YT_VERIFY(jobSummary->State == TJobSummaryType::ExpectedState);
    return std::unique_ptr<TJobSummaryType>{static_cast<TJobSummaryType*>(jobSummary.release())};
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
