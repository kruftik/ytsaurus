#pragma once

#include "public.h"

#include <ytlib/node_tracker_client/node_tracker_service.pb.h>

#include <server/cell_node/public.h>

#include <server/hydra/public.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

//! Controls all tablet slots running at this node.
class TTabletCellController
    : public TRefCounted
{
public:
    TTabletCellController(
        NCellNode::TCellNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap);
    ~TTabletCellController();

    void Initialize();

    int GetAvailableTabletSlotCount() const;
    int GetUsedTableSlotCount() const;

    const std::vector<TTabletSlotPtr>& GetSlots() const;

    TTabletSlotPtr FindSlot(const NHydra::TCellGuid& guid);

    void CreateSlot(const NNodeTrackerClient::NProto::TCreateTabletSlotInfo& createInfo);
    void ConfigureSlot(TTabletSlotPtr slot, const NNodeTrackerClient::NProto::TConfigureTabletSlotInfo& configureInfo);
    void RemoveSlot(TTabletSlotPtr slot);

    NHydra::IChangelogCatalogPtr GetChangelogCatalog();
    NHydra::ISnapshotCatalogPtr GetSnapshotCatalog();

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
