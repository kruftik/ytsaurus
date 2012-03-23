#include "stdafx.h"
#include "map_controller.h"
#include "operation_controller.h"
#include "operation.h"
#include "job.h"
#include "exec_node.h"
#include "private.h"

#include <ytlib/logging/tagged_logger.h>

#include <ytlib/ytree/serialize.h>

#include <ytlib/misc/thread_affinity.h>

#include <ytlib/transaction_server/transaction_ypath_proxy.h>

#include <ytlib/table_server/table_ypath_proxy.h>
#include <ytlib/table_client/table_reader.pb.h>

#include <ytlib/object_server/object_ypath_proxy.h>

#include <ytlib/file_server/file_ypath_proxy.h>

#include <ytlib/cypress/cypress_service_proxy.h>

#include <ytlib/chunk_server/public.h>
#include <ytlib/chunk_server/chunk_list_ypath_proxy.h>

#include <ytlib/table_client/schema.h>

namespace NYT {
namespace NScheduler {

using namespace NProto;
using namespace NYTree;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NTableServer;
using namespace NTableServer::NProto;
using namespace NTransactionClient;
using namespace NTransactionServer;
using namespace NCypress;
using namespace NChunkServer;
using namespace NChunkHolder::NProto;
using namespace NFileServer;

////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger(OperationLogger);

////////////////////////////////////////////////////////////////////

class TOperationControllerBase
    : public IOperationController
{
public:
    TOperationControllerBase(IOperationHost* host, TOperation* operation)
        : Host(host)
        , Operation(operation)
        , CypressProxy(host->GetMasterChannel())
        , Logger(OperationLogger)
    {
        VERIFY_INVOKER_AFFINITY(Host->GetControlInvoker(), ControlThread);
        VERIFY_INVOKER_AFFINITY(Host->GetBackgroundInvoker(), BackgroundThread);
    }

    virtual void Initialize()
    { }

    virtual TFuture<TVoid>::TPtr Prepare()
    {
        return MakeFuture(TVoid());
    }

    virtual void OnJobRunning(TJobPtr job)
    {
        UNUSED(job);
    }

    virtual void OnJobCompleted(TJobPtr job)
    {
        UNUSED(job);

    }

    virtual void OnJobFailed(TJobPtr job)
    {
        UNUSED(job);
    }


    virtual void OnOperationAborted()
    { }


    virtual void ScheduleJobs(
        TExecNodePtr node,
        std::vector<TJobPtr>* jobsToStart,
        std::vector<TJobPtr>* jobsToAbort)
    {
        UNUSED(node);
        UNUSED(jobsToStart);
        UNUSED(jobsToAbort);
    }

protected:
    IOperationHost* Host;
    TOperation* Operation;

    TCypressServiceProxy CypressProxy;
    NLog::TTaggedLogger Logger;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(BackgroundThread);


    void OnOperationFailed(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Operation failed\n%s", ~error.ToString());

        Host->OnOperationFailed(Operation, error);
        AbortOperation();
    }

    void OnOperationCompleted()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Operation completed");

        Host->OnOperationCompleted(Operation);
    }


    virtual void AbortOperation()
    { }


    //static i64 GetRowCount(const TInputChunk& fetchedChunk)
    //{
    //    TChunkAttributes chunkAttributes;
    //    DeserializeProtobuf(&chunkAttributes, fetchedChunk.attributes());

    //    YASSERT(chunkAttributes.HasExtension(TTableChunkAttributes::table_attributes));
    //    const auto& tableChunkAttributes = chunkAttributes.GetExtension(TTableChunkAttributes::table_attributes);

    //    return tableChunkAttributes.row_count();
    //}

    //static i64 GetRowCount(TTableYPathProxy::TRspFetch::TPtr rsp)
    //{
    //    i64 result = 0;
    //    FOREACH (const auto& chunk, rsp->chunks()) {
    //        result += GetRowCount(chunk);
    //    }
    //    return result;
    //}
};

////////////////////////////////////////////////////////////////////

class TChunkAllocationMap
{
public:
    int PutChunk(const TInputChunk& chunk, i64 weight)
    {
        TChunkInfo info;
        info.Chunk = chunk;
        info.Weight = weight;
        int index = static_cast<int>(ChunkInfos.size());
        ChunkInfos.push_back(info);
        RegisterChunk(index);
        return index;
    }

    const TInputChunk& GetChunk(int index)
    {
        return ChunkInfos[index].Chunk;
    }

    void AllocateChunks(
        const Stroka& address,
        i64 maxWeight,
        std::vector<int>* indexes,
        i64* allocatedWeight,
        int* localCount,
        int* remoteCount)
    {
        *allocatedWeight = 0;

        // Take local chunks first.
        *localCount = 0;
        auto addressIt = AddressToIndexSet.find(address);
        if (addressIt != AddressToIndexSet.end()) {
            auto& localIndexes = addressIt->second;
            auto localIt = localIndexes.begin();
            while (localIt != localIndexes.end()) {
                if (*allocatedWeight >= maxWeight) {
                    break;
                }

                auto nextLocalIt = localIt;
                ++nextLocalIt;
                int chunkIndex = *localIt;

                indexes->push_back(chunkIndex);
                localIndexes.erase(localIt);
                YVERIFY(UnallocatedIndexes.erase(chunkIndex) == 1);
                ++*localCount;
                allocatedWeight += ChunkInfos[chunkIndex].Weight;
                
                localIt = nextLocalIt;
            }
        }

        // Proceed with remote chunks next.
        *remoteCount = 0;
        auto remoteIt = UnallocatedIndexes.begin();
        while (remoteIt != UnallocatedIndexes.end()) {
            if (*allocatedWeight >= maxWeight) {
                break;
            }

            auto nextRemoteIt = remoteIt;
            ++nextRemoteIt;
            int chunkIndex = *remoteIt;

            indexes->push_back(*remoteIt);
            const auto& info = ChunkInfos[chunkIndex];
            FOREACH (const auto& address, info.Chunk.holder_addresses()) {
                YVERIFY(AddressToIndexSet[address].erase(chunkIndex) == 1);
            }
            ++*remoteCount;
            allocatedWeight += ChunkInfos[chunkIndex].Weight;
            
            remoteIt = nextRemoteIt;
        }
    }

    void DeallocateChunks(const std::vector<int>& indexes)
    {
        FOREACH (auto index, indexes) {
            RegisterChunk(index);
        }
    }

private:
    struct TChunkInfo
    {
        TInputChunk Chunk;
        i64 Weight;
    };

    std::vector<TChunkInfo> ChunkInfos;
    yhash_map<Stroka, yhash_set<int> > AddressToIndexSet;
    yhash_set<int> UnallocatedIndexes;

    void RegisterChunk(int index)
    {
        const auto& info = ChunkInfos[index];
        FOREACH (const auto& address, info.Chunk.holder_addresses()) {
            YVERIFY(AddressToIndexSet[address].insert(index).second);
        }
        YVERIFY(UnallocatedIndexes.insert(index).second);
    }
};

////////////////////////////////////////////////////////////////////

class TChunkListPool
    : public TRefCounted
{
public:
    TChunkListPool(
        NRpc::IChannel::TPtr masterChannel,
        IInvoker::TPtr controlInvoker,
        TOperationPtr operation,
        const TTransactionId& transactionId)
        : MasterChannel(masterChannel)
        , ControlInvoker(controlInvoker)
        , Operation(operation)
        , TransactionId(transactionId)
        , Logger(OperationLogger)
        , RequestInProgress(false)
    {
        VERIFY_INVOKER_AFFINITY(ControlInvoker, ControlThread);
        
        Logger.AddTag(Sprintf("OperationId: %s", ~operation->GetOperationId().ToString()));
    }

    int GetSize() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return static_cast<int>(Ids.size());
    }

    TChunkListId Extract()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YASSERT(!Ids.empty());
        auto id = Ids.back();
        Ids.pop_back();

        LOG_DEBUG("Extracted chunk list %s from the pool, %d remaining",
            ~id.ToString(),
            static_cast<int>(Ids.size()));

        return id;
    }

    void Allocate(int count)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (RequestInProgress) {
            LOG_DEBUG("Cannot allocate more chunk lists, another request is in progress");
            return;
        }

        LOG_INFO("Allocating %d chunk lists for pool", count);

        TCypressServiceProxy cypressProxy(MasterChannel);
        auto batchReq = cypressProxy.ExecuteBatch();

        for (int index = 0; index < count; ++index) {
            auto req = TTransactionYPathProxy::CreateObject(FromObjectId(TransactionId));
            req->set_type(EObjectType::ChunkList);
            batchReq->AddRequest(req);
        }

        batchReq->Invoke()->Subscribe(
            FromMethod(&TChunkListPool::OnChunkListsCreated, MakeWeak(this))
            ->Via(ControlInvoker));

        RequestInProgress = true;
    }

private:
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    NRpc::IChannel::TPtr MasterChannel;
    IInvoker::TPtr ControlInvoker;
    TOperationPtr Operation;
    TTransactionId TransactionId;

    NLog::TTaggedLogger Logger;
    bool RequestInProgress;
    std::vector<TChunkListId> Ids;

    void OnChunkListsCreated(TCypressServiceProxy::TRspExecuteBatch::TPtr batchRsp)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        RequestInProgress = false;

        if (!batchRsp->IsOK()) {
            LOG_ERROR("Error allocating chunk lists\n%s", ~batchRsp->GetError().ToString());
            // TODO(babenko): backoff time?
            return;
        }

        LOG_INFO("Chunk lists allocated");

        YASSERT(RequestInProgress);
        YASSERT(Ids.empty());

        FOREACH (auto rsp, batchRsp->GetResponses<TTransactionYPathProxy::TRspCreateObject>()) {
            Ids.push_back(TChunkListId::FromProto(rsp->object_id()));
        }
    }
};

typedef TIntrusivePtr<TChunkListPool> TChunkListPoolPtr;

////////////////////////////////////////////////////////////////////

class TRunningCounter
{
public:
    TRunningCounter()
        : Total_(-1)
        , Running_(-1)
        , Done_(-1)
        , Pending_(-1)
        , Failed_(-1)
    { }

    void Init(i64 total)
    {
        Total_ = total;
        Running_ = 0;
        Done_ = 0;
        Pending_ = total;
        Failed_ = 0;
    }


    i64 GetTotal() const
    {
        return Total_;
    }

    i64 GetRunning() const
    {
        return Running_;
    }

    i64 GetDone() const
    {
        return Done_;
    }

    i64 GetPending() const
    {
        return Pending_;
    }

    i64 GetFailed() const
    {
        return Failed_;
    }


    void Start(i64 count)
    {
        Running_ += count;
        Pending_ -= count;
    }

    void Completed(i64 count)
    {
        Running_ -= count;
        Done_ += count;
    }

    void Failed(i64 count)
    {
        Running_ -= count;
        Pending_ += count;
        Failed_ += count;
    }

private:
    i64 Total_;
    i64 Running_;
    i64 Done_;
    i64 Pending_;
    i64 Failed_;
};

////////////////////////////////////////////////////////////////////

