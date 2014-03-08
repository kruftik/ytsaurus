#pragma once

#include "public.h"

#include <core/rpc/public.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/ypath/public.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

IAsyncWriterPtr CreateBufferedTableWriter(
    TBufferedTableWriterConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    NTransactionClient::TTransactionManagerPtr transactionManager,
    const NYPath::TYPath& path);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
