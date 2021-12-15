#include "cache_config.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TSlruCacheConfig::TSlruCacheConfig(i64 capacity)
{
    RegisterParameter("capacity", Capacity)
        .Default(capacity)
        .GreaterThanOrEqual(0);
    RegisterParameter("younger_size_fraction", YoungerSizeFraction)
        .Default(0.25)
        .InRange(0.0, 1.0);
    RegisterParameter("shard_count", ShardCount)
        .Default(16)
        .GreaterThan(0);
    RegisterParameter("touch_buffer_capacity", TouchBufferCapacity)
        .Default(65536)
        .GreaterThan(0);

    RegisterPostprocessor([&] () {
        if (!IsPowerOf2(ShardCount)) {
            THROW_ERROR_EXCEPTION("\"shard_count\" must be power of two, actual: %v",
                ShardCount);
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

TSlruCacheDynamicConfig::TSlruCacheDynamicConfig()
{
    RegisterParameter("capacity", Capacity)
        .Optional()
        .GreaterThanOrEqual(0);
    RegisterParameter("younger_size_fraction", YoungerSizeFraction)
        .Optional()
        .InRange(0.0, 1.0);
}

////////////////////////////////////////////////////////////////////////////////

TAsyncExpiringCacheConfig::TAsyncExpiringCacheConfig()
{
    RegisterParameter("expire_after_access_time", ExpireAfterAccessTime)
        .Default(TDuration::Seconds(300));
    RegisterParameter("expire_after_successful_update_time", ExpireAfterSuccessfulUpdateTime)
        .Alias("success_expiration_time")
        .Default(TDuration::Seconds(15));
    RegisterParameter("expire_after_failed_update_time", ExpireAfterFailedUpdateTime)
        .Alias("failure_expiration_time")
        .Default(TDuration::Seconds(15));
    RegisterParameter("refresh_time", RefreshTime)
        .Alias("success_probation_time")
        .Default(TDuration::Seconds(10));
    RegisterParameter("batch_update", BatchUpdate)
        .Default(false);

    RegisterPostprocessor([&] () {
        if (RefreshTime && *RefreshTime && *RefreshTime > ExpireAfterSuccessfulUpdateTime) {
            THROW_ERROR_EXCEPTION("\"refresh_time\" must be less than \"expire_after_successful_update_time\"")
                << TErrorAttribute("refresh_time", RefreshTime)
                << TErrorAttribute("expire_after_successful_update_time", ExpireAfterSuccessfulUpdateTime);
        }
    });
}

void TAsyncExpiringCacheConfig::ApplyDynamicInplace(
    const TAsyncExpiringCacheDynamicConfigPtr& dynamicConfig)
{
    ExpireAfterAccessTime = dynamicConfig->ExpireAfterAccessTime.value_or(ExpireAfterAccessTime);
    ExpireAfterSuccessfulUpdateTime = dynamicConfig->ExpireAfterSuccessfulUpdateTime.value_or(ExpireAfterSuccessfulUpdateTime);
    ExpireAfterFailedUpdateTime = dynamicConfig->ExpireAfterFailedUpdateTime.value_or(ExpireAfterFailedUpdateTime);
    RefreshTime = dynamicConfig->RefreshTime.has_value()
        ? dynamicConfig->RefreshTime
        : RefreshTime;
    BatchUpdate = dynamicConfig->BatchUpdate.value_or(BatchUpdate);
}

TAsyncExpiringCacheConfigPtr TAsyncExpiringCacheConfig::ApplyDynamic(
    const TAsyncExpiringCacheDynamicConfigPtr& dynamicConfig) const
{
    auto config = New<TAsyncExpiringCacheConfig>();

    config->ApplyDynamicInplace(dynamicConfig);

    config->Postprocess();
    return config;
}

////////////////////////////////////////////////////////////////////////////////

TAsyncExpiringCacheDynamicConfig::TAsyncExpiringCacheDynamicConfig()
{
    RegisterParameter("expire_after_access_time", ExpireAfterAccessTime)
        .Optional();
    RegisterParameter("expire_after_successful_update_time", ExpireAfterSuccessfulUpdateTime)
        .Optional();
    RegisterParameter("expire_after_failed_update_time", ExpireAfterFailedUpdateTime)
        .Optional();
    RegisterParameter("refresh_time", RefreshTime)
        .Optional();
    RegisterParameter("batch_update", BatchUpdate)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
