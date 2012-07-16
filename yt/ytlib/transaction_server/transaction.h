#pragma once

#include "public.h"

#include <ytlib/cell_master/public.h>
#include <ytlib/misc/property.h>
#include <ytlib/cypress_client/id.h>
#include <ytlib/cypress/public.h>
#include <ytlib/chunk_server/public.h>
#include <ytlib/object_server/object_detail.h>

namespace NYT {
namespace NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ETransactionState,
    (Active)
    (Committed)
    (Aborted)
);

class TTransaction
    : public NObjectServer::TObjectWithIdBase
{
    DEFINE_BYVAL_RW_PROPERTY(ETransactionState, State);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TTransaction*>, NestedTransactions);
    DEFINE_BYVAL_RW_PROPERTY(TTransaction*, Parent);

    // Object Manager stuff
    DEFINE_BYREF_RW_PROPERTY(yhash_set<NObjectServer::TObjectId>, CreatedObjectIds);

    // Cypress stuff
    DEFINE_BYREF_RW_PROPERTY(std::vector<NCypress::ICypressNode*>, LockedNodes);
    DEFINE_BYREF_RW_PROPERTY(std::vector<NCypress::ICypressNode*>, BranchedNodes);
    DEFINE_BYREF_RW_PROPERTY(std::vector<NCypress::ICypressNode*>, CreatedNodes);

public:
    explicit TTransaction(const TTransactionId& id);

    void Save(TOutputStream* output) const;
    void Load(const NCellMaster::TLoadContext& context, TInputStream* input);

    bool IsActive() const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionServer
} // namespace NYT
