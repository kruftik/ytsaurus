#include "checkpointable_stream.h"
#include "serialize.h"
#include "checkpointable_stream_block_header.h"

#include <yt/core/misc/error.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TCheckpointableInputStream
    : public ICheckpointableInputStream
{
public:
    explicit TCheckpointableInputStream(IInputStream* underlyingStream)
        : UnderlyingStream_(underlyingStream)
    { }

    virtual void SkipToCheckpoint() override
    {
        while (true) {
            if (!EnsureBlock()) {
                break;
            }
            if (BlockLength_ == TCheckpointableStreamBlockHeader::CheckpointSentinel) {
                HasBlock_ = false;
                break;
            }
            UnderlyingStream_->Skip(BlockLength_ - BlockOffset_);
            HasBlock_ = false;
        }
    }

private:
    IInputStream* const UnderlyingStream_;

    size_t BlockLength_;
    size_t BlockOffset_;
    bool HasBlock_ = false;


    virtual size_t DoRead(void* buf_, size_t len) override
    {
        char* buf = reinterpret_cast<char*>(buf_);
        size_t pos = 0;
        while (pos < len) {
            if (!EnsureBlock()) {
                break;
            }
            size_t size = std::min(BlockLength_ - BlockOffset_, len - pos);
            auto loadedSize = UnderlyingStream_->Load(buf + pos, size);
            if (loadedSize != size) {
                THROW_ERROR_EXCEPTION("Broken checkpointable stream: expected %v bytes, got %v",
                    size,
                    loadedSize);
            }
            pos += size;
            BlockOffset_ += size;
            if (BlockOffset_ == BlockLength_) {
                HasBlock_ = false;
            }
        }
        return pos;
    }

    bool EnsureBlock()
    {
        if (!HasBlock_) {
            TCheckpointableStreamBlockHeader header;
            auto loadedSize = UnderlyingStream_->Load(&header, sizeof(header));
            if (loadedSize == 0) {
                return false;
            }

            if (loadedSize != sizeof(TCheckpointableStreamBlockHeader)) {
                THROW_ERROR_EXCEPTION("Broken checkpointable stream: expected %v bytes, got %v",
                    sizeof(TCheckpointableStreamBlockHeader),
                    loadedSize);
            }

            HasBlock_ = true;
            BlockLength_ = header.Length;
            BlockOffset_ = 0;
        }

        return true;
    }
};

std::unique_ptr<ICheckpointableInputStream> CreateCheckpointableInputStream(
    IInputStream* underlyingStream)
{
    return std::unique_ptr<ICheckpointableInputStream>(new TCheckpointableInputStream(
        underlyingStream));
}

////////////////////////////////////////////////////////////////////////////////

class TCheckpointableOutputStream
    : public ICheckpointableOutputStream
{
public:
    explicit TCheckpointableOutputStream(IOutputStream* underlyingStream)
        : UnderlyingStream_(underlyingStream)
    { }

    virtual void MakeCheckpoint() override
    {
        WritePod(*UnderlyingStream_, TCheckpointableStreamBlockHeader{TCheckpointableStreamBlockHeader::CheckpointSentinel});
    }

private:
    IOutputStream* const UnderlyingStream_;


    virtual void DoWrite(const void* buf, size_t len) override
    {
        if (len == 0) {
            return;
        }

        WritePod(*UnderlyingStream_, TCheckpointableStreamBlockHeader{len});
        UnderlyingStream_->Write(buf, len);
    }
};

std::unique_ptr<ICheckpointableOutputStream> CreateCheckpointableOutputStream(
    IOutputStream* underlyingStream)
{
    return std::unique_ptr<ICheckpointableOutputStream>(new TCheckpointableOutputStream(
        underlyingStream));
}

////////////////////////////////////////////////////////////////////////////////

class TBufferedCheckpointableOutputStream
    : public ICheckpointableOutputStream
{
public:
    TBufferedCheckpointableOutputStream(
        ICheckpointableOutputStream* underlyingStream,
        size_t bufferSize)
        : UnderlyingStream_(underlyingStream)
        , BufferSize_(bufferSize)
        , WriteThroughSize_(bufferSize / 2)
        , Buffer_(BufferSize_)
        , BufferBegin_(Buffer_.data())
        , BufferCurrent_(Buffer_.data())
        , BufferRemaining_(BufferSize_)
    {
        YT_VERIFY(BufferSize_ > 0);
    }

    virtual void MakeCheckpoint() override
    {
        Flush();
        UnderlyingStream_->MakeCheckpoint();
    }

    virtual ~TBufferedCheckpointableOutputStream()
    {
        try {
            Finish();
        } catch (...) {
        }
    }

private:
    ICheckpointableOutputStream* const UnderlyingStream_;
    const size_t BufferSize_;
    const size_t WriteThroughSize_;

    std::vector<char> Buffer_;
    char* BufferBegin_;
    char* BufferCurrent_;
    size_t BufferRemaining_;


    virtual void DoWrite(const void* buf, size_t len) override
    {
        const char* current = static_cast<const char*>(buf);
        size_t remaining = len;
        while (remaining > 0) {
            if (BufferRemaining_ == 0) {
                // Flush buffer.
                Flush();
            }
            size_t bytes = std::min(BufferRemaining_, remaining);
            if (BufferRemaining_ == BufferSize_ && bytes >= WriteThroughSize_) {
                // Write-through.
                UnderlyingStream_->Write(current, bytes);
            } else {
                // Copy into buffer.
                ::memcpy(BufferCurrent_, current, bytes);
                BufferCurrent_ += bytes;
                BufferRemaining_ -= bytes;
            }
            current += bytes;
            remaining -= bytes;
        }
    }

    virtual void DoFlush() override
    {
        UnderlyingStream_->Write(BufferBegin_, BufferCurrent_ - BufferBegin_);
        BufferCurrent_ = BufferBegin_;
        BufferRemaining_ = BufferSize_;
        UnderlyingStream_->Flush();
    }
};

std::unique_ptr<ICheckpointableOutputStream> CreateBufferedCheckpointableOutputStream(
    ICheckpointableOutputStream* underlyingStream,
    size_t bufferSize)
{
    return std::unique_ptr<ICheckpointableOutputStream>(new TBufferedCheckpointableOutputStream(
        underlyingStream,
        bufferSize));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

