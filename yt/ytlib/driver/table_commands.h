#pragma once

#include "command.h"

#include <ytlib/ytree/public.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct TReadRequest
    : public TTransactedRequest
{
    NYTree::TYPath Path;
    NYTree::INodePtr Stream;

    TReadRequest()
    {
        Register("path", Path);
    }
};

class TReadCommand
    : public TCommandBase<TReadRequest>
{
public:
    TReadCommand(ICommandHost* commandHost)
        : TCommandBase(commandHost)
    { }

private:
    virtual void DoExecute(TReadRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TWriteRequest
    : public TTransactedRequest
{
    NYTree::TYPath Path;
    NYTree::INodePtr Stream;
    NYTree::INodePtr Value;

    TWriteRequest()
    {
        Register("path", Path);
        Register("value", Value)
            .Default();
    }

    virtual void DoValidate() const
    {
        if (Value) {
            auto type = Value->GetType();
            if (type != NYTree::ENodeType::List && type != NYTree::ENodeType::Map) {
                ythrow yexception() << "\"value\" must be a list or a map";
            }
        }
    }
};

class TWriteCommand
    : public TCommandBase<TWriteRequest>
{
public:
    TWriteCommand(ICommandHost* commandHost)
        : TCommandBase(commandHost)
    { }

private:
    virtual void DoExecute(TWriteRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

