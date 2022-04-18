#include "job_controller.h"

#include "job.h"

#include <yt/yt/server/master/node_tracker_server/node.h>

namespace NYT::NChunkServer {

static const auto& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TCompositeJobController
    : public ICompositeJobController
{
public:
    // IJobController implementation.
    TCompositeJobController() = default;

    void ScheduleJobs(IJobSchedulingContext* context) override
    {
        auto jobRegistry = context->GetJobRegistry();
        if (jobRegistry->IsOverdraft()) {
            YT_LOG_ERROR("Job throttler is overdrafted, skipping job scheduling (Address: %v)",
                context->GetNode()->GetDefaultAddress());
            return;
        }
        for (auto jobType : TEnumTraits<EJobType>::GetDomainValues()) {
            if (IsMasterJobType(jobType)) {
                if (jobRegistry->IsOverdraft(jobType)) {
                    YT_LOG_ERROR("Job throttler is overdrafted for job type, skipping job scheduling (Address: %v, JobType: %v)",
                        context->GetNode()->GetDefaultAddress(),
                        jobType);
                } else {
                    const auto& jobController = GetControllerForJobType(jobType);
                    jobController->ScheduleJobs(context);
                }
            }
        }
    }

    void OnJobWaiting(const TJobPtr& job, IJobControllerCallbacks* callbacks) override
    {
        const auto& jobController = GetControllerForJob(job);
        jobController->OnJobWaiting(job, callbacks);
    }

    void OnJobRunning(const TJobPtr& job, IJobControllerCallbacks* callbacks) override
    {
        const auto& jobController = GetControllerForJob(job);
        jobController->OnJobRunning(job, callbacks);
    }

    void OnJobCompleted(const TJobPtr& job) override
    {
        const auto& jobController = GetControllerForJob(job);
        jobController->OnJobCompleted(job);
    }

    void OnJobAborted(const TJobPtr& job) override
    {
        const auto& jobController = GetControllerForJob(job);
        jobController->OnJobAborted(job);
    }

    void OnJobFailed(const TJobPtr& job) override
    {
        const auto& jobController = GetControllerForJob(job);
        jobController->OnJobFailed(job);
    }

    // ICompositeJobController implementation.
    void RegisterJobController(EJobType jobType, IJobControllerPtr controller) override
    {
        YT_VERIFY(JobTypeToJobController_.emplace(jobType, controller).second);
        JobControllers_.insert(std::move(controller));
    }

private:
    THashMap<EJobType, IJobControllerPtr> JobTypeToJobController_;
    THashSet<IJobControllerPtr> JobControllers_;

    const IJobControllerPtr& GetControllerForJob(const TJobPtr& job) const
    {
        return GetOrCrash(JobTypeToJobController_, job->GetType());
    }

    const IJobControllerPtr& GetControllerForJobType(EJobType jobType)
    {
        return GetOrCrash(JobTypeToJobController_, jobType);
    }
};

////////////////////////////////////////////////////////////////////////////////

ICompositeJobControllerPtr CreateCompositeJobController()
{
    return New<TCompositeJobController>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
