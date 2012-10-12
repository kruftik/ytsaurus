#include "stdafx.h"
#include "assert.h"
#include "raw_formatter.h"

#include <ytlib/logging/log_manager.h>

#include <io.h>

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

void AssertTrapImpl(
    const char* trapType, 
    const char* expr,
    const char* file,
    int line)
{
    TRawFormatter<1024> formatter;
    formatter.AppendString(trapType);
    formatter.AppendString("(");
    formatter.AppendString(expr);
    formatter.AppendString(") at ");
    formatter.AppendString(file);
    formatter.AppendString(":");
    formatter.AppendNumber(line);
    formatter.AppendString("\n");

    auto unused = ::write(2, formatter.GetData(), formatter.GetBytesWritten());

    NLog::TLogManager::Get()->Shutdown();

    BUILTIN_TRAP();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
