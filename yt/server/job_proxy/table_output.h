﻿#pragma once

#include <ytlib/yson/public.h>
#include <ytlib/formats/public.h>
#include <ytlib/table_client/public.h>
#include <util/stream/output.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////

class TTableOutput
    : public TOutputStream
{
public:
    TTableOutput(
        std::unique_ptr<NFormats::IParser> parser,
        std::unique_ptr<NYson::IYsonConsumer> consumer);

    ~TTableOutput() throw();

private:
    void DoWrite(const void* buf, size_t len);
    void DoFinish();

    std::unique_ptr<NFormats::IParser> Parser;

    // Just holds the consumer that parser is using.
    std::unique_ptr<NYson::IYsonConsumer> Consumer;

    bool IsParserValid;
};

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
