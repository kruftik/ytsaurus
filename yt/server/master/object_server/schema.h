#pragma once

#include "object.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/security_server/acl.h>

namespace NYT::NObjectServer {

////////////////////////////////////////////////////////////////////////////////

//! A schema (i.e. metaclass) object.
class TSchemaObject
    : public TNonversionedObjectBase
{
public:
    explicit TSchemaObject(TObjectId id);

    DEFINE_BYREF_RW_PROPERTY(NSecurityServer::TAccessControlDescriptor, Acd);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);
};

////////////////////////////////////////////////////////////////////////////////

IObjectTypeHandlerPtr CreateSchemaTypeHandler(
    NCellMaster::TBootstrap* bootstrap,
    EObjectType type);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
