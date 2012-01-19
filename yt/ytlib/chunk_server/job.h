#pragma once

#include "common.h"

#include <ytlib/chunk_client/common.h>
#include <ytlib/chunk_holder/chunk_holder_service_proxy.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TJob
{
    TJob(
        NChunkHolder::EJobType type,
        const NChunkHolder::TJobId& jobId,
        const NChunkClient::TChunkId& chunkId,
        const Stroka& runnerAddress,
        const yvector<Stroka>& targetAddresses,
        TInstant startTime)
        : Type_(type)
        , JobId_(jobId)
        , ChunkId_(chunkId)
        , RunnerAddress_(runnerAddress)
        , TargetAddresses_(targetAddresses)
        , StartTime_(startTime)
    { }

    TJob(const TJob& other)
        : Type_(other.Type_)
        , JobId_(other.JobId_)
        , ChunkId_(other.ChunkId_)
        , RunnerAddress_(other.RunnerAddress_)
        , TargetAddresses_(other.TargetAddresses_)
        , StartTime_(other.StartTime_)
    { }

    TAutoPtr<TJob> Clone()
    {
        return new TJob(*this);
    }

    void Save(TOutputStream* output) const
    {
        ::Save(output, Type_);
        ::Save(output, ChunkId_);
        ::Save(output, RunnerAddress_);
        ::Save(output, TargetAddresses_);
        ::Save(output, StartTime_);
    }

    static TAutoPtr<TJob> Load(const NChunkHolder::TJobId& jobId, TInputStream* input)
    {
        NChunkHolder::EJobType type;
        NChunkClient::TChunkId chunkId;
        Stroka runnerAddress;
        yvector<Stroka> targetAddresses;
        TInstant startTime;
        ::Load(input, type);
        ::Load(input, chunkId);
        ::Load(input, runnerAddress);
        ::Load(input, targetAddresses);
        ::Load(input, startTime);
        return new TJob(
            type,
            jobId, 
            chunkId, 
            runnerAddress, 
            targetAddresses, 
            startTime);
    }

    DEFINE_BYVAL_RO_PROPERTY(NChunkHolder::EJobType, Type);
    DEFINE_BYVAL_RO_PROPERTY(NChunkHolder::TJobId, JobId);
    DEFINE_BYVAL_RO_PROPERTY(NChunkClient::TChunkId, ChunkId);
    DEFINE_BYVAL_RO_PROPERTY(Stroka, RunnerAddress);
    DEFINE_BYREF_RO_PROPERTY(yvector<Stroka>, TargetAddresses);
    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
