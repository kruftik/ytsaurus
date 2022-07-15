#include "response_keeper.h"
#include "private.h"
#include "config.h"
#include "helpers.h"
#include "service.h"

#include <atomic>
#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/misc/ring_queue.h>

#include <yt/yt/core/profiling/profiler.h>
#include <yt/yt/core/profiling/timing.h>

namespace NYT::NRpc {

using namespace NConcurrency;
using namespace NThreading;

////////////////////////////////////////////////////////////////////////////////

static constexpr auto EvictionPeriod = TDuration::Seconds(1);
static constexpr auto EvictionTickTimeCheckPeriod = 1024;

////////////////////////////////////////////////////////////////////////////////

class TResponseKeeper::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TResponseKeeperConfigPtr config,
        IInvokerPtr invoker,
        const NLogging::TLogger& logger,
        const NProfiling::TProfiler& profiler)
        : Config_(std::move(config))
        , Invoker_(std::move(invoker))
        , Logger(logger)
    {
        YT_VERIFY(Config_);
        YT_VERIFY(Invoker_);

        EvictionExecutor_ = New<TPeriodicExecutor>(
            Invoker_,
            BIND(&TImpl::OnEvict, MakeWeak(this)),
            EvictionPeriod);
        EvictionExecutor_->Start();

        profiler.AddFuncGauge("/response_keeper/kept_response_count", MakeStrong(this), [this] {
            return FinishedResponseCount_;
        });
        profiler.AddFuncGauge("/response_keeper/kept_response_space", MakeStrong(this), [this] {
            return FinishedResponseSpace_;
        });
    }

    void Start()
    {
        auto guard = WriterGuard(Lock_);

        if (Started_) {
            return;
        }

        WarmupDeadline_ = Config_->EnableWarmup
            ? NProfiling::GetCpuInstant() + NProfiling::DurationToCpuDuration(Config_->WarmupTime)
            : 0;
        Started_ = true;

        YT_LOG_INFO("Response keeper started (WarmupTime: %v, ExpirationTime: %v)",
            Config_->WarmupTime,
            Config_->ExpirationTime);
    }

    void Stop()
    {
        auto guard = WriterGuard(Lock_);

        if (!Started_) {
            return;
        }

        PendingResponses_.clear();
        FinishedResponses_.clear();
        ResponseEvictionQueue_.clear();
        FinishedResponseSpace_ = 0;
        FinishedResponseCount_ = 0;
        Started_ = false;

        YT_LOG_INFO("Response keeper stopped");
    }

    TFuture<TSharedRefArray> TryBeginRequest(TMutationId id, bool isRetry)
    {
        auto guard = WriterGuard(Lock_);

        return DoTryBeginRequest(id, isRetry);
    }

    TFuture<TSharedRefArray> FindRequest(TMutationId id, bool isRetry) const
    {
        auto guard = ReaderGuard(Lock_);

        return DoFindRequest(id, isRetry);
    }

    void EndRequest(TMutationId id, TSharedRefArray response, bool remember)
    {
        auto guard = WriterGuard(Lock_);

        YT_ASSERT(id);

        if (!Started_) {
            return;
        }

        if (!response) {
            YT_LOG_ALERT("Null response passed to response keeper (MutationId: %v, Remember: %v)",
                id,
                remember);
        }


        TPromise<TSharedRefArray> promise;
        if (auto pendingIt = PendingResponses_.find(id)) {
            promise = std::move(pendingIt->second);
            PendingResponses_.erase(pendingIt);
        }

        if (remember) {
            // NB: Allow duplicates.
            auto [it, inserted] = FinishedResponses_.emplace(id, response);
            if (!inserted) {
                return;
            }

            auto space = static_cast<i64>(GetByteSize(response));
            ResponseEvictionQueue_.push(TEvictionItem{
                id,
                NProfiling::GetCpuInstant(),
                space,
                it
            });

            FinishedResponseCount_ += 1;
            FinishedResponseSpace_ += space;
        }

        if (promise) {
            guard.Release();
            promise.Set(response);
        }
    }

    void EndRequest(TMutationId id, TErrorOr<TSharedRefArray> responseOrError, bool remember)
    {
        YT_ASSERT(id);

        if (responseOrError.IsOK()) {
            EndRequest(id, std::move(responseOrError.Value()), remember);
            return;
        }

        auto guard = WriterGuard(Lock_);

        if (!Started_) {
            return;
        }

        auto it = PendingResponses_.find(id);
        if (it == PendingResponses_.end()) {
            return;
        }

        auto promise = std::move(it->second);
        PendingResponses_.erase(it);

        guard.Release();

        promise.Set(TError(std::move(responseOrError)));
    }

    void CancelPendingRequests(const TError& error)
    {
        auto guard = WriterGuard(Lock_);

        if (!Started_) {
            return;
        }

        auto pendingResponses = std::move(PendingResponses_);

        guard.Release();

        for (const auto& [id, promise] : pendingResponses) {
            promise.Set(error);
        }

        YT_LOG_INFO(error, "All pending requests canceled");
    }

    bool TryReplyFrom(const IServiceContextPtr& context, bool subscribeToResponse)
    {
        auto guard = WriterGuard(Lock_);

        auto mutationId = context->GetMutationId();
        if (!mutationId) {
            return false;
        }

        if (auto keptAsyncResponseMessage = DoTryBeginRequest(mutationId, context->IsRetry())) {
            context->ReplyFrom(std::move(keptAsyncResponseMessage));
            return true;
        }

        if (subscribeToResponse) {
            context->GetAsyncResponseMessage()
                .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TSharedRefArray>&) {
                    auto responseMessage = context->GetResponseMessage();
                    bool remember = context->GetError().GetCode() != NRpc::EErrorCode::Unavailable;
                    Invoker_->Invoke(BIND([this, this_ = std::move(this_), mutationId, remember, responseMessage = std::move(responseMessage)] {
                        EndRequest(
                            mutationId,
                            std::move(responseMessage),
                            remember);
                    }));
                }));
        }

        return false;
    }

    bool IsWarmingUp() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return NProfiling::GetCpuInstant() < WarmupDeadline_;
    }

private:
    const TResponseKeeperConfigPtr Config_;
    const IInvokerPtr Invoker_;
    const NLogging::TLogger Logger;

    TPeriodicExecutorPtr EvictionExecutor_;

    YT_DECLARE_SPIN_LOCK(TReaderWriterSpinLock, Lock_);

    bool Started_ = false;
    NProfiling::TCpuInstant WarmupDeadline_ = 0;

    using TFinishedResponseMap = THashMap<TMutationId, TSharedRefArray>;
    TFinishedResponseMap FinishedResponses_;

    int FinishedResponseCount_ = 0;
    i64 FinishedResponseSpace_ = 0;

    struct TEvictionItem
    {
        TMutationId Id;
        NProfiling::TCpuInstant When;
        i64 Space;
        TFinishedResponseMap::iterator Iterator;
    };

    TRingQueue<TEvictionItem> ResponseEvictionQueue_;

    THashMap<TMutationId, TPromise<TSharedRefArray>> PendingResponses_;

    TFuture<TSharedRefArray> DoTryBeginRequest(TMutationId id, bool isRetry)
    {
        VERIFY_SPINLOCK_AFFINITY(Lock_);

        auto result = DoFindRequest(id, isRetry);
        if (!result) {
            EmplaceOrCrash(PendingResponses_, std::make_pair(id, NewPromise<TSharedRefArray>()));
        }
        return result;
    }

    TFuture<TSharedRefArray> DoFindRequest(TMutationId id, bool isRetry) const
    {
        VERIFY_SPINLOCK_AFFINITY(Lock_);
        YT_ASSERT(id);

        if (!Started_) {
            THROW_ERROR_EXCEPTION("Response keeper is not active");
        }

        auto pendingIt = PendingResponses_.find(id);
        if (pendingIt != PendingResponses_.end()) {
            if (!isRetry) {
                THROW_ERROR_EXCEPTION("Duplicate request is not marked as \"retry\"")
                    << TErrorAttribute("mutation_id", id);
            }
            YT_LOG_DEBUG("Replying with pending response (MutationId: %v)", id);
            return pendingIt->second;
        }

        auto finishedIt = FinishedResponses_.find(id);
        if (finishedIt != FinishedResponses_.end()) {
            if (!isRetry) {
                THROW_ERROR_EXCEPTION("Duplicate request is not marked as \"retry\"")
                    << TErrorAttribute("mutation_id", id);
            }
            YT_LOG_DEBUG("Replying with finished response (MutationId: %v)", id);
            return MakeFuture(finishedIt->second);
        }

        if (isRetry && IsWarmingUp()) {
            THROW_ERROR_EXCEPTION("Cannot reliably check for a duplicate mutating request")
                << TErrorAttribute("mutation_id", id)
                << TErrorAttribute("warmup_time", Config_->WarmupTime);
        }

        return {};
    }

    void OnEvict()
    {
        auto guard = WriterGuard(Lock_);

        if (!Started_) {
            return;
        }

        YT_LOG_DEBUG("Response Keeper eviction tick started");

        NProfiling::TWallTimer timer;
        int counter = 0;

        auto deadline = NProfiling::GetCpuInstant() - NProfiling::DurationToCpuDuration(Config_->ExpirationTime);
        while (!ResponseEvictionQueue_.empty()) {
            const auto& item = ResponseEvictionQueue_.front();
            if (item.When > deadline) {
                break;
            }

            if (++counter % EvictionTickTimeCheckPeriod == 0) {
                if (timer.GetElapsedTime() > Config_->MaxEvictionTickTime) {
                    YT_LOG_DEBUG("Response Keeper eviction tick interrupted (ResponseCount: %v)",
                        counter);
                    return;
                }
            }

            FinishedResponses_.erase(item.Iterator);

            FinishedResponseCount_ -= 1;
            FinishedResponseSpace_ -= item.Space;

            ResponseEvictionQueue_.pop();
        }

        YT_LOG_DEBUG("Response Keeper eviction tick completed (ResponseCount: %v)",
            counter);
    }
};

////////////////////////////////////////////////////////////////////////////////

TResponseKeeper::TResponseKeeper(
    TResponseKeeperConfigPtr config,
    IInvokerPtr invoker,
    const NLogging::TLogger& logger,
    const NProfiling::TProfiler& profiler)
    : Impl_(New<TImpl>(
        std::move(config),
        std::move(invoker),
        logger,
        profiler))
{ }

TResponseKeeper::~TResponseKeeper() = default;

void TResponseKeeper::Start()
{
    Impl_->Start();
}

void TResponseKeeper::Stop()
{
    Impl_->Stop();
}

TFuture<TSharedRefArray> TResponseKeeper::TryBeginRequest(TMutationId id, bool isRetry)
{
    return Impl_->TryBeginRequest(id, isRetry);
}

TFuture<TSharedRefArray> TResponseKeeper::FindRequest(TMutationId id, bool isRetry) const
{
    return Impl_->FindRequest(id, isRetry);
}

void TResponseKeeper::EndRequest(TMutationId id, TSharedRefArray response, bool remember)
{
    Impl_->EndRequest(id, std::move(response), remember);
}

void TResponseKeeper::EndRequest(TMutationId id, TErrorOr<TSharedRefArray> responseOrError, bool remember)
{
    Impl_->EndRequest(id, std::move(responseOrError), remember);
}

void TResponseKeeper::CancelPendingRequests(const TError& error)
{
    Impl_->CancelPendingRequests(error);
}

bool TResponseKeeper::TryReplyFrom(const IServiceContextPtr& context, bool subscribeToResponse)
{
    return Impl_->TryReplyFrom(context, subscribeToResponse);
}

bool TResponseKeeper::IsWarmingUp() const
{
    return Impl_->IsWarmingUp();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
