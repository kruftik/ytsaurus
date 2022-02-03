#include "config.h"

#include <yt/yt/core/concurrency/config.h>

#include <yt/yt/core/https/config.h>

namespace NYT::NAuth {

////////////////////////////////////////////////////////////////////////////////

TAuthCacheConfig::TAuthCacheConfig()
{
    RegisterParameter("cache_ttl", CacheTtl)
        .Default(TDuration::Minutes(5));
    RegisterParameter("optimistic_cache_ttl", OptimisticCacheTtl)
        .Default(TDuration::Hours(1));
    RegisterParameter("error_ttl", ErrorTtl)
        .Default(TDuration::Seconds(15));
}

////////////////////////////////////////////////////////////////////////////////

TDefaultBlackboxServiceConfig::TDefaultBlackboxServiceConfig()
{
    RegisterParameter("http_client", HttpClient)
        .DefaultNew();
    RegisterParameter("host", Host)
        .Default("blackbox.yandex-team.ru");
    RegisterParameter("port", Port)
        .Default(443);
    RegisterParameter("secure", Secure)
        .Default(true);
    RegisterParameter("blackbox_service_id", BlackboxServiceId)
        .Default("blackbox");
    RegisterParameter("request_timeout", RequestTimeout)
        .Default(TDuration::Seconds(15));
    RegisterParameter("attempt_timeout", AttemptTimeout)
        .Default(TDuration::Seconds(10));
    RegisterParameter("backoff_timeout", BackoffTimeout)
        .Default(TDuration::Seconds(1));
    RegisterParameter("use_lowercase_login", UseLowercaseLogin)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

TDefaultTvmServiceConfig::TDefaultTvmServiceConfig()
{
    RegisterParameter("use_tvm_tool", UseTvmTool)
        .Default(false);
    RegisterParameter("client_self_id", ClientSelfId)
        .Default(0);
    RegisterParameter("client_disk_cache_dir", ClientDiskCacheDir)
        .Optional();
    RegisterParameter("tvm_host", TvmHost)
        .Optional();
    RegisterParameter("tvm_port", TvmPort)
        .Optional();
    RegisterParameter("client_enable_user_ticket_checking", ClientEnableUserTicketChecking)
        .Default(false);
    RegisterParameter("client_blackbox_env", ClientBlackboxEnv)
        .Default("ProdYateam");
    RegisterParameter("client_enable_service_ticket_fetching", ClientEnableServiceTicketFetching)
        .Default(false);
    RegisterParameter("client_self_secret", ClientSelfSecret)
        .Optional();
    RegisterParameter("client_dst_map", ClientDstMap)
        .Optional();
    RegisterParameter("client_enable_service_ticket_checking", ClientEnableServiceTicketChecking)
        .Default(false);

    RegisterParameter("tvm_tool_self_alias", TvmToolSelfAlias)
        .Optional();
    RegisterParameter("tvm_tool_port", TvmToolPort)
        .Optional();
    RegisterParameter("tvm_tool_auth_token", TvmToolAuthToken)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

TBlackboxTokenAuthenticatorConfig::TBlackboxTokenAuthenticatorConfig()
{
    RegisterParameter("scope", Scope);
    RegisterParameter("enable_scope_check", EnableScopeCheck)
        .Default(true);
    RegisterParameter("get_user_ticket", GetUserTicket)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

TBlackboxTicketAuthenticatorConfig::TBlackboxTicketAuthenticatorConfig()
{
    RegisterParameter("scopes", Scopes)
        .Optional();
    RegisterParameter("enable_scope_check", EnableScopeCheck)
        .Default(false);
}

////////////////////////////////////////////////////////////////////////////////

TCachingTokenAuthenticatorConfig::TCachingTokenAuthenticatorConfig()
{
    RegisterParameter("cache", Cache)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

TCypressTokenAuthenticatorConfig::TCypressTokenAuthenticatorConfig()
{
    RegisterParameter("root_path", RootPath)
        .Default("//sys/tokens");
    RegisterParameter("realm", Realm)
        .Default("cypress");

    RegisterParameter("secure", Secure)
        .Default(false);
}

////////////////////////////////////////////////////////////////////////////////

TBlackboxCookieAuthenticatorConfig::TBlackboxCookieAuthenticatorConfig()
{
    RegisterParameter("domain", Domain)
        .Default("yt.yandex-team.ru");

    RegisterParameter("csrf_secret", CsrfSecret)
        .Default();
    RegisterParameter("csrf_token_ttl", CsrfTokenTtl)
        .Default(DefaultCsrfTokenTtl);

    RegisterParameter("get_user_ticket", GetUserTicket)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

TCachingCookieAuthenticatorConfig::TCachingCookieAuthenticatorConfig()
{
    RegisterParameter("cache", Cache)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

TDefaultSecretVaultServiceConfig::TDefaultSecretVaultServiceConfig()
{
    RegisterParameter("host", Host)
        .Default("vault-api.passport.yandex.net");
    RegisterParameter("port", Port)
        .Default(443);
    RegisterParameter("secure", Secure)
        .Default(true);
    RegisterParameter("http_client", HttpClient)
        .DefaultNew();
    RegisterParameter("request_timeout", RequestTimeout)
        .Default(TDuration::Seconds(3));
    RegisterParameter("vault_service_id", VaultServiceId)
        .Default("yav");
    RegisterParameter("consumer", Consumer)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

TBatchingSecretVaultServiceConfig::TBatchingSecretVaultServiceConfig()
{
    RegisterParameter("batch_delay", BatchDelay)
        .Default(TDuration::MilliSeconds(100));
    RegisterParameter("max_subrequests_per_request", MaxSubrequestsPerRequest)
        .Default(100)
        .GreaterThan(0);
    RegisterParameter("requests_throttler", RequestsThrottler)
        .DefaultNew();

    RegisterPreprocessor([&] {
        RequestsThrottler->Limit = 100;
    });
}

////////////////////////////////////////////////////////////////////////////////

TCachingSecretVaultServiceConfig::TCachingSecretVaultServiceConfig()
{
    RegisterParameter("cache", Cache)
        .DefaultNew();

    RegisterPreprocessor([&] {
        Cache->RefreshTime = std::nullopt;
        Cache->ExpireAfterAccessTime = TDuration::Seconds(60);
        Cache->ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(60);
        Cache->ExpireAfterFailedUpdateTime = TDuration::Seconds(60);
    });
}

////////////////////////////////////////////////////////////////////////////////

TString TAuthenticationManagerConfig::GetCsrfSecret() const
{
    if (BlackboxCookieAuthenticator &&
        BlackboxCookieAuthenticator->CsrfSecret)
    {
        return *BlackboxCookieAuthenticator->CsrfSecret;
    }

    return TString();
}

TInstant TAuthenticationManagerConfig::GetCsrfTokenExpirationTime() const
{
    if (BlackboxCookieAuthenticator) {
        return TInstant::Now() - BlackboxCookieAuthenticator->CsrfTokenTtl;
    }

    return TInstant::Now() - DefaultCsrfTokenTtl;
}

void TAuthenticationManagerConfig::Register(TRegistrar registrar)
{
    // COMPAT(prime@)
    registrar.Parameter("require_authentication", &TThis::RequireAuthentication)
        .Alias("enable_authentication")
        .Default(true);
    registrar.Parameter("blackbox_token_authenticator", &TThis::BlackboxTokenAuthenticator)
        .Alias("token_authenticator")
        .Optional();
    registrar.Parameter("blackbox_cookie_authenticator", &TThis::BlackboxCookieAuthenticator)
        .Alias("cookie_authenticator")
        .DefaultNew();
    registrar.Parameter("blackbox_service", &TThis::BlackboxService)
        .Alias("blackbox")
        .DefaultNew();
    registrar.Parameter("cypress_token_authenticator", &TThis::CypressTokenAuthenticator)
        .Optional();
    registrar.Parameter("tvm_service", &TThis::TvmService)
        .Optional();
    registrar.Parameter("blackbox_ticket_authenticator", &TThis::BlackboxTicketAuthenticator)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
