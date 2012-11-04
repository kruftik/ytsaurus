#include "stdafx.h"
#include "map_controller.h"
#include "private.h"
#include "operation_controller_detail.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"
#include "job_resources.h"

#include <ytlib/ytree/fluent.h>

#include <ytlib/table_client/schema.h>

#include <server/job_proxy/config.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/table_client/key.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYPath;
using namespace NChunkServer;
using namespace NJobProxy;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger(OperationLogger);
static NProfiling::TProfiler Profiler("/operations/map");

////////////////////////////////////////////////////////////////////

class TMapController
    : public TOperationControllerBase
{
public:
    TMapController(
        TSchedulerConfigPtr config,
        TMapOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, host, operation)
        , Spec(spec)
        , TotalJobCount(0)
    { }

    virtual TNodeResources GetMinNeededResources() override
    {
        return MapTask
            ? MapTask->GetMinNeededResources()
            : InfiniteNodeResources();
    }


private:
    TMapOperationSpecPtr Spec;

    //! Overrides the spec limit to prevent huge job counts.
    i64 MaxDataSizePerJob;

    // Counters.
    int TotalJobCount;

    class TMapTask
        : public TTask
    {
    public:
        explicit TMapTask(TMapController* controller)
            : TTask(controller)
            , Controller(controller)
        {
            ChunkPool = CreateUnorderedChunkPool();
        }

        virtual Stroka GetId() const override
        {
            return "Map";
        }

        virtual int GetPendingJobCount() const override
        {
            return
                IsPending()
                ? Controller->TotalJobCount - Controller->RunningJobCount - Controller->CompletedJobCount
                : 0;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return Controller->Spec->LocalityTimeout;
        }

        virtual TNodeResources GetMinNeededResources() const override
        {
            TNodeResources result;
            result.set_slots(1);
            result.set_cpu(Controller->Spec->Mapper->CpuLimit);
            result.set_memory(
                GetIOMemorySize(
                Controller->Spec->JobIO,
                1,
                Controller->Spec->OutputTablePaths.size()) +
                GetFootprintMemorySize());
            return result;
        }

    private:
        TMapController* Controller;

        virtual int GetChunkListCountPerJob() const override
        {
            return Controller->OutputTables.size();
        }

        virtual TNullable<i64> GetJobDataSizeThreshold() const override
        {
            return GetJobDataSizeThresholdGeneric(
                GetPendingJobCount(),
                DataSizeCounter().GetPending());
        }

        virtual void BuildJobSpec(
            TJobInProgressPtr jip,
            NProto::TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller->JobSpecTemplate);
            AddSequentialInputSpec(jobSpec, jip);
            AddOutputSpecs(jobSpec, jip);

            auto* jobSpecExt = jobSpec->MutableExtension(TMapJobSpecExt::map_job_spec_ext);
            Controller->AddUserJobEnvironment(jobSpecExt->mutable_mapper_spec(), jip);
        }

        virtual void OnJobCompleted(TJobInProgressPtr jip) override
        {
            TTask::OnJobCompleted(jip);

            Controller->RegisterOutputChunkTrees(jip, 0);
        }

        virtual void OnTaskCompleted() override
        {
            TTask::OnTaskCompleted();

            // Finalize the counter.
            Controller->TotalJobCount = Controller->CompletedJobCount;
        }
    };
    
    typedef TIntrusivePtr<TMapTask> TMapTaskPtr;

    TMapTaskPtr MapTask;
    TJobSpec JobSpecTemplate;


    // Custom bits of preparation pipeline.

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec->OutputTablePaths;
    }

    virtual std::vector<TRichYPath> GetFilePaths() const override
    {
        return Spec->Mapper->FilePaths;
    }

    virtual TAsyncPipeline<void>::TPtr CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline) override
    {
        return pipeline->Add(BIND(&TMapController::ProcessInputs, MakeStrong(this)));
    }

    TFuture<void> ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");
            
            // Compute statistics and populate the pool.
            auto inputChunks = CollectInputTablesChunks();
            MapTask = New<TMapTask>(this);
            auto stripes = PrepareChunkStripes(
                inputChunks,
                Spec->JobCount,
                Spec->JobSliceDataSize);
            MapTask->AddStripes(stripes);

            // Check for empty inputs.
            if (MapTask->IsCompleted()) {
                LOG_INFO("Empty input");
                OnOperationCompleted();
                return NewPromise<void>();
            }

            TotalJobCount = SuggestJobCount(
                MapTask->DataSizeCounter().GetTotal(),
                Spec->MinDataSizePerJob,
                Spec->MaxDataSizePerJob,
                Spec->JobCount,
                MapTask->ChunkCounter().GetTotal());
            
            InitJobSpecTemplate();

            LOG_INFO("Inputs processed (DataSize: %" PRId64 ", ChunkCount: %" PRId64 ", JobCount: %d)",
                MapTask->DataSizeCounter().GetTotal(),
                MapTask->ChunkCounter().GetTotal(),
                TotalJobCount);

            // Kick-start the map task.
            AddTaskPendingHint(MapTask);
        }

        return MakeFuture();
    }

    // Progress reporting.

    virtual void LogProgress() override
    {
        LOG_DEBUG("Progress: "
            "Jobs = {T: %d, R: %d, C: %d, P: %d, F: %d}",
            TotalJobCount,
            RunningJobCount,
            CompletedJobCount,
            GetPendingJobCount(),
            FailedJobCount);
    }


    // Unsorted helpers.

    void InitJobSpecTemplate()
    {
        JobSpecTemplate.set_type(EJobType::Map);

        auto* jobSpecExt = JobSpecTemplate.MutableExtension(TMapJobSpecExt::map_job_spec_ext);
        
        InitUserJobSpec(
            jobSpecExt->mutable_mapper_spec(),
            Spec->Mapper,
            Files);

        *JobSpecTemplate.mutable_output_transaction_id() = OutputTransaction->GetId().ToProto();

        JobSpecTemplate.set_io_config(ConvertToYsonString(Spec->JobIO).Data());
    }

};

IOperationControllerPtr CreateMapController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TMapOperationSpec>(operation, config->MapOperationSpec);
    return New<TMapController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

