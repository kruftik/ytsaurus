#pragma once

#include "public.h"

#include <ytlib/actions/bind.h>
#include <ytlib/actions/signal.h>
#include <server/cell_master/public.h>
#include <ytlib/misc/property.h>
#include <ytlib/misc/id_generator.h>
#include <ytlib/misc/lease_manager.h>
#include <ytlib/meta_state/meta_state_manager.h>
#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/mutation.h>
#include <ytlib/meta_state/map.h>
#include <server/object_server/public.h>

namespace NYT {
namespace NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

//! Manages client transactions.
class TTransactionManager
    : public NMetaState::TMetaStatePart
{
    //! Raised when a new transaction is started.
    DEFINE_SIGNAL(void(TTransaction*), TransactionStarted);

    //! Raised when a transaction is committed.
    DEFINE_SIGNAL(void(TTransaction*), TransactionCommitted);

    //! Raised when a transaction is aborted.
    DEFINE_SIGNAL(void(TTransaction*), TransactionAborted);

public:
    typedef TIntrusivePtr<TTransactionManager> TPtr;

    //! Creates an instance.
    TTransactionManager(
        TTransactionManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    void Init();

    NObjectServer::IObjectProxyPtr GetRootTransactionProxy();

    TTransaction* Start(TTransaction* parent, TNullable<TDuration> timeout);
    void Commit(TTransaction* transaction);
    void Abort(TTransaction* transaction);
    void RenewLease(const TTransaction* transaction, bool renewAncestors = false);

    DECLARE_METAMAP_ACCESSORS(Transaction, TTransaction, TTransactionId);

    //! Returns the list of all transaction ids on the path up to the root.
    //! This list includes #transactionId itself and #NullTransactionId.
    std::vector<TTransaction*> GetTransactionPath(TTransaction* transaction) const;

private:
    typedef TTransactionManager TThis;
    class TTransactionTypeHandler;
    class TTransactionProxy;
    friend class TTransactionProxy;

    TTransactionManagerConfigPtr Config;
    NCellMaster::TBootstrap* Bootstrap;

    NMetaState::TMetaStateMap<TTransactionId, TTransaction> TransactionMap;
    yhash_map<TTransactionId, TLeaseManager::TLease> LeaseMap;

    void OnTransactionExpired(const TTransactionId& id);

    void CreateLease(const TTransaction* transaction, TDuration timeout);
    void CloseLease(const TTransaction* transaction);
    void FinishTransaction(TTransaction* transaction);

    void DoRenewLease(const TTransaction* transaction);

    // TMetaStatePart overrides
    virtual void OnActiveQuorumEstablished() override;
    virtual void OnStopLeading() override;

    void SaveKeys(TOutputStream* output);
    void SaveValues(TOutputStream* output);
    void LoadKeys(TInputStream* input);
    void LoadValues(NCellMaster::TLoadContext context, TInputStream* input);
    virtual void Clear() override;

    TDuration GetActualTimeout(TNullable<TDuration> timeout);

    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionServer
} // namespace NYT
