#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/misc/arithmetic_formula.h>

namespace NYT::NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

class TNodeTrackerConfig
    : public NYTree::TYsonSerializable
{ };

DEFINE_REFCOUNTED_TYPE(TNodeTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

class TNodeGroupConfigBase
    : public NYTree::TYsonSerializable
{
public:
    int MaxConcurrentNodeRegistrations;

    TNodeGroupConfigBase()
    {
        RegisterParameter("max_concurrent_node_registrations", MaxConcurrentNodeRegistrations)
            .Default(5)
            .GreaterThanOrEqual(0);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TNodeGroupConfig
    : public TNodeGroupConfigBase
{
public:
    TBooleanFormula NodeTagFilter;

    TNodeGroupConfig()
    {
        RegisterParameter("node_tag_filter", NodeTagFilter);
    }
};

DEFINE_REFCOUNTED_TYPE(TNodeGroupConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicNodeTrackerConfig
    : public NYTree::TYsonSerializable
{
public:
    THashMap<TString, TNodeGroupConfigPtr> NodeGroups;

    TDuration TotalNodeStatisticsUpdatePeriod;

    bool BanNewNodes;

    TDuration IncrementalNodeStatesGossipPeriod;
    TDuration FullNodeStatesGossipPeriod;

    int MaxConcurrentNodeRegistrations;
    int MaxConcurrentNodeUnregistrations;

    int MaxConcurrentFullHeartbeats;
    int MaxConcurrentIncrementalHeartbeats;

    TDynamicNodeTrackerConfig()
    {
        RegisterParameter("node_groups", NodeGroups)
            .Default();

        RegisterParameter("total_node_statistics_update_period", TotalNodeStatisticsUpdatePeriod)
            .Default(TDuration::Seconds(60));

        RegisterParameter("ban_new_nodes", BanNewNodes)
            .Default(false);

        RegisterParameter("incremental_node_states_gossip_period", IncrementalNodeStatesGossipPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("full_node_states_gossip_period", FullNodeStatesGossipPeriod)
            .Default(TDuration::Minutes(1));

        RegisterParameter("max_concurrent_node_registrations", MaxConcurrentNodeRegistrations)
            .Default(5)
            .GreaterThanOrEqual(0);
        RegisterParameter("max_concurrent_node_unregistrations", MaxConcurrentNodeUnregistrations)
            .Default(5)
            .GreaterThan(0);

        RegisterParameter("max_concurrent_full_heartbeats", MaxConcurrentFullHeartbeats)
            .Default(1)
            .GreaterThan(0);
        RegisterParameter("max_concurrent_incremental_heartbeats", MaxConcurrentIncrementalHeartbeats)
            .Default(10)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicNodeTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerServer
