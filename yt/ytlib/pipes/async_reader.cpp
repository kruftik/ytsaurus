#include "async_reader.h"
#include "non_block_reader.h"

#include "io_dispatcher.h"
#include "private.h"

namespace NYT {
namespace NPipes {

////////////////////////////////////////////////////////////////////////////////

TAsyncReader::TAsyncReader(int fd)
    : Reader(new NDetail::TNonblockingReader(fd))
    , ReadyPromise()
    , Logger(ReaderLogger)
{
    Logger.AddTag(Sprintf("FD: %s", ~ToString(fd)));

    FDWatcher.set(fd, ev::READ);
}

TAsyncReader::~TAsyncReader()
{
}

void TAsyncReader::OnRegistered(TError status)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(Lock);

    if (!status.IsOK()) {
        if (ReadyPromise) {
            ReadyPromise.Set(status);
            ReadyPromise.Reset();
        }
        RegistrationError = status;
    }
}

void TAsyncReader::OnUnregister(TError status)
{
    if (!status.IsOK()) {
        LOG_ERROR(status, "Failed to unregister");
    }
}

void TAsyncReader::DoStart(ev::dynamic_loop& eventLoop)
{
    VERIFY_THREAD_AFFINITY(EventLoop);

    TGuard<TSpinLock> guard(Lock);

    StartWatcher.set(eventLoop);
    StartWatcher.set<TAsyncReader, &TAsyncReader::OnStart>(this);
    StartWatcher.start();

    FDWatcher.set(eventLoop);
    FDWatcher.set<TAsyncReader, &TAsyncReader::OnRead>(this);
    FDWatcher.start();
}

void TAsyncReader::DoStop()
{
    VERIFY_THREAD_AFFINITY(EventLoop);

    FDWatcher.stop();
    StartWatcher.stop();

    Reader->Close();
}

void TAsyncReader::OnStart(ev::async&, int eventType)
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YCHECK((eventType & ev::ASYNC) == ev::ASYNC);

    TGuard<TSpinLock> guard(Lock);

    YCHECK(State_ == EAsyncIOState::Started);

    YCHECK(!Reader->IsClosed());
    FDWatcher.start();
}

void TAsyncReader::OnRead(ev::io&, int eventType)
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YCHECK((eventType & ev::READ) == ev::READ);

    TGuard<TSpinLock> guard(Lock);

    YCHECK(State_ == EAsyncIOState::Started);

    YCHECK(!Reader->ReachedEOF());

    if (!Reader->IsBufferFull()) {
        Reader->ReadToBuffer();

        if (!CanReadSomeMore()) {
            Stop();
        }

        if (Reader->IsReady()) {
            if (ReadyPromise) {
                ReadyPromise.Set(GetState());
                ReadyPromise.Reset();
            }
        }
    } else {
        // pause for a while
        FDWatcher.stop();
    }
}

std::pair<TBlob, bool> TAsyncReader::Read(TBlob&& buffer)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(Lock);

    // should refactor
    if (State_ == EAsyncIOState::Created) {
        return std::make_pair(TBlob(), false);
    }

    if (CanReadSomeMore()) {
        // is it safe?
        if (!FDWatcher.is_active()) {
            StartWatcher.send();
        }
    }

    return Reader->GetRead(std::move(buffer));
}

TAsyncError TAsyncReader::GetReadyEvent()
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(Lock);

    if ((State_ == EAsyncIOState::StartAborted) || !RegistrationError.IsOK() || Reader->IsReady()) {
        return MakePromise(GetState());
    }

    YCHECK(!ReadyPromise);
    ReadyPromise = NewPromise<TError>();
    return ReadyPromise.ToFuture();
}

TError TAsyncReader::Abort()
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(Lock);

    Unregister();

    if (State_ == EAsyncIOState::StartAborted) {
        // close the reader if TAsyncReader was aborted before registering
        Reader->Close();
    }

    if (ReadyPromise) {
        ReadyPromise.Set(GetState());
        ReadyPromise.Reset();
    }

    // report the last reader error if any
    if (Reader->InFailedState()) {
        return TError::FromSystem(Reader->GetLastSystemError());
    } else {
        return TError();
    }
}

bool TAsyncReader::CanReadSomeMore() const
{
    return !Reader->InFailedState() && !Reader->ReachedEOF();
}

TError TAsyncReader::GetState() const
{
    if (State_ == EAsyncIOState::StartAborted) {
        return TError("Start of the reader was aborted");
    } else if (!RegistrationError.IsOK()) {
        return RegistrationError;
    } else if (Reader->ReachedEOF() || !Reader->IsBufferEmpty()) {
        return TError();
    } else if (Reader->InFailedState()) {
        return TError::FromSystem(Reader->GetLastSystemError());
    } else {
        YCHECK(false);
        return TError();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
