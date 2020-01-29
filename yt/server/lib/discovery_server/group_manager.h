#pragma once

#include "config.h"
#include "helpers.h"
#include "public.h"

#include <yt/core/rpc/server.h>

#include <yt/core/concurrency/rw_spinlock.h>
#include <yt/core/concurrency/action_queue.h>

#include <yt/ytlib/discovery_client/helpers.h>

namespace NYT::NDiscoveryServer {

////////////////////////////////////////////////////////////////////////////////

class TGroupManager
    : public TRefCounted
{
public:
    explicit TGroupManager(const NLogging::TLogger& logger);

    void ProcessGossip(const std::vector<TGossipMemberInfo>& membersBatch);
    void ProcessHeartbeat(
        const TGroupId& groupId,
        const NDiscoveryClient::TMemberInfo& memberInfo,
        TDuration leaseTimeout);

    TGroupPtr GetGroupOrThrow(const TGroupId& id);
    std::vector<TGroupPtr> ListGroups();

    THashSet<TMemberPtr> GetModifiedMembers();

private:
    const NLogging::TLogger Logger;

    NConcurrency::TReaderWriterSpinLock GroupsLock_;
    THashMap<TGroupId, TGroupPtr> IdToGroup_;

    TSpinLock ModifiedMembersLock_;
    THashSet<TMemberPtr> ModifiedMembers_;

    THashMap<TGroupId, TGroupPtr> GetOrCreateGroups(const std::vector<TGroupId>& groupIds);
    TGroupPtr FindGroup(const TGroupId& id);
};

DEFINE_REFCOUNTED_TYPE(TGroupManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryServer
