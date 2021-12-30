#pragma once

#include "public.h"

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

//! Returns true if inside a Hive mutation but not in a boomerang one.
bool IsSubordinateMutation();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
