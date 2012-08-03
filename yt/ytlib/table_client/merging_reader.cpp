﻿#include "stdafx.h"
#include "merging_reader.h"
#include "table_chunk_sequence_reader.h"
#include "key.h"

#include <ytlib/misc/sync.h>

#include <ytlib/actions/parallel_awaiter.h>

#include <ytlib/ytree/yson_string.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

namespace {

inline bool CompareReaders(
    const TTableChunkSequenceReader* lhs,
    const TTableChunkSequenceReader* rhs)
{
    return CompareKeys(lhs->GetKey(), rhs->GetKey()) > 0;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TMergingReader
    : public ISyncReader
{
public:
    explicit TMergingReader(const std::vector<TTableChunkSequenceReaderPtr>& readers)
        : Readers(readers)
    { }

    virtual void Open() override
    {
        // Open all readers in parallel and wait until of them are opened.
        auto awaiter = New<TParallelAwaiter>(NChunkClient::ReaderThread->GetInvoker());
        std::vector<TError> errors;

        FOREACH (auto reader, Readers) {
            awaiter->Await(
                reader->AsyncOpen(),
                BIND([&] (TError error) {
                    if (!error.IsOK()) {
                        errors.push_back(error);
                    }
            }));
        }

        TPromise<void> completed(NewPromise<void>());
        awaiter->Complete(BIND([=] () mutable {
            completed.Set();
        }));
        completed.Get();

        if (!errors.empty()) {
            Stroka message = "Error opening merging reader\n";
            FOREACH (const auto& error, errors) {
                message.append("\n");
                message.append(error.ToString());
            }
            ythrow yexception() << message;
        }

        // Push all non-empty readers to the heap.
        FOREACH (auto reader, Readers) {
            if (reader->IsValid()) {
                ReaderHeap.push_back(~reader);
            }
        }

        // Prepare the heap.
        if (!ReaderHeap.empty()) {
            std::make_heap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
            std::pop_heap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
        }
    }

    virtual void NextRow() override
    {
        if (ReaderHeap.empty())
            return;

        auto* currentReader = ReaderHeap.back();

        if (!currentReader->FetchNextItem()) {
            Sync(currentReader, &TTableChunkSequenceReader::GetReadyEvent);
        }

        if (currentReader->IsValid()) {
            std::push_heap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
            std::pop_heap(ReaderHeap.begin(), ReaderHeap.end(), CompareReaders);
        } else {
            ReaderHeap.pop_back();
        }
    }

    virtual bool IsValid() const override
    {
        return !ReaderHeap.empty();
    }

    virtual const TRow& GetRow() const override
    {
        return ReaderHeap.back()->GetRow();
    }

    virtual const TNonOwningKey& GetKey() const override
    {
        return ReaderHeap.back()->GetKey();
    }

private:
    std::vector<TTableChunkSequenceReaderPtr> Readers;
    std::vector<TTableChunkSequenceReader*> ReaderHeap;

};

ISyncReaderPtr CreateMergingReader(const std::vector<TTableChunkSequenceReaderPtr>& readers)
{
    return New<TMergingReader>(readers);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
