#pragma once

#include "common.h"
#include "cypress_manager.h"
#include "node.h"
#include "node_proxy.h"

#include <ytlib/ytree/ypath_service.h>

namespace NYT {
namespace NCypress {

////////////////////////////////////////////////////////////////////////////////

struct TVirtualYPathContext
{
    TNodeId NodeId;
    TTransactionId TransactionId;
    NYTree::TYson Manifest;
};

typedef
    IParamFunc<
        const TVirtualYPathContext&,
        NYTree::IYPathService::TPtr
    >
    TYPathServiceProducer;

INodeTypeHandler::TPtr CreateVirtualTypeHandler(
    TCypressManager* cypressManager,
    ERuntimeNodeType runtypeType,
    const Stroka& typeName,
    TYPathServiceProducer* producer);

INodeTypeHandler::TPtr CreateVirtualTypeHandler(
    TCypressManager* cypressManager,
    ERuntimeNodeType runtypeType,
    const Stroka& typeName,
    NYTree::IYPathService* service);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT
