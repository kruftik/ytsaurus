#pragma once

#include <yt/cpp/roren/yt/state.h>
#include "yt_io_private.h"

#include <yt/cpp/roren/interface/fwd.h>
#include <yt/cpp/roren/interface/roren.h>
#include <yt/cpp/roren/interface/private/fwd.h>

#include <yt/cpp/mapreduce/interface/client.h>

#include <vector>

namespace NRoren::NPrivate {

////////////////////////////////////////////////////////////////////////////////

NYT::IRawJobPtr CreateImpulseJob(const IRawParDoPtr& rawParDo);
IRawParDoPtr CreateGbkImpulseReadNodeParDo(IRawGroupByKeyPtr rawComputation);
IRawParDoPtr CreateCoGbkImpulseReadNodeParDo(
    IRawCoGroupByKeyPtr rawCoGbk,
    std::vector<TRowVtable> rowVtable);

IRawParDoPtr CreateCombineCombinerImpulseReadNodeParDo(IRawCombinePtr rawCombine);
IRawParDoPtr CreateCombineReducerImpulseReadNodeParDo(IRawCombinePtr rawCombine);

IRawParDoPtr CreateStateDecodingParDo(const TYtStateVtable& stateVtable);
IRawParDoPtr CreateStateEncodingParDo(const TYtStateVtable& stateVtable);
IRawParDoPtr CreateStatefulParDoReducerImpulseReadNode(IRawStatefulParDoPtr rawStatefulParDo, const TYtStateVtable& stateVtable);

NYT::IRawJobPtr CreateParDoMap(
    const IRawParDoPtr& rawParDo,
    const IYtJobInputPtr& input,
    const std::vector<IYtJobOutputPtr>& outputs);

NYT::IRawJobPtr CreateSplitKvMap(
    TRowVtable rowVtable);

NYT::IRawJobPtr CreateSplitKvMap(
    const std::vector<TRowVtable>& rowVtables);

NYT::IRawJobPtr CreateSplitStateKvMap(
    const std::vector<TRowVtable>& rowVtables,
    TYtStateVtable stateVtable);

NYT::IRawJobPtr CreateMultiJoinKvReduce(
    const IRawCoGroupByKeyPtr& rawComputation,
    const std::vector<TRowVtable>& inVtables,
    const IYtJobOutputPtr& output);

NYT::IRawJobPtr CreateStatefulKvReduce(
    const IRawStatefulParDoPtr& rawComputation,
    const std::vector<TRowVtable>& inVtables,
    const std::vector<IYtJobOutputPtr>& outputs,
    const TYtStateVtable& stateVtable);

NYT::IRawJobPtr CreateCombineCombiner(
    const IRawCombinePtr& combine,
    const TRowVtable& inRowVtable);

NYT::IRawJobPtr CreateCombineReducer(
    const IRawCombinePtr& combine,
    const TRowVtable& outRowVtable,
    const IYtJobOutputPtr& output);

IExecutionContextPtr CreateYtExecutionContext();

////////////////////////////////////////////////////////////////////////////////

void ProcessOneGroup(const IRawCoGroupByKeyPtr& rawComputation, const IYtNotSerializableJobInputPtr& input, const IRawOutputPtr& rawOutput);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRoren::NPrivate