class TMapController
    : public TOperationControllerBase
{
public:
    TMapController(IOperationHost* host, TOperation* operation)
        : TOperationControllerBase(host, operation)
    { }

    virtual void Initialize()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        Spec = New<TMapOperationSpec>();
        try {
            Spec->Load(~Operation->GetSpec());
        } catch (const std::exception& ex) {
            ythrow yexception() << Sprintf("Error parsing operation spec\n%s", ex.what());
        }

        if (Spec->In.empty()) {
            // TODO(babenko): is this an error?
            ythrow yexception() << "No input tables are given";
        }
    }

    virtual TFuture<TVoid>::TPtr Prepare()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return MakeFuture(TVoid())
            ->Apply(BindBackgroundTask(&TThis::StartPrimaryTransaction, this))
            ->Apply(BindBackgroundTask(&TThis::OnPrimaryTransactionStarted, this))
            ->Apply(BindBackgroundTask(&TThis::StartSeconaryTransactions, this))
            ->Apply(BindBackgroundTask(&TThis::OnSecondaryTransactionsStarted, this))
            ->Apply(BindBackgroundTask(&TThis::RequestInputs, this))
            ->Apply(BindBackgroundTask(&TThis::OnInputsReceived, this))
            ->Apply(BindBackgroundTask(&TThis::CompletePreparation, this));
    }


    virtual void OnJobCompleted(TJobPtr job)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto jobInfo = GetJobInfo(job);

        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto chunkListId = jobInfo->OutputChunkListIds[index];
            OutputTables[index].DoneChunkListIds.push_back(chunkListId);
        }

        JobCounter.Completed(1);
        ChunkCounter.Completed(jobInfo->ChunkIndexes.size());

        RemoveJobInfo(job);
    }

    virtual void OnJobFailed(TJobPtr job)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto jobInfo = GetJobInfo(job);

        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto chunkListId = jobInfo->OutputChunkListIds[index];
            OutputTables[index].DoneChunkListIds.push_back(chunkListId);
        }

        JobCounter.Failed(1);
        ChunkCounter.Failed(jobInfo->ChunkIndexes.size());

        ReleaseChunkLists(jobInfo->OutputChunkListIds);

        RemoveJobInfo(job);

        // TODO(babenko): make configurable
        if (JobCounter.GetFailed() > 10) {
            OnOperationFailed(TError("%d jobs failed, aborting the operation",
                JobCounter.GetFailed()));
        }
    }


    virtual void OnOperationAborted()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        AbortOperation();
    }


    virtual void ScheduleJobs(
        TExecNodePtr node,
        std::vector<TJobPtr>* jobsToStart,
        std::vector<TJobPtr>* jobsToAbort)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        
        // Check if we have any unassigned chunks left.
        if (ChunkCounter.GetPending() == 0) {
            LOG_DEBUG("No pending chunks left, ignoring scheduling request");
            return;
        }

        // Check if we have enough chunk lists in the pool.
        if (ChunkListPool->GetSize() < OutputTables.size()) {
            LOG_DEBUG("No chunk lists in pool left, ignoring scheduling request");
            // TODO(babenko): make configurable
            ChunkListPool->Allocate(OutputTables.size() * 10);
            return;
        }

        // We've got a job to do! :)
        
        // Make a copy of the generic spec and customize it.
        auto jobSpec = GenericJobSpec;
        auto* mapJobSpec = jobSpec.MutableExtension(TMapJobSpec::map_job_spec);

        i64 pendingJobs = JobCounter.GetPending();
        YASSERT(pendingJobs > 0);
        i64 pendingWeight = WeightCounter.GetPending();
        YASSERT(pendingWeight > 0);
        int maxWeightPerJob = (pendingWeight + pendingJobs - 1) / pendingJobs;

        auto jobInfo = New<TJobInfo>();

        // Allocate chunks for the job.
        auto& chunkIndexes = jobInfo->ChunkIndexes;
        i64 allocatedWeight;
        int localCount;
        int remoteCount;
        ChunkAllocationMap->AllocateChunks(
            node->GetAddress(),
            maxWeightPerJob,
            &chunkIndexes,
            &allocatedWeight,
            &localCount,
            &remoteCount);

        LOG_DEBUG("Allocated %d input chunks for node %s (AllocatedWeight: %" PRId64 ", MaxWeight: %" PRId64 ", LocalCount: %d, RemoteCount: %d)",
            static_cast<int>(chunkIndexes.size()),
            ~node->GetAddress(),
            allocatedWeight,
            maxWeightPerJob,
            localCount,
            remoteCount);

        YASSERT(!chunkIndexes.empty());

        FOREACH (int chunkIndex, chunkIndexes) {
            const auto& chunk = ChunkAllocationMap->GetChunk(chunkIndex);
            *mapJobSpec->mutable_input_spec()->add_chunks() = chunk;
        }

        FOREACH (auto& outputSpec, *mapJobSpec->mutable_output_specs()) {
            auto chunkListId = ChunkListPool->Extract();
            jobInfo->OutputChunkListIds.push_back(chunkListId);
            outputSpec.set_chunk_list_id(chunkListId.ToProto());
        }
        
        auto job = Host->CreateJob(
            Operation,
            node,
            jobSpec);

        PutJobInfo(job, jobInfo);

        JobCounter.Start(1);
        ChunkCounter.Start(chunkIndexes.size());
        WeightCounter.Start(allocatedWeight);

        jobsToStart->push_back(job);
    }

    virtual i64 GetPendingJobCount()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return JobCounter.GetPending();
    }

private:
    typedef TMapController TThis;

    TMapOperationSpecPtr Spec;

    // The primary transaction for the whole operation (nested inside operation's transaction).
    ITransaction::TPtr PrimaryTransaction;
    // The transaction for reading input tables (nested inside the primary one).
    // These tables are locked with Snapshot mode.
    ITransaction::TPtr InputTransaction;
    // The transaction for writing output tables (nested inside the primary one).
    // These tables are locked with Shared mode.
    ITransaction::TPtr OutputTransaction;

    // Input tables.
    struct TInputTable
    {
        TTableYPathProxy::TRspFetch::TPtr FetchResponse;
    };

    std::vector<TInputTable> InputTables;

    // Output tables.
    struct TOutputTable
    {
        TYson Schema;
        TChunkListId OutputChunkListId;
        std::vector<TChunkListId> DoneChunkListIds;
    };

    std::vector<TOutputTable> OutputTables;

    // Files.
    struct TFile
    {
        TFileYPathProxy::TRspFetch::TPtr FetchResponse;
    };

    std::vector<TFile> Files;

    // Running counters.
    TRunningCounter JobCounter;
    TRunningCounter ChunkCounter;
    TRunningCounter WeightCounter;

    // Size estimates.
    i64 TotalRowCount;
    i64 TotalDataSize;

    ::THolder<TChunkAllocationMap> ChunkAllocationMap;
    TChunkListPoolPtr ChunkListPool;

    // The template for starting new jobs.
    TJobSpec GenericJobSpec;

    // Job scheduled so far.
    struct TJobInfo
        : public TIntrinsicRefCounted
    {
        // Chunk indexes assigned to this job.
        std::vector<int> ChunkIndexes;
        // Chunk lists allocated to store the result of this job (one per each output table).
        std::vector<TChunkListId> OutputChunkListIds;
    };

    typedef TIntrusivePtr<TJobInfo> TJobInfoPtr;

    yhash_map<TJobPtr, TJobInfoPtr> JobInfos;


    // Helpers for constructing a pipeline.
    template <class TTarget, class TIn, class TOut>
    TIntrusivePtr< IParamFunc<TIn, TIntrusivePtr< TFuture<TOut> > > >
    BindBackgroundTask(TIntrusivePtr< TFuture<TOut> > (TTarget::*method)(TIn), TTarget* target)
    {
        return FromMethod(method, MakeWeak(target))->AsyncVia(Host->GetBackgroundInvoker());
    }

    template <class TTarget, class TIn, class TOut>
    TIntrusivePtr< IParamFunc<TIn, TIntrusivePtr< TFuture<TOut> > > >
    BindBackgroundTask(TOut (TTarget::*method)(TIn), TTarget* target)
    {
        return FromMethod(method, MakeWeak(target))->AsyncVia(Host->GetBackgroundInvoker());
    }


    void PutJobInfo(TJobPtr job, TJobInfoPtr jobInfo)
    {
        YVERIFY(JobInfos.insert(MakePair(job, jobInfo)).second);
    }

    TJobInfoPtr GetJobInfo(TJobPtr job)
    {
        auto it = JobInfos.find(job);
        YASSERT(it != JobInfos.end());
        return it->second;
    }

    void RemoveJobInfo(TJobPtr job)
    {
        YVERIFY(JobInfos.erase(job) == 1);
    }

    
    // Unsorted helpers.

    void InitGenericJobSpec()
    {
        TJobSpec jobSpec;
        jobSpec.set_type(EJobType::Map);

        TUserJobSpec userJobSpec;
        userJobSpec.set_shell_comand(Spec->ShellCommand);
        FOREACH (const auto& file, Files) {
            *userJobSpec.add_files() = *file.FetchResponse;
        }
        *jobSpec.MutableExtension(TUserJobSpec::user_job_spec) = userJobSpec;

        TMapJobSpec mapJobSpec;
        mapJobSpec.set_input_transaction_id(InputTransaction->GetId().ToProto());
        FOREACH (const auto& table, OutputTables) {
            auto* outputSpec = mapJobSpec.add_output_specs();
            outputSpec->set_schema(table.Schema);
        }
        *jobSpec.MutableExtension(TMapJobSpec::map_job_spec) = mapJobSpec;

        // TODO(babenko): stderr
    }

    void ReleaseChunkLists(const std::vector<TChunkListId>& ids)
    {
        auto batchReq = CypressProxy.ExecuteBatch();
        FOREACH (const auto& id, ids) {
            auto req = TTransactionYPathProxy::ReleaseObject();
            req->set_object_id(id.ToProto());
            batchReq->AddRequest(req);
        }
        // Fire-and-forget.
        // TODO(babenko): log result
        batchReq->Invoke();
    }

    // TODO(babenko): YPath and RPC responses currently share no base class
    template <class TResponse>
    bool CheckResponse(TResponse response, const Stroka& failureMessage) 
    {
        if (response->IsOK()) {
            return true;
        } else {
            OnOperationFailed(TError(failureMessage + "\n" + response->GetError().ToString()));
            return false;
        }
    }


    // Here comes the preparation pipeline.

    // Round 1:
    // - Start primary transaction.

    TCypressServiceProxy::TInvExecuteBatch::TPtr StartPrimaryTransaction(TVoid)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        auto batchReq = CypressProxy.ExecuteBatch();

        {
            auto req = TTransactionYPathProxy::CreateObject(FromObjectId(Operation->GetTransactionId()));
            req->set_type(EObjectType::Transaction);
            batchReq->AddRequest(req, "start_primary_tx");
        }

        return batchReq->Invoke();
    }

    TVoid OnPrimaryTransactionStarted(TCypressServiceProxy::TRspExecuteBatch::TPtr batchRsp)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        if (!CheckResponse(batchRsp, "Error creating primary transaction")) {
            return TVoid();
        }

        {
            auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_primary_tx");
            if (!CheckResponse(rsp, "Error creating primary transaction")) {
                return TVoid();
            }
            auto id = TTransactionId::FromProto(rsp->object_id());
            PrimaryTransaction = Host->GetTransactionManager()->Attach(id);
        }

        return TVoid();
    }

    // Round 2:
    // - Start input transaction.
    // - Start output transaction.

    TCypressServiceProxy::TInvExecuteBatch::TPtr StartSeconaryTransactions(TVoid)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        auto batchReq = CypressProxy.ExecuteBatch();

        {
            auto req = TTransactionYPathProxy::CreateObject(FromObjectId(PrimaryTransaction->GetId()));
            req->set_type(EObjectType::Transaction);
            batchReq->AddRequest(req, "start_input_tx");
        }

        {
            auto req = TTransactionYPathProxy::CreateObject(FromObjectId(PrimaryTransaction->GetId()));
            req->set_type(EObjectType::Transaction);
            batchReq->AddRequest(req, "start_output_tx");
        }

        return batchReq->Invoke();
    }

    TVoid OnSecondaryTransactionsStarted(TCypressServiceProxy::TRspExecuteBatch::TPtr batchRsp)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        if (!CheckResponse(batchRsp, "Error creating secondary transactions")) {
            return TVoid();
        }

        {
            auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_input_tx");
            if (!CheckResponse(rsp, "Error creating input transaction")) {
                return TVoid();
            }
            auto id = TTransactionId::FromProto(rsp->object_id());
            LOG_INFO("Input transaction id is %s", ~id.ToString());
            InputTransaction = Host->GetTransactionManager()->Attach(id);
        }

        {
            auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_output_tx");
            if (!CheckResponse(rsp, "Error creating output transaction")) {
                return TVoid();
            }
            auto id = TTransactionId::FromProto(rsp->object_id());
            LOG_INFO("Output transaction id is %s", ~id.ToString());
            OutputTransaction = Host->GetTransactionManager()->Attach(id);
        }

        return TVoid();
    }

    // Round 3: 
    // - Fetch input tables.
    // - Lock input tables.
    // - Lock output tables.
    // - Fetch files.
    // - Get output tables schemata.
    // - Get output chunk lists.

    TCypressServiceProxy::TInvExecuteBatch::TPtr RequestInputs(TVoid)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        LOG_INFO("Requesting inputs");

        auto batchReq = CypressProxy.ExecuteBatch();

        FOREACH (const auto& path, Spec->In) {
            auto req = TTableYPathProxy::Fetch(WithTransaction(path, PrimaryTransaction->GetId()));
            req->set_fetch_holder_addresses(true);
            req->set_fetch_chunk_attributes(true);
            batchReq->AddRequest(req, "fetch_in_tables");
        }

        FOREACH (const auto& path, Spec->In) {
            auto req = TCypressYPathProxy::Lock(WithTransaction(path, InputTransaction->GetId()));
            req->set_mode(ELockMode::Snapshot);
            batchReq->AddRequest(req, "lock_in_tables");
        }

        FOREACH (const auto& path, Spec->Out) {
            auto req = TCypressYPathProxy::Lock(WithTransaction(path, OutputTransaction->GetId()));
            req->set_mode(ELockMode::Shared);
            batchReq->AddRequest(req, "lock_out_tables");
        }

        FOREACH (const auto& path, Spec->Out) {
            auto req = TYPathProxy::Get(CombineYPaths(
                WithTransaction(path, Operation->GetTransactionId()),
                "@schema"));
            batchReq->AddRequest(req, "get_out_tables_schemata");
        }

        FOREACH (const auto& path, Spec->Files) {
            auto req = TFileYPathProxy::Fetch(WithTransaction(path, Operation->GetTransactionId()));
            batchReq->AddRequest(req, "fetch_files");
        }

        FOREACH (const auto& path, Spec->Out) {
            auto req = TTableYPathProxy::GetChunkListForUpdate(WithTransaction(path, OutputTransaction->GetId()));
            batchReq->AddRequest(req, "get_chunk_lists");
        }

        return batchReq->Invoke();
    }

    TVoid OnInputsReceived(TCypressServiceProxy::TRspExecuteBatch::TPtr batchRsp)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        if (!CheckResponse(batchRsp, "Error requesting inputs")) {
            return TVoid();
        }

        {
            InputTables.resize(Spec->In.size());
            TotalRowCount = 0;
            auto fetchInTablesRsps = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch_in_tables");
            auto lockInTablesRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_in_tables");
            for (int index = 0; index < static_cast<int>(Spec->In.size()); ++index) {
                auto lockInTableRsp = lockInTablesRsps[index];
                if (!CheckResponse(lockInTableRsp, "Error locking input table")) {
                    return TVoid();
                }

                auto fetchInTableRsp = fetchInTablesRsps[index];
                if (!CheckResponse(fetchInTableRsp, "Error fetching input input table")) {
                    return TVoid();
                }

                auto& table = InputTables[index];
                table.FetchResponse = fetchInTableRsp;
            }
        }

        {
            OutputTables.resize(Spec->Out.size());
            auto lockOutTablesRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_out_tables");
            auto getChunkListsRsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetChunkListForUpdate>("get_chunk_lists");
            auto getOutTablesSchemataRsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetChunkListForUpdate>("get_out_tables_schemata");
            for (int index = 0; index < static_cast<int>(Spec->Out.size()); ++index) {
                auto lockOutTablesRsp = lockOutTablesRsps[index];
                if (!CheckResponse(lockOutTablesRsp, "Error fetching input input table")) {
                    return TVoid();
                }

                auto getChunkListRsp = getChunkListsRsps[index];
                if (!CheckResponse(getChunkListRsp, "Error getting output chunk list")) {
                    return TVoid();
                }

                auto getOutTableSchemaRsp = getOutTablesSchemataRsps[index];

                auto& table = OutputTables[index];
                table.OutputChunkListId = TChunkListId::FromProto(getChunkListRsp->chunk_list_id());
                // TODO(babenko): fill output schema
                table.Schema = "{}";
            }
        }

        {
            auto fetchFilesRsps = batchRsp->GetResponses<TFileYPathProxy::TRspFetch>("fetch_files");
            FOREACH (auto fetchFileRsp, fetchFilesRsps) {
                if (!CheckResponse(fetchFileRsp, "Error fetching files")) {
                    return TVoid();
                }
                TFile file;
                file.FetchResponse = fetchFileRsp;
                Files.push_back(file);
            }
        }

        LOG_INFO("Inputs received");

        return TVoid();
    }

    // Round 4.
    // - Compute ???

    TVoid CompletePreparation(TVoid)
    {
        return TVoid();
    }
    


    // Here comes the completion pipeline.

    void Complete()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        MakeFuture(TVoid())
            ->Apply(BindBackgroundTask(&TThis::CommitOutputs, this))
            ->Apply(BindBackgroundTask(&TThis::OnOutputsCommitted, this));
    }

    // Round 1.
    // - Attach output chunk lists.
    // - Commit input transaction.
    // - Commit output transaction.
    // - Commit primary transaction.

    TCypressServiceProxy::TInvExecuteBatch::TPtr CommitOutputs(TVoid)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        LOG_INFO("Committing outputs (ChunkCount: %d)",
            ChunkCounter.GetDone());

        auto batchReq = CypressProxy.ExecuteBatch();

        FOREACH (const auto& table, OutputTables) {
            auto req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
            FOREACH (const auto& childId, table.DoneChunkListIds) {
                req->add_children_ids(childId.ToProto());
            }
            batchReq->AddRequest(req, "attach_chunk_lists");
        }

        {
            auto req = TTransactionYPathProxy::Commit(FromObjectId(InputTransaction->GetId()));
            batchReq->AddRequest(req, "commit_input_tx");
        }

        {
            auto req = TTransactionYPathProxy::Commit(FromObjectId(OutputTransaction->GetId()));
            batchReq->AddRequest(req, "commit_output_tx");
        }

        {
            auto req = TTransactionYPathProxy::Commit(FromObjectId(PrimaryTransaction->GetId()));
            batchReq->AddRequest(req, "commit_primary_tx");
        }

        return batchReq->Invoke();
    }

    TVoid OnOutputsCommitted(TCypressServiceProxy::TRspExecuteBatch::TPtr batchRsp)
    {
        VERIFY_THREAD_AFFINITY(BackgroundThread);

        if (!CheckResponse(batchRsp, "Error committing outputs")) {
            return TVoid();
        }

        {
            auto rsp = batchRsp->GetResponse("attach_chunk_lists");
            if (!CheckResponse(rsp, "Error attaching chunk lists")) {
                return TVoid();
            }       
        }

        {
            auto rsp = batchRsp->GetResponse("commit_input_tx");
            if (!CheckResponse(rsp, "Error committing input transaction")) {
                return TVoid();
            }
        }

        {
            auto rsp = batchRsp->GetResponse("commit_output_tx");
            if (!CheckResponse(rsp, "Error committing output transaction")) {
                return TVoid();
            }
        }

        {
            auto rsp = batchRsp->GetResponse("commit_primary_tx");
            if (!CheckResponse(rsp, "Error committing primary transaction")) {
                return TVoid();
            }
        }

        LOG_INFO("Outputs committed");

        OnOperationCompleted();
    }


    // Abortion... not a pipeline really :)

    virtual void AbortOperation()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Aborting operation");

        AbortTransactions();

        LOG_INFO("Operation aborted");
    }

    void AbortTransactions()
    {
        LOG_INFO("Aborting operation transactions")
        if (PrimaryTransaction) {
            // This method is async, no problem in using it here.
            PrimaryTransaction->Abort();
        }

        // No need to abort the others.

        PrimaryTransaction.Reset();
        InputTransaction.Reset();
        OutputTransaction.Reset();
    }


};

IOperationControllerPtr CreateMapController(
    IOperationHost* host,
    TOperation* operation)
{
    return New<TMapController>(host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

