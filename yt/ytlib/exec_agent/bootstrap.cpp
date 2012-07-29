#include "stdafx.h"
#include "bootstrap.h"
#include "config.h"
#include "private.h"
#include "job.h"
#include "job_manager.h"
#include "supervisor_service.h"
#include "environment.h"
#include "environment_manager.h"
#include "unsafe_environment.h"
#include "scheduler_connector.h"
#include "slot.h"

#include <ytlib/cell_node/bootstrap.h>
#include <ytlib/cell_node/config.h>
#include <ytlib/job_proxy/config.h>
#include <ytlib/chunk_holder/bootstrap.h>
#include <ytlib/chunk_holder/config.h>
#include <ytlib/chunk_holder/chunk_cache.h>
#include <ytlib/rpc/server.h>

namespace NYT {
namespace NExecAgent {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(
    TExecAgentConfigPtr config,
    NCellNode::TBootstrap* nodeBootstrap)
    : Config(config)
    , NodeBootstrap(nodeBootstrap)
{
    YASSERT(config);
    YASSERT(nodeBootstrap);
}

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Init()
{
    JobProxyConfig = New<NJobProxy::TJobProxyConfig>();
    JobProxyConfig->SupervisorRpcTimeout = Config->SupervisorRpcTimeout;
    JobProxyConfig->Logging = Config->JobProxyLogging;
    JobProxyConfig->SandboxName = SandboxName;
    JobProxyConfig->Masters = NodeBootstrap->GetConfig()->Masters;
    JobProxyConfig->SupervisorConnection = New<NBus::TTcpBusClientConfig>();
    JobProxyConfig->SupervisorConnection->Address = NodeBootstrap->GetPeerAddress();
    // TODO(babenko): consider making this priority configurable
    JobProxyConfig->SupervisorConnection->Priority = 6;

    JobManager = New<TJobManager>(Config->JobManager, this);
    JobManager->Initialize();

    auto supervisorService = New<TSupervisorService>(this);
    NodeBootstrap->GetRpcServer()->RegisterService(supervisorService);

    EnvironmentManager = New<TEnvironmentManager>(Config->EnvironmentManager);
    EnvironmentManager->Register("unsafe", CreateUnsafeEnvironmentBuilder());

    SchedulerConnector = New<TSchedulerConnector>(Config->SchedulerConnector, this);
    SchedulerConnector->Start();
}

TExecAgentConfigPtr TBootstrap::GetConfig() const
{
    return Config;
}

IInvokerPtr TBootstrap::GetControlInvoker() const
{
    return NodeBootstrap->GetControlInvoker();
}

IChannelPtr TBootstrap::GetMasterChannel() const
{
    return NodeBootstrap->GetMasterChannel();
}

IChannelPtr TBootstrap::GetSchedulerChannel() const
{
    return NodeBootstrap->GetSchedulerChannel();
}

Stroka TBootstrap::GetPeerAddress() const
{
    return NodeBootstrap->GetPeerAddress();
}

TJobManagerPtr TBootstrap::GetJobManager() const
{
    return JobManager;
}

TEnvironmentManagerPtr TBootstrap::GetEnvironmentManager() const
{
    return EnvironmentManager;
}

NChunkHolder::TChunkCachePtr TBootstrap::GetChunkCache() const
{
    return NodeBootstrap->GetChunkHolderBootstrap()->GetChunkCache();
}

NJobProxy::TJobProxyConfigPtr TBootstrap::GetJobProxyConfig() const
{
    return JobProxyConfig;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
