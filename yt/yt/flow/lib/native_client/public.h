#pragma once

#include <yt/yt/flow/lib/client/public.h>

namespace NYT::NFlow {

////////////////////////////////////////////////////////////////////////////////

inline const TString InputMessagesTableName = "input_messages";
inline const TString OutputMessagesTableName = "output_messages";
inline const TString PartitionDataTableName = "partition_data";
inline const TString InternalMessagesTableName = "internal_messages";

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFlow
