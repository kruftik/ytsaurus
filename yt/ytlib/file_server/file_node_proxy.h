#pragma once

#include "common.h"
#include "file_node.h"
#include <ytlib/file_server/file_ypath.pb.h>

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/cypress_server/node_proxy_detail.h>
#include <ytlib/chunk_server/chunk_manager.h>
#include <ytlib/cell_master/public.h>

namespace NYT {
namespace NFileServer {

////////////////////////////////////////////////////////////////////////////////

class TFileNodeProxy
    : public NCypressServer::TCypressNodeProxyBase<NYTree::IEntityNode, TFileNode>
{
public:
    TFileNodeProxy(
        NCypressServer::INodeTypeHandlerPtr typeHandler,
        NCellMaster::TBootstrap* bootstrap,
        NTransactionServer::TTransaction* transaction,
        const NCypressServer::TNodeId& nodeId);

    bool IsExecutable();
    Stroka GetFileName();

private:
    typedef NCypressServer::TCypressNodeProxyBase<NYTree::IEntityNode, TFileNode> TBase;

    virtual void DoCloneTo(TFileNode* clonedNode);

    virtual void GetSystemAttributes(std::vector<TAttributeInfo>* attributes);
    virtual bool GetSystemAttribute(const Stroka& name, NYTree::IYsonConsumer* consumer);
    virtual void OnUpdateAttribute(
        const Stroka& key,
        const TNullable<NYTree::TYsonString>& oldValue,
        const TNullable<NYTree::TYsonString>& newValue);

    virtual void DoInvoke(NRpc::IServiceContextPtr context);

    DECLARE_RPC_SERVICE_METHOD(NProto, Fetch);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileServer
} // namespace NYT

