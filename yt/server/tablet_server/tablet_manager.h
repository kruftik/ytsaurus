#pragma once

#include "public.h"

#include <server/hydra/entity_map.h>
#include <server/hydra/mutation.h>

#include <server/cell_master/public.h>

#include <server/object_server/public.h>

#include <server/table_server/public.h>

#include <server/tablet_server/tablet_manager.pb.h>

namespace NYT {
namespace NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletManager
    : public TRefCounted
{
public:
    explicit TTabletManager(
        TTabletManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);
    ~TTabletManager();

    void Initialize();

    NHydra::TMutationPtr CreateStartSlotsMutation(
        const NProto::TMetaReqStartSlots& request);

    NHydra::TMutationPtr CreateSetCellStateMutation(
        const NProto::TMetaReqSetCellState& request);

    NHydra::TMutationPtr CreateRevokePeerMutation(
        const NProto::TMetaReqRevokePeer& request);

    DECLARE_ENTITY_MAP_ACCESSORS(TabletCell, TTabletCell, TTabletCellId);
    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet, TTabletId);

    void MountTable(NTableServer::TTableNode* table);
    void UnmountTable(NTableServer::TTableNode* table);

private:
    class TTabletCellTypeHandler;
    class TTabletTypeHandler;
    class TImpl;
    
    TIntrusivePtr<TImpl> Impl;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
