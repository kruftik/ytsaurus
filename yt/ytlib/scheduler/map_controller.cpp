#include "stdafx.h"
#include "map_controller.h"
#include "private.h"
#include "operation_controller_detail.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"

#include <ytlib/ytree/fluent.h>
#include <ytlib/table_client/schema.h>
#include <ytlib/job_proxy/config.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>
#include <ytlib/transaction_client/transaction.h>
#include <ytlib/table_client/key.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NChunkServer;
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
        , Config(config)
        , Spec(spec)
        , TotalJobCount(0)
        , MapTask(New<TMapTask>(this))
    { }

private:
    TSchedulerConfigPtr Config;
    TMapOperationSpecPtr Spec;

    // Counters.
    int TotalJobCount;

    // Map task.

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

        virtual Stroka GetId() const
        {
            return "Map";
        }

        virtual int GetPendingJobCount() const
        {
            return
                IsPending()
                ? Controller->TotalJobCount - Controller->RunningJobCount - Controller->CompletedJobCount
                : 0;
        }

        virtual TDuration GetLocalityTimeout() const
        {
            return Controller->Spec->LocalityTimeout;
        }

    private:
        friend class TMapController;

        TMapController* Controller;

        virtual int GetChunkListCountPerJob() const 
        {
            return static_cast<int>(Controller->OutputTables.size());
        }

        virtual TNullable<i64> GetJobWeightThreshold() const
        {
            return GetJobWeightThresholdGeneric(
                GetPendingJobCount(),
                WeightCounter().GetPending());
        }

        virtual TJobSpec GetJobSpec(TJobInProgressPtr jip)
        {
            auto jobSpec = Controller->JobSpecTemplate;
            AddSequentialInputSpec(&jobSpec, jip);
            for (int index = 0; index < static_cast<int>(Controller->OutputTables.size()); ++index) {
                AddTabularOutputSpec(&jobSpec, jip, index);
            }
            return jobSpec;
        }

        virtual void OnJobCompleted(TJobInProgressPtr jip)
        {
            TTask::OnJobCompleted(jip);

            for (int index = 0; index < static_cast<int>(Controller->OutputTables.size()); ++index) {
                Controller->RegisterOutputChunkTree(jip->ChunkListIds[index], 0, index);
            }
        }
    };
    
    typedef TIntrusivePtr<TMapTask> TMapTaskPtr;

    TMapTaskPtr MapTask;
    TJobSpec JobSpecTemplate;


    // Custom bits of preparation pipeline.

    virtual std::vector<TYPath> GetInputTablePaths()
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TYPath> GetOutputTablePaths()
    {
        return Spec->OutputTablePaths;
    }

    virtual std::vector<TYPath> GetFilePaths()
    {
        return Spec->Mapper->FilePaths;
    }

    virtual TAsyncPipeline<void>::TPtr CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline)
    {
        return pipeline->Add(BIND(&TMapController::ProcessInputs, MakeStrong(this)));
    }

    TFuture<void> ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");
            
            // Compute statistics and populate the pool.
            auto inputChunks = CollectInputTablesChunks();
            auto stripes = PrepareChunkStripes(
                inputChunks,
                Spec->JobCount,
                Spec->MaxWeightPerJob);
            MapTask->AddStripes(stripes);

            // Check for empty inputs.
            if (MapTask->IsCompleted()) {
                LOG_INFO("Empty input");
                OnOperationCompleted();
                return NewPromise<void>();
            }

            TotalJobCount = GetJobCount(
                MapTask->WeightCounter().GetTotal(),
                Spec->MaxWeightPerJob,
                Spec->JobCount,
                MapTask->ChunkCounter().GetTotal());
            
            // Allocate some initial chunk lists.
            ChunkListPool->Allocate(OutputTables.size() * TotalJobCount + Config->SpareChunkListCount);

            InitJobSpecTemplate();

            LOG_INFO("Inputs processed (Weight: %" PRId64 ", ChunkCount: %" PRId64 ", JobCount: %d)",
                MapTask->WeightCounter().GetTotal(),
                MapTask->ChunkCounter().GetTotal(),
                TotalJobCount);

            // Kick-start the map task.
            AddTaskPendingHint(MapTask);
        }

        return MakeFuture();
    }


    // Progress reporting.

    virtual void LogProgress()
    {
        LOG_DEBUG("Progress: "
            "Jobs = {T: %d, R: %d, C: %d, P: %d, F: %d}, "
            "Chunks = {%s}, "
            "Weight = {%s}",
            TotalJobCount,
            RunningJobCount,
            CompletedJobCount,
            GetPendingJobCount(),
            FailedJobCount,
            ~ToString(MapTask->ChunkCounter()),
            ~ToString(MapTask->WeightCounter()));
    }

    virtual void DoGetProgress(IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item("chunks").Do(BIND(&TProgressCounter::ToYson, &MapTask->ChunkCounter()))
            .Item("weight").Do(BIND(&TProgressCounter::ToYson, &MapTask->WeightCounter()));
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

        JobSpecTemplate.set_io_config(ConvertToYsonString(Config->MapJobIO).Data());
    }

};

IOperationControllerPtr CreateMapController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TMapOperationSpec>(operation);
    return New<TMapController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

