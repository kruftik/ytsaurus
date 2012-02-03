#pragma once

#include "command.h"

#include <ytlib/ytree/ytree.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct TReadRequest
    : public TRequestBase
{
    NYTree::TYPath Path;
    NYTree::INode::TPtr Stream;

    TReadRequest()
    {
        Register("path", Path);
        Register("stream", Stream)
            .Default()
            .CheckThat(~StreamSpecIsValid);
    }
};

class TReadCommand
    : public TCommandBase<TReadRequest>
{
public:
    TReadCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TReadRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TWriteRequest
    : public TRequestBase
{
    NYTree::TYPath Path;
    NYTree::INode::TPtr Stream;
    NYTree::INode::TPtr Value;

    TWriteRequest()
    {
        Register("path", Path);
        Register("stream", Stream)
            .Default()
            .CheckThat(~StreamSpecIsValid);
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
    TWriteCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TWriteRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

