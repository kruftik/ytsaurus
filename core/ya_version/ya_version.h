#pragma once

#include <util/generic/string.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TString CreateYtVersion(int major, int minor, int patch, const TStringBuf& branch);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

