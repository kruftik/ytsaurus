#include "bootstrap.h"

#include "query_tracker.h"
#include "query_tracker_proxy.h"
#include "config.h"
#include "private.h"
#include "proxy_service.h"
#include "dynamic_config_manager.h"

#include <yt/yt/server/lib/admin/admin_service.h>

#include <yt/yt/library/coredumper/coredumper.h>

#include <yt/yt/server/lib/cypress_registrar/config.h>
#include <yt/yt/server/lib/cypress_registrar/cypress_registrar.h>

#include <yt/yt/ytlib/cell_master_client/cell_directory_synchronizer.h>

#include <yt/yt/ytlib/query_tracker_client/records/query.record.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/config.h>
#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/helpers.h>

#include <yt/yt/ytlib/hive/cluster_directory_synchronizer.h>
#include <yt/yt/ytlib/hive/cluster_directory.h>

#include <yt/yt/library/monitoring/http_integration.h>
#include <yt/yt/library/monitoring/monitoring_manager.h>

#include <yt/yt/ytlib/orchid/orchid_service.h>

#include <yt/yt/library/program/build_attributes.h>
#include <yt/yt/library/program/config.h>
#include <yt/yt/ytlib/program/helpers.h>

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/core/bus/server.h>

#include <yt/yt/core/bus/tcp/server.h>

#include <yt/yt/core/http/server.h>

#include <yt/yt/core/concurrency/action_queue.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/local_address.h>

#include <yt/yt/library/coredumper/coredumper.h>

#include <yt/yt/core/misc/ref_counted_tracker.h>

#include <yt/yt/core/rpc/bus/server.h>

#include <yt/yt/core/ypath/token.h>

#include <yt/yt/core/ytree/virtual.h>
#include <yt/yt/core/ytree/ypath_client.h>

