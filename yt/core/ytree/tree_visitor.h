#pragma once

#include "ypath_service.h"

#include <yt/core/yson/consumer.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

void VisitTree(
    INodePtr root,
    NYson::IYsonConsumer* consumer,
    const TAttributeFilter& attributeFilter = TAttributeFilter::All,
    bool sortKeys = false,
    bool ignoreOpaque = false);

void VisitTree(
    INodePtr root,
    NYson::IAsyncYsonConsumer* consumer,
    const TAttributeFilter& attributeFilter = TAttributeFilter::All,
    bool sortKeys = false,
    bool ignoreOpaque = false);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
