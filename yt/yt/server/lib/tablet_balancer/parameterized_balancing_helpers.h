#pragma once

#include "public.h"

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/core/logging/public.h>

#include <yt/yt/library/profiling/sensor.h>

#include <yt/yt/library/query/base/public.h>

namespace NYT::NTabletBalancer {

////////////////////////////////////////////////////////////////////////////////

//! The ultimate goal of this class is to evenly distribute tablets between cells.
//!
//! A metric is calculated for each tablet based on its statistics and performance counters.
//! Then the cell metric is defined as the square of the sum of metrics of tablets belonging to this cell.
//! The node metric is defined as the square of the sum of metrics of tablets belonging to this node.
//! Tablets are moved in order to minimize total sum of cell metric and node metric.
//!
//! There is only one kind of action: move a tablet to another cell.
//! On every step we greedily pick the action which minimizes total metric the most and repeat
//! until maxMoveActionCount is reached.
struct IParameterizedReassignSolver
    : public TRefCounted
{
    virtual std::vector<TMoveDescriptor> BuildActionDescriptors() = 0;
};

DEFINE_REFCOUNTED_TYPE(IParameterizedReassignSolver)

////////////////////////////////////////////////////////////////////////////////

struct IParameterizedResharder
    : public TRefCounted
{
    virtual std::vector<TReshardDescriptor> BuildTableActionDescriptors(const TTablePtr& table) = 0;
};

DEFINE_REFCOUNTED_TYPE(IParameterizedResharder)

////////////////////////////////////////////////////////////////////////////////

struct TTableParameterizedMetricTracker
    : public TRefCounted
{
    NProfiling::TGauge BeforeMetric;
    NProfiling::TGauge AfterMetric;
};

DEFINE_REFCOUNTED_TYPE(TTableParameterizedMetricTracker)

////////////////////////////////////////////////////////////////////////////////

bool IsTableMovable(TTableId tableId);

struct TParameterizedReassignSolverConfig
{
    int MaxMoveActionCount = 0;
    double NodeDeviationThreshold = 0;
    double CellDeviationThreshold = 0;
    double MinRelativeMetricImprovement = 0;
    TString Metric;

    TParameterizedReassignSolverConfig MergeWith(
        const TParameterizedBalancingConfigPtr& groupConfig,
        std::optional<int> maxMoveActionHardLimit = std::nullopt) const;
};

struct TParameterizedResharderConfig
{
    bool EnableReshardByDefault = false;
    TString Metric;

    TParameterizedResharderConfig MergeWith(const TParameterizedBalancingConfigPtr& groupConfig) const;
};

void FormatValue(TStringBuilderBase* builder, const TParameterizedReassignSolverConfig& config, TStringBuf spec);
void FormatValue(TStringBuilderBase* builder, const TParameterizedResharderConfig& config, TStringBuf spec);

////////////////////////////////////////////////////////////////////////////////

IParameterizedReassignSolverPtr CreateParameterizedReassignSolver(
    TTabletCellBundlePtr bundle,
    std::vector<TString> performanceCountersKeys,
    NTableClient::TTableSchemaPtr performanceCountersTableSchema,
    TParameterizedReassignSolverConfig config,
    TString groupName,
    TTableParameterizedMetricTrackerPtr metricTracker,
    const NLogging::TLogger& logger);

IParameterizedResharderPtr CreateParameterizedResharder(
    TTabletCellBundlePtr bundle,
    std::vector<TString> performanceCountersKeys,
    NTableClient::TTableSchemaPtr performanceCountersTableSchema,
    TParameterizedResharderConfig config,
    TString groupName,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletBalancer
