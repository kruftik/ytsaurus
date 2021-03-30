#include "exporter.h"
#include "private.h"

#include <yt/yt/build/build.h>

#include <yt/yt/core/http/http.h>
#include <yt/yt/core/http/server.h>
#include <yt/yt/core/http/helpers.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/scheduler_api.h>

#include <library/cpp/monlib/encode/json/json.h>
#include <library/cpp/monlib/encode/spack/spack_v1.h>

#include <library/cpp/cgiparam/cgiparam.h>

#include <util/datetime/base.h>
#include <util/stream/str.h>

namespace NYT::NProfiling {

using namespace NConcurrency;
using namespace NHttp;
using namespace NYTree;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = SolomonLogger;

////////////////////////////////////////////////////////////////////////////////

TShardConfigPtr TSolomonExporterConfig::MatchShard(const TString& sensorName)
{
    TShardConfigPtr matchedShard;
    int matchSize = 0;

    for (const auto& [name, config] : Shards) {
        for (auto prefix : config->Filter) {
            if (!sensorName.StartsWith(prefix)) {
                continue;
            }

            if (prefix.size() > static_cast<size_t>(matchSize)) {
                matchSize = prefix.size();
                matchedShard = config;
            }
        }
    }

    return matchedShard;
}

////////////////////////////////////////////////////////////////////////////////

TSolomonExporter::TSolomonExporter(
    const TSolomonExporterConfigPtr& config,
    const IInvokerPtr& invoker,
    const TSolomonRegistryPtr& registry)
    : Config_(config)
    , Invoker_(CreateSerializedInvoker(invoker))
    , Registry_(registry ? registry : TSolomonRegistry::Get())
    , CoreProfilingPusher_(New<TPeriodicExecutor>(
        Invoker_,
        BIND(&TSolomonExporter::DoPushCoreProfiling, MakeWeak(this)),
        TDuration::MilliSeconds(500)))
    , Root_(New<TSensorService>(Registry_, Invoker_))
{
    if (Config_->EnableSelfProfiling) {
        Registry_->Profile(TProfiler{Registry_, ""});
    }

    CollectionStartDelay_ = Registry_->GetSelfProfiler().Timer("/collection_delay");
    WindowErrors_ = Registry_->GetSelfProfiler().Counter("/window_error_count");
    ReadDelays_ = Registry_->GetSelfProfiler().Counter("/read_delay_count");
    ResponseCacheMiss_ = Registry_->GetSelfProfiler().Counter("/response_cache_miss");
    ResponseCacheHit_ = Registry_->GetSelfProfiler().Counter("/response_cache_hit");

    for (const auto& [name, config] : Config_->Shards) {
        LastShardFetch_[name] = std::nullopt;
    }

    Registry_->SetWindowSize(Config_->WindowSize);
    Registry_->SetGridFactor([config] (const TString& name) -> int {
        auto shard = config->MatchShard(name);
        if (!shard) {
            return 1;
        }

        if (!shard->GridStep) {
            return 1;
        }

        return shard->GridStep->GetValue() / config->GridStep.GetValue();
    });

    if (config->ReportBuildInfo) {
        TProfiler profiler{registry, ""};

        profiler
            .WithRequiredTag("version", GetVersion())
            .AddFuncGauge("/build/version", MakeStrong(this), [] { return 1.0; });
    }
    if (config->ReportRestart) {
        TProfiler profiler{registry, ""};

        profiler.AddFuncGauge(
            "/server/restarted_5min_ago",
            MakeStrong(this),
            [this] {
                return (TInstant::Now() - StartTime_ < TDuration::Minutes(5)) ? 1.0 : 0.0;
            });
    }
}

void TSolomonExporter::Register(const TString& prefix, const NHttp::IServerPtr& server)
{
    auto via = [this] (auto cb) {
        return BIND([this, cb] (const IRequestPtr& req, const IResponseWriterPtr& rsp) {
            auto result = BIND([cb, req, rsp] () {
                cb(req, rsp);
            })
                .AsyncVia(Invoker_)
                .Run();

            WaitFor(result).ThrowOnError();
        });
    };

    server->AddHandler(prefix + "/", via(BIND(&TSolomonExporter::HandleIndex, MakeStrong(this), prefix)));
    server->AddHandler(prefix + "/sensors", via(BIND(&TSolomonExporter::HandleDebugSensors, MakeStrong(this))));
    server->AddHandler(prefix + "/tags", via(BIND(&TSolomonExporter::HandleDebugTags, MakeStrong(this))));
    server->AddHandler(prefix + "/status", via(BIND(&TSolomonExporter::HandleStatus, MakeStrong(this))));
    server->AddHandler(prefix + "/all", via(BIND(&TSolomonExporter::HandleShard, MakeStrong(this), std::nullopt)));

    for (const auto& [shardName, shard] : Config_->Shards) {
        server->AddHandler(
            prefix + "/shard/" + shardName,
            via(BIND(&TSolomonExporter::HandleShard, MakeStrong(this), shardName)));
    }
}

void TSolomonExporter::Start()
{
    if (Config_->ThreadPoolSize) {
        ThreadPool_ = New<TThreadPool>(*Config_->ThreadPoolSize, "SolomonExporter");
    }

    Collector_ = BIND([this, thiz=MakeStrong(this)] {
        try {
            DoCollect();
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Sensor collector crashed");
        }
    })
        .AsyncVia(Invoker_)
        .Run();

    if (Config_->EnableCoreProfilingCompatibility) {
        CoreProfilingPusher_->Start();
    }
}

void TSolomonExporter::Stop()
{
    Collector_.Cancel(TError("Stopped"));

    if (Config_->EnableCoreProfilingCompatibility) {
        CoreProfilingPusher_->Stop();
    }

    if (ThreadPool_) {
        ThreadPool_->Shutdown();
    }
}

void TSolomonExporter::DoCollect()
{
    auto nowUnix = TInstant::Now().GetValue();
    nowUnix -= (nowUnix % Config_->GridStep.GetValue());
    auto nextGridTime = TInstant::FromValue(nowUnix) + Config_->GridStep;

    auto waitUntil = [] (auto deadline) {
        auto now = TInstant::Now();
        if (now >= deadline) {
            return;
        }
        TDelayedExecutor::WaitForDuration(deadline - now);
    };

    // Compute start time. Zero iteration time should be aligned with each GridStep.
    while (true) {
        bool aligned = true;
        for (const auto& [name, shard] : Config_->Shards) {
            if (shard->GridStep && nextGridTime.GetValue() % shard->GridStep->GetValue() != 0) {
                aligned = false;
            }
        }

        if (aligned) {
            break;
        }

        nextGridTime += Config_->GridStep;
    }

    YT_LOG_DEBUG("Sensor collector started (StartTime: %v)", nextGridTime);
    while (true) {
        CleanResponseCache();

        waitUntil(nextGridTime);

        auto delay = TInstant::Now() - nextGridTime;
        CollectionStartDelay_.Record(delay);

        YT_LOG_DEBUG("Started sensor collection (Delay: %v)", delay);
        Registry_->ProcessRegistrations();

        auto iteration = Registry_->GetNextIteration();
        {
            TForbidContextSwitchGuard guard;
            Registry_->Collect(ThreadPool_ ? ThreadPool_->GetInvoker() : GetSyncInvoker());
        }

        Window_.emplace_back(iteration, nextGridTime);
        if (Window_.size() > static_cast<size_t>(Registry_->GetWindowSize())) {
            Window_.erase(Window_.begin());
        }

        YT_LOG_DEBUG("Finished sensor collection (Delay: %v)", TInstant::Now() - nextGridTime);
        nextGridTime += Config_->GridStep;
    }
}

constexpr auto IndexPage = R"EOF(
<!DOCTYPE html>
<html>
<body>
<a href="sensors">sensors top</a>
<br/>
<a href="tags">tags top</a>
<br/>
<a href="status">status</a>
</body>
</html>
)EOF";

void TSolomonExporter::HandleIndex(const TString& prefix, const IRequestPtr& req, const IResponseWriterPtr& rsp)
{
    if (req->GetUrl().Path != prefix && req->GetUrl().Path != prefix + "/") {
        rsp->SetStatus(EStatusCode::NotFound);
        WaitFor(rsp->WriteBody(TSharedRef::FromString("Not found")))
            .ThrowOnError();
        return;
    }

    rsp->SetStatus(EStatusCode::OK);
    rsp->GetHeaders()->Add("Content-Type", "text/html; charset=UTF-8");

    WaitFor(rsp->WriteBody(TSharedRef::FromString(IndexPage)))
        .ThrowOnError();
}

void TSolomonExporter::HandleStatus(const IRequestPtr&, const IResponseWriterPtr& rsp)
{
    auto sensors = Registry_->ListSensors();
    THashMap<TString, TError> invalidSensors;
    for (const auto& sensor : sensors) {
        if (sensor.Error.IsOK()) {
            continue;
        }

        invalidSensors[sensor.Name] = sensor.Error;
    }

    rsp->SetStatus(EStatusCode::OK);
    ReplyJson(rsp, [&] (auto consumer) {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("start_time").Value(StartTime_)
                .Item("last_fetch").Value(LastFetch_)
                .Item("last_shard_fetch").DoMapFor(LastShardFetch_, [] (auto fluent, auto time) {
                    fluent
                        .Item(time.first).Value(time.second);
                })
                .Item("window").Value(Window_)
                .Item("sensor_errors").DoMapFor(invalidSensors, [] (auto fluent, auto sensor) {
                    fluent
                        .Item(sensor.first).Value(sensor.second);
                })
                .Item("dynamic_tags").Value(Registry_->GetDynamicTags())
            .EndMap();
    });
}

void TSolomonExporter::HandleDebugSensors(const IRequestPtr&, const IResponseWriterPtr& rsp)
{
    rsp->SetStatus(EStatusCode::OK);
    rsp->GetHeaders()->Add("Content-Type", "text/plain; charset=UTF-8");

    auto sensors = Registry_->ListSensors();
    std::sort(sensors.begin(), sensors.end(), [] (const auto& a, const auto& b) {
        return std::tie(a.CubeSize, a.Name) > std::tie(b.CubeSize, b.Name);
    });

    TStringStream out;
    out << "# cube_size object_count name error" << Endl;
    for (const auto& sensor : sensors) {
        out << sensor.CubeSize << " " << sensor.ObjectCount << " " << sensor.Name;

        if (!sensor.Error.IsOK()) {
            out << " " << ToString(sensor.Error);
        }

        out << Endl;
    }

    WaitFor(rsp->WriteBody(TSharedRef::FromString(out.Str())))
        .ThrowOnError();
}

void TSolomonExporter::HandleDebugTags(const IRequestPtr&, const IResponseWriterPtr& rsp)
{
    rsp->SetStatus(EStatusCode::OK);
    rsp->GetHeaders()->Add("Content-Type", "text/plain; charset=UTF-8");

    auto tags = Registry_->GetTags().TopByKey();
    std::vector<std::pair<TString, size_t>> tagList{tags.begin(), tags.end()};
    std::sort(tagList.begin(), tagList.end(), [] (auto a, auto b) {
        return std::tie(a.second, a.first) > std::tie(b.second, b.first);
    });

    TStringStream out;
    out << "# value_count tag_name" << Endl;
    for (const auto& tag : tagList) {
        out << tag.second << " " << tag.first << Endl;
    }

    WaitFor(rsp->WriteBody(TSharedRef::FromString(out.Str())))
        .ThrowOnError();
}

std::optional<TString> TSolomonExporter::ReadJson(const TReadOptions& options)
{
    auto result = BIND([this, options, this_ = MakeStrong(this)] () -> std::optional<TString> {
        TStringStream buffer;
        auto encoder = NMonitoring::BufferedEncoderJson(&buffer);

        if (Window_.empty()) {
            return {};
        }

        // Read last value.
        auto readOptions = options;
        readOptions.Times.emplace_back(std::vector<int>{Registry_->IndexOf(Window_.back().first)}, TInstant::Zero());
        readOptions.ConvertCountersToRateGauge = false;
        readOptions.ExportSummary |= this_->Config_->ExportSummary;
        readOptions.ExportSummaryAsMax |= this_->Config_->ExportSummaryAsMax;
        readOptions.ExportSummaryAsAvg |= this_->Config_->ExportSummaryAsAvg;
        readOptions.MarkAggregates |= this_->Config_->MarkAggregates;
        if (!readOptions.Host && this_->Config_->Host) {
            readOptions.Host = this_->Config_->Host;
        }
        if (readOptions.InstanceTags.empty() && !this_->Config_->InstanceTags.empty()) {
            readOptions.InstanceTags.reserve(this_->Config_->InstanceTags.size());
            for (auto&& [k, v] : this_->Config_->InstanceTags) {
                readOptions.InstanceTags.emplace_back(k, v);
            }
        }
        readOptions.SensorFilter = [this] (const TString& sensorName) {
            return FilterDefaultGrid(sensorName);
        };

        encoder->OnStreamBegin();
        Registry_->ReadSensors(readOptions, encoder.Get());
        encoder->OnStreamEnd();
        encoder->Close();

        return buffer.Str();
    })
        .AsyncVia(Invoker_)
        .Run();

    return WaitFor(result).ValueOrThrow();
}

