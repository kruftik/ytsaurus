#pragma once

#include "private.h"
#include "public.h"
#include "driver.h"

#include <ytlib/misc/error.h>

#include <ytlib/ytree/public.h>
#include <ytlib/ytree/yson_serializable.h>

#include <ytlib/yson/consumer.h>
#include <ytlib/yson/parser.h>
#include <ytlib/yson/writer.h>

#include <ytlib/rpc/public.h>
#include <ytlib/rpc/channel.h>

#include <ytlib/chunk_client/public.h>

#include <ytlib/meta_state/public.h>

#include <ytlib/transaction_client/transaction.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/security_client/public.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/scheduler/scheduler_service_proxy.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct TRequest
    : public virtual TYsonSerializable
{
    TRequest()
    {
        SetKeepOptions(true);
    }
};

typedef TIntrusivePtr<TRequest> TRequestPtr;

////////////////////////////////////////////////////////////////////////////////

struct TTransactionalRequest
    : public virtual TRequest
{
    NObjectClient::TTransactionId TransactionId;
    bool PingAncestors;

    TTransactionalRequest()
    {
        RegisterParameter("transaction_id", TransactionId)
            .Default(NObjectClient::NullTransactionId);
        RegisterParameter("ping_ancestor_transactions", PingAncestors)
            .Default(false);
    }
};

typedef TIntrusivePtr<TTransactionalRequest> TTransactionalRequestPtr;

////////////////////////////////////////////////////////////////////////////////

struct TMutatingRequest
    : public virtual TRequest
{
    NMetaState::TMutationId MutationId;

    TMutatingRequest()
    {
        RegisterParameter("mutation_id", MutationId)
            .Default(NMetaState::NullMutationId);
    }
};

typedef TIntrusivePtr<TMutatingRequest> TMutatingRequestPtr;

////////////////////////////////////////////////////////////////////////////////

struct ICommandContext
{
    virtual ~ICommandContext()
    { }

    virtual TDriverConfigPtr GetConfig() = 0;
    virtual NRpc::IChannelPtr GetMasterChannel() = 0;
    virtual NRpc::IChannelPtr GetSchedulerChannel() = 0;
    virtual NChunkClient::IBlockCachePtr GetBlockCache() = 0;
    virtual NTransactionClient::TTransactionManagerPtr GetTransactionManager() = 0;

    virtual const TDriverRequest* GetRequest() = 0;
    virtual TDriverResponse* GetResponse() = 0;

    virtual NYTree::TYsonProducer CreateInputProducer() = 0;
    virtual TAutoPtr<NYson::IYsonConsumer> CreateOutputConsumer() = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct ICommand
{
    virtual ~ICommand()
    { }

    virtual void Execute(ICommandContext* context) = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TCommandBase
    : public ICommand
{
protected:
    ICommandContext* Context;
    bool Replied;

    THolder<NObjectClient::TObjectServiceProxy> ObjectProxy;
    THolder<NScheduler::TSchedulerServiceProxy> SchedulerProxy;

    TRequestPtr UntypedRequest;

    TCommandBase();

    void Prepare();

    void ReplyError(const TError& error);
    void ReplySuccess(const NYTree::TYsonString& yson);

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequest>
class TTypedCommandBase
    : public virtual TCommandBase
{
public:
    virtual void Execute(ICommandContext* context)
    {
        Context = context;
        try {
            ParseRequest();
            Prepare();
            DoExecute();
        } catch (const std::exception& ex) {
            ReplyError(ex);
        }
    }

protected:
    TIntrusivePtr<TRequest> Request;

    virtual void DoExecute() = 0;

private:
    void ParseRequest()
    {
        Request = New<TRequest>();
        try {
            auto arguments = Context->GetRequest()->Arguments;;
            Request = ConvertTo<TIntrusivePtr<TRequest>>(arguments);
            UntypedRequest = Request;
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing command arguments") <<
                ex;
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

class TTransactionalCommand
    : public virtual TCommandBase
{
protected:
    NTransactionClient::TTransactionId GetTransactionId(bool required);
    NTransactionClient::ITransactionPtr GetTransaction(bool required, bool ping);

    void SetTransactionId(NRpc::IClientRequestPtr request, bool required);

private:
    TTransactionalRequestPtr TransactionalRequest;

    TTransactionalRequestPtr GetTransactionalRequest();

};

////////////////////////////////////////////////////////////////////////////////

class TMutatingCommand
    : public virtual TCommandBase
{
protected:
    NMetaState::TMutationId GenerateMutationId();
    void GenerateMutationId(NRpc::IClientRequestPtr request);

private:
    TNullable<NMetaState::TMutationId> CurrentMutationId;
    TMutatingRequestPtr MutatingRequest;

    TMutatingRequestPtr GetMutatingRequest();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

