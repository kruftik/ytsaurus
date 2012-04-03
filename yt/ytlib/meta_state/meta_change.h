#pragma once

#include "public.h"

#include <ytlib/actions/cancelable_context.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

template <class TResult>
class TMetaChange
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TMetaChange> TPtr;
    typedef TCallback<TResult()> TChangeFunc;

    TMetaChange(
        IMetaStateManager* metaStateManager,
        TChangeFunc func,
        const TSharedRef& changeData);

    typename TFuture<TResult>::TPtr Commit();

    TPtr SetRetriable(TDuration backoffTime);
    TPtr OnSuccess(TCallback<void(TResult)> onSuccess);
    TPtr OnError(TCallback<void()> onError);

private:
    typedef TMetaChange<TResult> TThis;

    IMetaStateManagerPtr MetaStateManager;
    TChangeFunc Func;
    TClosure ChangeAction;
    TSharedRef ChangeData;
    bool Started;
    bool Retriable;

    TCancelableContextPtr EpochContext;
    TDuration BackoffTime;
    TCallback<void(TResult)> OnSuccess_;
    TClosure OnError_;
    typename TFuture<TResult>::TPtr AsyncResult;
    TResult Result;

    void DoCommit();
    void ChangeFuncThunk();
    void OnCommitted(ECommitResult result);

};

template <class TTarget, class TMessage, class TResult>
typename TMetaChange<TResult>::TPtr CreateMetaChange(
    IMetaStateManager* metaStateManager,
    const TMessage& message,
    TResult (TTarget::* func)(const TMessage&),
    TTarget* target);

template <class TMessage, class TResult>
typename TMetaChange<TResult>::TPtr CreateMetaChange(
    IMetaStateManager* metaStateManager,
    const TMessage& message,
    TCallback<TResult()> func);

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

#define META_CHANGE_INL_H_
#include "meta_change-inl.h"
#undef META_CHANGE_INL_H_