void TSolomonExporter::HandleShard(
    const std::optional<TString>& name,
    const IRequestPtr& req,
    const IResponseWriterPtr& rsp)
{
    try {
        NMonitoring::EFormat format = NMonitoring::EFormat::JSON;
        if (auto accept = req->GetHeaders()->Find("Accept")) {
            format = NMonitoring::FormatFromAcceptHeader(*accept);
        }

        NMonitoring::ECompression compression = NMonitoring::ECompression::IDENTITY;
        if (auto acceptEncoding = req->GetHeaders()->Find("Accept-Encoding")) {
            compression = NMonitoring::CompressionFromAcceptEncodingHeader(*acceptEncoding);
        }

        TStringStream buffer;

        NMonitoring::IMetricEncoderPtr encoder;
        switch (format) {
            case NMonitoring::EFormat::UNKNOWN:
            case NMonitoring::EFormat::JSON:
                encoder = NMonitoring::BufferedEncoderJson(&buffer);
                format = NMonitoring::EFormat::JSON;
                compression = NMonitoring::ECompression::IDENTITY;
                break;

            case NMonitoring::EFormat::SPACK:
                encoder = NMonitoring::EncoderSpackV1(
                    &buffer,
                    NMonitoring::ETimePrecision::SECONDS,
                    compression,
                    NMonitoring::EMetricsMergingMode::MERGE_METRICS);
                break;

            default:
                THROW_ERROR_EXCEPTION("Unsupported format %Qv", NMonitoring::ContentTypeByFormat(format));
        }

        rsp->GetHeaders()->Set("Content-Type", TString{NMonitoring::ContentTypeByFormat(format)});
        rsp->GetHeaders()->Set("Content-Encoding", TString{NMonitoring::ContentEncodingByCompression(compression)});

        TCgiParameters params(req->GetUrl().RawQuery);

        std::optional<TDuration> period;
        if (auto it = params.Find("period"); it != params.end()) {
            period = TDuration::Parse(it->second);
        }

        std::optional<TInstant> now;
        if (auto it = params.Find("now"); it != params.end()) {
            now = TInstant::ParseIso8601(it->second);
        }

        std::optional<TDuration> readGridStep;
        if (auto gridHeader = req->GetHeaders()->Find("X-Solomon-GridSec"); gridHeader) {
            int gridSeconds;
            if (!TryFromString<int>(*gridHeader, gridSeconds)) {
                THROW_ERROR_EXCEPTION("Invalid value of \"X-Solomon-GridSec\" header")
                    << TErrorAttribute("value", *gridHeader);
            }
            readGridStep = TDuration::Seconds(gridSeconds);
        }

        if ((now && !period) || (period && !now)) {
            THROW_ERROR_EXCEPTION("Both \"period\" and \"now\" must be present in request")
                << TErrorAttribute("now", now)
                << TErrorAttribute("period", period);
        }

        auto gridStep = Config_->GridStep;
        if (name) {
            auto shardConfig = Config_->Shards[*name];
            if (shardConfig->GridStep) {
                gridStep = *shardConfig->GridStep;
            }
        }

        ValidatePeriodAndGrid(period, readGridStep, gridStep);

        if (Window_.empty()) {
            WindowErrors_.Increment();
            THROW_ERROR_EXCEPTION("Window is empty");
        }

        std::optional<TCacheKey> cacheKey;
        if (now && period) {
            cacheKey = TCacheKey{
                .Shard = name,
                .Format = format,
                .Compression = compression,
                .Now = *now,
                .Period = *period,
                .Grid = readGridStep,
            };
        }

        auto solomonCluster = req->GetHeaders()->Find("X-Solomon-ClusterId");
        YT_LOG_DEBUG("Processing sensor pull (Format: %v, Compression: %v, SolomonCluster: %v, Now: %v, Period: %v, Grid: %v)",
            format,
            compression,
            solomonCluster ? *solomonCluster : "",
            now,
            period,
            readGridStep);

        if (cacheKey && TryReplyFromCache(*cacheKey, rsp)) {
            return;
        }

        TReadWindow readWindow;
        if (period) {
            if (auto errorOrWindow = SelectReadWindow(*now, *period, readGridStep, gridStep); !errorOrWindow.IsOK()) {
                ReadDelays_.Increment();
                YT_LOG_WARNING(errorOrWindow, "Delaying sensor read (Delay: %v)", Config_->ReadDelay);
                TDelayedExecutor::WaitForDuration(Config_->ReadDelay);

                if (cacheKey && TryReplyFromCache(*cacheKey, rsp)) {
                    return;
                }

                if (auto errorOrWindow = SelectReadWindow(*now, *period, readGridStep, gridStep); !errorOrWindow.IsOK()) {
                    WindowErrors_.Increment();
                    THROW_ERROR errorOrWindow;
                } else {
                    readWindow = errorOrWindow.Value();
                }
            } else {
                readWindow = errorOrWindow.Value();
            }
        } else {
            YT_LOG_WARNING("Timestamp query arguments are missing; returning last value");

            int gridFactor = gridStep / Config_->GridStep;
            for (auto i = static_cast<int>(Window_.size()) - 1; i >= 0; --i) {
                auto [iteration, time] = Window_[i];
                if (iteration % gridFactor == 0) {
                    readWindow.emplace_back(std::vector<int>{Registry_->IndexOf(iteration / gridFactor)}, time);
                    break;
                }
            }

            if (readWindow.empty()) {
                THROW_ERROR_EXCEPTION("Can't find latest timestamp")
                    << TErrorAttribute("first_iteration", Window_[0].first)
                    << TErrorAttribute("grid_factor", gridFactor)
                    << TErrorAttribute("window_size", Window_.size());
            }
        }

        if (cacheKey) {
            ResponseCacheMiss_.Increment();
        }

        TReadOptions options;
        options.Host = Config_->Host;
        options.InstanceTags = std::vector<TTag>{Config_->InstanceTags.begin(), Config_->InstanceTags.end()};

        if (Config_->ConvertCountersToRate) {
            options.ConvertCountersToRateGauge = true;

            options.RateDenominator = gridStep.SecondsFloat();
            if (readGridStep) {
                options.RateDenominator = readGridStep->SecondsFloat();
            }
        }

        options.EnableSolomonAggregationWorkaround = true;
        options.Times = readWindow;
        options.ExportSummary = Config_->ExportSummary;
        options.ExportSummaryAsMax = Config_->ExportSummaryAsMax;
        options.ExportSummaryAsAvg = Config_->ExportSummaryAsAvg;
        options.MarkAggregates = Config_->MarkAggregates;
        options.LingerWindowSize = Config_->LingerTimeout / gridStep;

        if (name) {
            options.SensorFilter = [&] (const TString& sensorName) {
                return Config_->MatchShard(sensorName) == Config_->Shards[*name];
            };
        } else {
            options.SensorFilter = [this] (const TString& sensorName) {
                return FilterDefaultGrid(sensorName);
            };
        }

        encoder->OnStreamBegin();
        Registry_->ReadSensors(options, encoder.Get());
        encoder->OnStreamEnd();
        encoder->Close();

        if (name) {
            LastShardFetch_[*name] = TInstant::Now();
        } else {
            LastFetch_ = TInstant::Now();
        }

        rsp->SetStatus(EStatusCode::OK);

        auto replyBlob = TSharedRef::FromString(buffer.Str());
        if (cacheKey) {
            ResponseCache_[*cacheKey] = replyBlob;
        }

        WaitFor(rsp->WriteBody(replyBlob))
            .ThrowOnError();
    } catch(const std::exception& ex) {
        YT_LOG_ERROR(ex, "Failed to export sensors");

        if (!rsp->IsHeadersFlushed()) {
            try {
                rsp->SetStatus(EStatusCode::InternalServerError);
                rsp->GetHeaders()->Remove("Content-Type");
                rsp->GetHeaders()->Remove("Content-Encoding");

                // Send only message. It should be displayed nicely in Solomon UI.
                WaitFor(rsp->WriteBody(TSharedRef::FromString(TError(ex).GetMessage())))
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                YT_LOG_ERROR(ex, "Failed to send export error");
            }
        }
    }
}

void TSolomonExporter::ValidatePeriodAndGrid(std::optional<TDuration> period, std::optional<TDuration> readGridStep, TDuration gridStep)
{
    if (!period) {
        return;
    }

    if (*period < gridStep) {
        THROW_ERROR_EXCEPTION("Period cannot be lower than grid step")
            << TErrorAttribute("period", *period)
            << TErrorAttribute("grid_step", gridStep);
    }

    if (period->GetValue() % gridStep.GetValue() != 0) {
        THROW_ERROR_EXCEPTION("Period must be multiple of grid step")
            << TErrorAttribute("period", *period)
            << TErrorAttribute("grid_step", gridStep);
    }

    if (readGridStep) {
        if (*readGridStep < gridStep) {
            THROW_ERROR_EXCEPTION("Server grid step cannot be lower than client grid step")
                << TErrorAttribute("server_grid_step", *readGridStep)
                << TErrorAttribute("grid_step", gridStep);
        }

        if (readGridStep->GetValue() % gridStep.GetValue() != 0) {
            THROW_ERROR_EXCEPTION("Server grid step must be multiple of client grid step")
                << TErrorAttribute("server_grid_step", *readGridStep)
                << TErrorAttribute("grid_step", gridStep);
        }

        if (*readGridStep > *period) {
            THROW_ERROR_EXCEPTION("Server grid step cannot be greater than fetch period")
                << TErrorAttribute("server_grid_step", *readGridStep)
                << TErrorAttribute("period", *period);
        }

        if (period->GetValue() % readGridStep->GetValue() != 0) {
            THROW_ERROR_EXCEPTION("Server grid step must be multiple of fetch period")
                << TErrorAttribute("server_grid_step", *readGridStep)
                << TErrorAttribute("period", *period);
        }
    }
}

void TSolomonExporter::DoPushCoreProfiling()
{
    try {
        TForbidContextSwitchGuard guard;
        Registry_->ProcessRegistrations();
        Registry_->LegacyReadSensors();
    } catch (const std::exception& ex) {
        YT_LOG_ERROR(ex, "Legacy push failed");
    }
}

IYPathServicePtr TSolomonExporter::GetService() const
{
    return Root_->Via(Invoker_);
}

TErrorOr<TReadWindow> TSolomonExporter::SelectReadWindow(
    TInstant now,
    TDuration period,
    std::optional<TDuration> readGridStep,
    TDuration gridStep)
{
    TReadWindow readWindow;

    int gridSubsample = 1;
    if (readGridStep) {
        gridSubsample = *readGridStep / gridStep;
    }

    int gridFactor = gridStep / Config_->GridStep;

    for (auto [iteration, time] : Window_) {
        if (iteration % gridFactor != 0) {
            continue;
        }

        int index = Registry_->IndexOf(iteration / gridFactor);
        if (time >= now - period && time < now) {
            if (readWindow.empty() ||
                readWindow.back().first.size() >= static_cast<size_t>(gridSubsample)
            ) {
                readWindow.emplace_back(std::vector<int>{index}, time);
            } else {
                readWindow.back().first.push_back(index);
                readWindow.back().second = time;
            }
        }
    }

    auto readGrid = gridStep;
    if (readGridStep) {
        readGrid = *readGridStep;
    }

    if (readWindow.size() != period / readGrid ||
        readWindow.empty() ||
        readWindow.back().first.size() != static_cast<size_t>(gridSubsample))
    {
        return TError("Read query is outside of window")
            << TErrorAttribute("now", now)
            << TErrorAttribute("period", period)
            << TErrorAttribute("grid", readGridStep)
            << TErrorAttribute("window_first", Window_.front().second)
            << TErrorAttribute("window_last", Window_.back().second);
    }

    return readWindow;
}

bool TSolomonExporter::TryReplyFromCache(const TCacheKey& cacheKey, const NHttp::IResponseWriterPtr& rsp)
{
    auto cacheHitIt = ResponseCache_.find(cacheKey);
    if (cacheHitIt != ResponseCache_.end()) {
        YT_LOG_DEBUG("Replying from cache");

        ResponseCacheHit_.Increment();

        rsp->SetStatus(EStatusCode::OK);
        WaitFor(rsp->WriteBody(cacheHitIt->second))
            .ThrowOnError();

        return true;
    }

    return false;
}

void TSolomonExporter::CleanResponseCache()
{
    std::vector<TCacheKey> toRemove;
    for (const auto& [key, response] : ResponseCache_) {
        if (key.Now < TInstant::Now() - Config_->ResponseCacheTtl) {
            toRemove.push_back(key);
        }
    }

    for (const auto& removedKey : toRemove) {
        ResponseCache_.erase(removedKey);
    }
}

bool TSolomonExporter::FilterDefaultGrid(const TString& sensorName)
{
    auto shard = Config_->MatchShard(sensorName);
    if (shard && shard->GridStep) {
        return Config_->GridStep == *shard->GridStep;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

TSolomonExporter::TCacheKey::operator size_t() const
{
    size_t hash = 0;
    HashCombine(hash, Shard);
    HashCombine(hash, static_cast<int>(Format));
    HashCombine(hash, static_cast<int>(Compression));
    HashCombine(hash, Now);
    HashCombine(hash, Period);
    HashCombine(hash, Grid);
    return hash;
}

////////////////////////////////////////////////////////////////////////////////

TSolomonExporter::TSensorService::TSensorService(TSolomonRegistryPtr registry, IInvokerPtr invoker)
    : Registry_(std::move(registry))
    , Invoker_(std::move(invoker))
{ }

bool TSolomonExporter::TSensorService::DoInvoke(const NRpc::IServiceContextPtr& context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    return TYPathServiceBase::DoInvoke(context);
}

void TSolomonExporter::TSensorService::GetSelf(TReqGet* request, TRspGet* response, const TCtxGetPtr& context)
{
    VERIFY_INVOKER_AFFINITY(Invoker_);

    auto options = FromProto(request->options());
    auto name = options->Find<TString>("name");
    auto tagMap = options->Find<TTagMap>("tags").value_or(TTagMap{});
    auto exportSummaryAsMax = options->Find<bool>("export_summary_as_max").value_or(true);
    context->SetRequestInfo("Name: %v, Tags: %v, ExportSummaryAsMax: %v", name, tagMap, exportSummaryAsMax);

    if (!name) {
        response->set_value(BuildYsonStringFluently().Entity().ToString());
        context->Reply();
        return;
    }

    TTagList tags(tagMap.begin(), tagMap.end());
    TReadOptions readOptions{.ExportSummaryAsMax = exportSummaryAsMax};
    response->set_value(BuildYsonStringFluently()
        .Do([&](TFluentAny fluent) {
            Registry_->ReadRecentSensorValue(*name, tags, readOptions, fluent);
        }).ToString());
    context->Reply();
}

void TSolomonExporter::TSensorService::ListSelf(TReqList* request, TRspList* response, const TCtxListPtr& context)
{
    VERIFY_INVOKER_AFFINITY(Invoker_);

    auto attributeKeys = FromProto<THashSet<TString>>(request->attributes().keys());
    context->SetRequestInfo("AttributeKeys: %v", attributeKeys);

    response->set_value(BuildYsonStringFluently()
        .DoListFor(Registry_->ListSensors(), [&] (TFluentList fluent, const TSensorInfo& sensorInfo) {
            if (!sensorInfo.Error.IsOK()) {
                THROW_ERROR_EXCEPTION("Broken sensor")
                    << TErrorAttribute("name", sensorInfo.Name)
                    << sensorInfo.Error;
            }

            fluent
                .Item()
                .BeginAttributes()
                    .DoIf(attributeKeys.contains("cube_size"), [&] (TFluentMap fluent) {
                        fluent.Item("cube_size").Value(sensorInfo.CubeSize);
                    })
                    .DoIf(attributeKeys.contains("object_count"), [&] (TFluentMap fluent) {
                        fluent.Item("object_count").Value(sensorInfo.CubeSize);
                    })
                .EndAttributes()
                .Value(sensorInfo.Name);
        }).ToString());

    context->Reply();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
