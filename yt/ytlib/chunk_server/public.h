#pragma once

#include <ytlib/misc/common.h>
#include <ytlib/object_server/id.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunk;  
class TChunkList;
class TJob;
class TJobList;
class TDataNode;
class TReplicationSink;

struct TChunkTreeStatistics;
struct TTotalNodeStatistics;

class TChunkTreeRef;

class TChunkManager;
typedef TIntrusivePtr<TChunkManager> TChunkManagerPtr;

struct INodeAuthority;
typedef TIntrusivePtr<INodeAuthority> INodeAuthorityPtr;

class TNodeLeaseTracker;
typedef TIntrusivePtr<TNodeLeaseTracker> TNodeLeaseTrackerPtr;

class TChunkReplicator;
typedef TIntrusivePtr<TChunkReplicator> TChunkReplicatorPtr;

class TChunkPlacement;
typedef TIntrusivePtr<TChunkPlacement> TChunkPlacementPtr;

class TChunkService;
typedef TIntrusivePtr<TChunkService> TChunkServicePtr;

struct TChunkReplicatorConfig;
typedef TIntrusivePtr<TChunkReplicatorConfig> TChunkReplicatorConfigPtr;

struct TChunkTreeBalancerConfig;
typedef TIntrusivePtr<TChunkTreeBalancerConfig> TChunkTreeBalancerConfigPtr;

struct TChunkManagerConfig;
typedef TIntrusivePtr<TChunkManagerConfig> TChunkManagerConfigPtr;

using NObjectServer::TTransactionId;
using NObjectServer::NullTransactionId;

typedef i32 TNodeId;
const i32 InvalidNodeId = -1;

typedef TGuid TIncarnationId;

typedef NObjectServer::TObjectId TChunkId;
extern TChunkId NullChunkId;

typedef NObjectServer::TObjectId TChunkListId;
extern TChunkListId NullChunkListId;

typedef NObjectServer::TObjectId TChunkTreeId;
extern TChunkTreeId NullChunkTreeId;

typedef TGuid TJobId;

DECLARE_ENUM(EJobState,
    (Running)
    (Completed)
    (Failed)
);

DECLARE_ENUM(EJobType,
    (Replicate)
    (Remove)
);

//! Represents an offset inside a chunk.
typedef i64 TBlockOffset;

//! A |(chunkId, blockIndex)| pair.
struct TBlockId;

DECLARE_ENUM(EChunkType,
    ((Unknown)(0))
    ((File)(1))
    ((Table)(2))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NNChunkServer
} // namespace NYT