namespace NYT::NQueryTracker {

using namespace NAdmin;
using namespace NAlertManager;
using namespace NBus;
using namespace NHydra;
using namespace NMonitoring;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NSecurityClient;
using namespace NYTree;
using namespace NYPath;
using namespace NConcurrency;
using namespace NApi;
using namespace NNodeTrackerClient;
using namespace NLogging;
using namespace NHiveClient;
using namespace NYson;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

static constexpr auto& Logger = QueryTrackerLogger;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(TQueryTrackerServerConfigPtr config, INodePtr configNode)
    : Config_(std::move(config))
    , ConfigNode_(std::move(configNode))
{
    if (Config_->AbortOnUnrecognizedOptions) {
        AbortOnUnrecognizedOptions(Logger(), Config_);
    } else {
        WarnForUnrecognizedOptions(Logger(), Config_);
    }
}

TBootstrap::~TBootstrap() = default;

void TBootstrap::Run()
{
    ControlQueue_ = New<TActionQueue>("Control");
    ControlInvoker_ = ControlQueue_->GetInvoker();

    ProxyPool_ = CreateThreadPool(Config_->ProxyThreadPoolSize, "Proxy");
    ProxyInvoker_ = ProxyPool_->GetInvoker();

    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(ControlInvoker_)
        .Run()
        .Get()
        .ThrowOnError();

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    YT_LOG_INFO(
        "Starting persistent query agent process (NativeCluster: %v, User: %v)",
        Config_->ClusterConnection->Static->ClusterName,
        Config_->User);

    SelfAddress_ = NNet::BuildServiceAddress(NNet::GetLocalHostName(), Config_->RpcPort);

    NApi::NNative::TConnectionOptions connectionOptions;
    connectionOptions.RetryRequestQueueSizeLimitExceeded = true;
    NativeConnection_ = NApi::NNative::CreateConnection(
        Config_->ClusterConnection,
        std::move(connectionOptions));

    NativeConnection_->GetClusterDirectorySynchronizer()->Start();
    NativeConnection_->GetMasterCellDirectorySynchronizer()->Start();

    SetupClusterConnectionDynamicConfigUpdate(
        NativeConnection_,
        NApi::NNative::EClusterConnectionDynamicConfigPolicy::FromClusterDirectory,
        /*staticClusterConnectionNode*/ nullptr,
        Logger());

    NativeAuthenticator_ = NNative::CreateNativeAuthenticator(NativeConnection_);

    auto clientOptions = TClientOptions::FromUser(Config_->User);
    NativeClient_ = NativeConnection_->CreateNativeClient(clientOptions);

    DynamicConfigManager_ = New<TDynamicConfigManager>(Config_, NativeClient_, ControlInvoker_);
    DynamicConfigManager_->SubscribeConfigChanged(BIND_NO_PROPAGATE(&TBootstrap::OnDynamicConfigChanged, Unretained(this)));

    BusServer_ = CreateBusServer(Config_->BusServer);

    RpcServer_ = NRpc::NBus::CreateBusServer(BusServer_);

    HttpServer_ = NHttp::CreateServer(Config_->CreateMonitoringHttpServerConfig());

    AlertManager_ = CreateAlertManager(QueryTrackerLogger(), TProfiler{}, ControlInvoker_);

    if (Config_->CoreDumper) {
        CoreDumper_ = NCoreDump::CreateCoreDumper(Config_->CoreDumper);
    }

    DynamicConfigManager_->Start();

    WaitFor(DynamicConfigManager_->GetConfigLoadedFuture())
        .ThrowOnError();

    NYTree::IMapNodePtr orchidRoot;
    NMonitoring::Initialize(
        HttpServer_,
        Config_->SolomonExporter,
        &MonitoringManager_,
        &orchidRoot);

    SetNodeByYPath(
        orchidRoot,
        "/alerts",
        CreateVirtualNode(AlertManager_->GetOrchidService()));
    if (Config_->ExposeConfigInOrchid) {
        SetNodeByYPath(
            orchidRoot,
            "/config",
            CreateVirtualNode(ConfigNode_));
        SetNodeByYPath(
            orchidRoot,
            "/dynamic_config_manager",
            CreateVirtualNode(DynamicConfigManager_->GetOrchidService()));
    }
    if (CoreDumper_) {
        SetNodeByYPath(
            orchidRoot,
            "/core_dumper",
            CreateVirtualNode(CoreDumper_->CreateOrchidService()));
    }
    SetBuildAttributes(
        orchidRoot,
        "query_tracker");

    QueryTrackerProxy_ = CreateQueryTrackerProxy(
        NativeClient_,
        Config_->Root,
        DynamicConfigManager_->GetConfig()->QueryTracker->ProxyConfig);

    RpcServer_->RegisterService(CreateAdminService(
        ControlInvoker_,
        CoreDumper_,
        NativeAuthenticator_));
    RpcServer_->RegisterService(CreateOrchidService(
        orchidRoot,
        ControlInvoker_,
        NativeAuthenticator_));
    RpcServer_->RegisterService(CreateProxyService(
        ProxyInvoker_,
        QueryTrackerProxy_));

    QueryTracker_ = CreateQueryTracker(
        DynamicConfigManager_->GetConfig()->QueryTracker,
        SelfAddress_,
        ControlInvoker_,
        CreateAlertCollector(AlertManager_),
        NativeClient_,
        Config_->Root,
        Config_->MinRequiredStateVersion);

    AlertManager_->Start();

    QueryTracker_->Start();

    YT_LOG_INFO("Listening for HTTP requests (Port: %v)", Config_->MonitoringPort);
    HttpServer_->Start();

    YT_LOG_INFO("Listening for RPC requests (Port: %v)", Config_->RpcPort);
    RpcServer_->Configure(Config_->RpcServer);
    RpcServer_->Start();

    UpdateCypressNode();
}

void TBootstrap::UpdateCypressNode()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    TCypressRegistrarOptions options{
        .RootPath = Format("%v/instances/%v", Config_->Root, ToYPathLiteral(SelfAddress_)),
        .OrchidRemoteAddresses = TAddressMap{{NNodeTrackerClient::DefaultNetworkName, SelfAddress_}},
        .AttributesOnStart = BuildAttributeDictionaryFluently()
            .Item("annotations").Value(Config_->CypressAnnotations)
            .Finish(),
    };

    auto registrar = CreateCypressRegistrar(
        std::move(options),
        New<TCypressRegistrarConfig>(),
        NativeClient_,
        GetCurrentInvoker());

    while (true) {
        auto error = WaitFor(registrar->CreateNodes());

        if (error.IsOK()) {
            break;
        } else {
            YT_LOG_DEBUG(error, "Error updating Cypress node");
        }
    }
}

void TBootstrap::OnDynamicConfigChanged(
    const TQueryTrackerServerDynamicConfigPtr& oldConfig,
    const TQueryTrackerServerDynamicConfigPtr& newConfig)
{
    ReconfigureNativeSingletons(Config_, newConfig);

    if (AlertManager_) {
        AlertManager_->Reconfigure(oldConfig->AlertManager, newConfig->AlertManager);
    }
    if (QueryTracker_) {
        QueryTracker_->Reconfigure(newConfig->QueryTracker);
    }
    if (QueryTrackerProxy_) {
        QueryTrackerProxy_->Reconfigure(newConfig->QueryTracker->ProxyConfig);
    }

    YT_LOG_DEBUG(
        "Updated query tracker server dynamic config (OldConfig: %v, NewConfig: %v)",
        ConvertToYsonString(oldConfig, EYsonFormat::Text),
        ConvertToYsonString(newConfig, EYsonFormat::Text));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryTracker
