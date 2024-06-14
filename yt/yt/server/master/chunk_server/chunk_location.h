#pragma once

#include "chunk_replica.h"

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt_proto/yt/client/node_tracker_client/proto/node.pb.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkLocation
{
private:
    struct TReplicaHasher
    {
        size_t operator()(const TChunkPtrWithReplicaInfo& replica) const;
    };

    struct TReplicaEqual
    {
        bool operator()(const TChunkPtrWithReplicaInfo& lhs, const TChunkPtrWithReplicaInfo& rhs) const;
    };

public:
    // NB: Randomize replica hashing to avoid collisions during balancing.
    using TReplicaSet = THashSet<TChunkPtrWithReplicaInfo, TReplicaHasher, TReplicaEqual>;
    DEFINE_BYREF_RO_PROPERTY(TReplicaSet, Replicas);

    //! Maps replicas to the leader timestamp when this replica was registered by a client.
    using TUnapprovedReplicaMap = THashMap<TChunkPtrWithReplicaIndex, TInstant>;
    DEFINE_BYREF_RW_PROPERTY(TUnapprovedReplicaMap, UnapprovedReplicas);

    using TDestroyedReplicaSet = THashSet<NChunkClient::TChunkIdWithIndex>;
    using TDestroyedReplicaShardedSet = std::array<TDestroyedReplicaSet, ChunkShardCount>;
    DEFINE_BYREF_RO_PROPERTY(TDestroyedReplicaShardedSet, DestroyedReplicas);

    using TChunkQueue = THashSet<NChunkClient::TChunkIdWithIndex>;
    //! Key:
    //!   Encodes chunk and one of its parts (for erasure chunks only, others use GenericChunkReplicaIndex).
    //! Value:
    //!   Indicates medium where removal of this chunk is scheduled.
    DEFINE_BYREF_RW_PROPERTY(TChunkQueue, ChunkRemovalQueue);

    //! Key:
    //!   Indicates an unsealed chunk.
    DEFINE_BYREF_RW_PROPERTY(TChunkQueue, ChunkSealQueue);

    DEFINE_BYVAL_RW_PROPERTY(bool, BeingDisposed);
    DEFINE_BYVAL_RW_PROPERTY(TNode*, Node);

public:
    TChunkLocation() = default;
    virtual ~TChunkLocation() = default;

    virtual bool IsImaginary() const = 0;
    const TRealChunkLocation* AsReal() const;
    TRealChunkLocation* AsReal();
    const TImaginaryChunkLocation* AsImaginary() const;
    TImaginaryChunkLocation* AsImaginary();

    // TODO(kvk1920): Use TDomesticMedium* here.
    virtual int GetEffectiveMediumIndex() const = 0;

    void ReserveReplicas(int sizeHint);

    //! Returns |true| if the replica was actually added.
    bool AddReplica(TChunkPtrWithReplicaInfo replica);
    //! Returns |true| if the replica was approved.
    bool RemoveReplica(TChunkPtrWithReplicaIndex replica);

    bool HasReplica(TChunkPtrWithReplicaIndex replica) const;
    TChunkPtrWithReplicaInfo PickRandomReplica();
    void ClearReplicas();

    void AddUnapprovedReplica(TChunkPtrWithReplicaIndex replica, TInstant timestamp);
    bool HasUnapprovedReplica(TChunkPtrWithReplicaIndex replica) const;
    void ApproveReplica(TChunkPtrWithReplicaInfo replica);

    bool AddDestroyedReplica(const NChunkClient::TChunkIdWithIndex& replica);
    bool RemoveDestroyedReplica(const NChunkClient::TChunkIdWithIndex& replica);
    void ClearDestroyedReplicas();

    i64 GetDestroyedReplicasCount() const;
    const TDestroyedReplicaSet& GetDestroyedReplicaSet(int shardId) const;

    class TDestroyedReplicasIterator;
    void SetDestroyedReplicasIterator(TDestroyedReplicasIterator iterator, int shardId);
    void ResetDestroyedReplicasIterator();
    TDestroyedReplicasIterator GetDestroyedReplicasIterator(int shardId) const;

    void AddToChunkRemovalQueue(const NChunkClient::TChunkIdWithIndex& replica);
    void RemoveFromChunkRemovalQueue(const NChunkClient::TChunkIdWithIndex& replica);

    void AddToChunkSealQueue(const NChunkClient::TChunkIdWithIndex& replica);
    void RemoveFromChunkSealQueue(const NChunkClient::TChunkIdWithIndex& replica);

    void ShrinkHashTables();
    void Reset();

    struct TLoadScratchData;
    TLoadScratchData* GetLoadScratchData() const;
    void ResetLoadScratchData();

protected:
    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

private:
    // TODO(kvk1920): TStrongObjectPtr<TDomesticMedium> EffectiveMedium.
    TReplicaSet::iterator RandomReplicaIter_;
    std::array<TDestroyedReplicaSet::const_iterator, ChunkShardCount> DestroyedReplicasIterators_;

    std::unique_ptr<TLoadScratchData> LoadScratchData_;

    bool DoRemoveReplica(TChunkPtrWithReplicaIndex replica);
    bool DoAddReplica(TChunkPtrWithReplicaInfo replica);
    bool DoHasReplica(TChunkPtrWithReplicaInfo replica) const;
};

class TChunkLocation::TDestroyedReplicasIterator
{
public:
    friend class TChunkLocation;

    TDestroyedReplicasIterator(
        const TDestroyedReplicaSet& replicas,
        TDestroyedReplicaSet::const_iterator start);

    TDestroyedReplicasIterator(const TDestroyedReplicasIterator&) = default;
    TDestroyedReplicasIterator& operator=(const TDestroyedReplicasIterator&) = default;

    //! Returns current destroyed replica.
    NChunkClient::TChunkIdWithIndex operator*() const;

    //! Returns |false| if all replicas were traversed.
    bool Advance();

    //! Returns advanced iterator like std::next().
    TDestroyedReplicasIterator GetNext() const;

private:
    const TDestroyedReplicaSet* Replicas_;
    TDestroyedReplicaSet::const_iterator Start_;
    TDestroyedReplicaSet::const_iterator Current_;
};

struct TChunkLocation::TLoadScratchData
{
    explicit TLoadScratchData(size_t replicaCount);

    std::atomic<int> CurrentReplicaIndex;
    std::vector<TChunkPtrWithReplicaInfo> Replicas;
};

////////////////////////////////////////////////////////////////////////////////

class TImaginaryChunkLocation
    : public TChunkLocation
{
public:
    TImaginaryChunkLocation();
    TImaginaryChunkLocation(int mediumIndex, TNode* node);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    bool IsImaginary() const override;

    bool operator<(const TImaginaryChunkLocation& rhs) const;

    int GetEffectiveMediumIndex() const override;

    static TImaginaryChunkLocation* GetOrCreate(NNodeTrackerServer::TNode* node, int mediumIndex, bool duringSnapshotLoading = false);

private:
    int MediumIndex_ = NChunkClient::GenericMediumIndex;
};

////////////////////////////////////////////////////////////////////////////////

class TRealChunkLocation
    : public NObjectServer::TObject
    , public TChunkLocation
    , public TRefTracked<TRealChunkLocation>
{
public:
    using TObject::TObject;

    DEFINE_BYVAL_RW_PROPERTY(TChunkLocationUuid, Uuid);
    DEFINE_BYVAL_RW_PROPERTY(EChunkLocationState, State, EChunkLocationState::Offline);
    DEFINE_BYREF_RW_PROPERTY(TDomesticMediumPtr, MediumOverride);
    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TChunkLocationStatistics, Statistics);

public:
    bool IsImaginary() const override;

    bool operator<(const TRealChunkLocation& rhs) const;

    int GetEffectiveMediumIndex() const override;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);
};

DEFINE_MASTER_OBJECT_TYPE(TRealChunkLocation)

////////////////////////////////////////////////////////////////////////////////

template <bool WithReplicaState, int IndexCount, template <class> class TAugmentationAccessor>
NNodeTrackerServer::TNode* GetChunkLocationNode(
    TAugmentedPtr<TChunkLocation, WithReplicaState, IndexCount, TAugmentationAccessor> ptr);


template <bool WithReplicaState, int IndexCount, template <class> class TAugmentationAccessor>
NNodeTrackerServer::TNodeId GetChunkLocationNodeId(
    TAugmentedPtr<TChunkLocation, WithReplicaState, IndexCount, TAugmentationAccessor> ptr);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class C>
struct TSerializerTraits<NChunkServer::TChunkLocation*, C>
{
    struct TSerializer
    {
        static void Save(C& context, const NChunkServer::TChunkLocation* location);
        static void Load(C& context, NChunkServer::TChunkLocation*& location);
    };

    struct TComparer
    {
        static bool Compare(
            const NChunkServer::TChunkLocation* lhs,
            const NChunkServer::TChunkLocation* rhs);
    };
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

namespace NYT::NChunkServer::NDetail {

////////////////////////////////////////////////////////////////////////////////

// NB: TNode is incomplete in this header, but we need some way to get its id in chunk_location-inl.h.
TNodeId GetNodeId(TNode* node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer::NDetail

#define CHUNK_LOCATION_INL_H_
#include "chunk_location-inl.h"
#undef CHUNK_LOCATION_INL_H_
