#include "config.h"

namespace NYT::NTransactionSupervisor {

////////////////////////////////////////////////////////////////////////////////

void TTransactionSupervisorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("participant_probation_period", &TThis::ParticipantProbationPeriod)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("rpc_timeout", &TThis::RpcTimeout)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("participant_backoff_time", &TThis::ParticipantBackoffTime)
        .Default(TDuration::Seconds(5));
    registrar.Parameter(
        "enable_wait_until_prepared_transactions_finished",
        &TThis::EnableWaitUntilPreparedTransactionsFinished)
        .Default(false);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionSupervisor
