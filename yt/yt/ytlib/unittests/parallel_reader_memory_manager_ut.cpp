#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader_memory_manager.h>
#include <yt/yt/ytlib/chunk_client/parallel_reader_memory_manager.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/scheduler.h>

#include <random>

namespace NYT::NChunkClient {
namespace {

constexpr auto WaitIterationCount = 50;
constexpr auto WaitIterationDuration = TDuration::MilliSeconds(5);
constexpr auto AssertIterationCount = 75;
constexpr auto AssertIterationDuration = TDuration::MilliSeconds(5);

////////////////////////////////////////////////////////////////////////////////

void WaitTestPredicate(std::function<bool()> predicate)
{
    WaitForPredicate(predicate, WaitIterationCount, WaitIterationDuration);
}

void AssertOverTime(std::function<bool()> predicate)
{
    for (int iteration = 0; iteration < AssertIterationCount; ++iteration) {
        if (!predicate()) {
            THROW_ERROR_EXCEPTION("Assert over time failed");
        }
        if (iteration + 1 < AssertIterationCount) {
            Sleep(AssertIterationDuration);
        }
    }
}

TEST(TParallelReaderMemoryManagerTest, TestMemoryManagerAllocatesDesiredMemorySizeIfPossible)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100'000,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();

    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 0; });

    reader1->SetRequiredMemorySize(123);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 123; });

    reader1->SetPrefetchMemorySize(234);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 357; });

    EXPECT_EQ(memoryManager->GetRequiredMemorySize(), 100'000);
    EXPECT_EQ(memoryManager->GetDesiredMemorySize(), 100'000);
    EXPECT_EQ(memoryManager->GetReservedMemorySize(), 100'000);
}

TEST(TParallelReaderMemoryManagerTest, TestChunkReaderMemoryManagerGetsMemory)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100'000,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(100);
    reader1->SetPrefetchMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 200; });
    EXPECT_EQ(reader1->GetFreeMemorySize(), 200);

    {
        auto acquire1 = reader1->AsyncAcquire(200);
        NConcurrency::WaitFor(acquire1)
            .ValueOrThrow();
    }

    EXPECT_EQ(reader1->GetFreeMemorySize(), 200);
    auto acquire2 = reader1->AsyncAcquire(201);
    AssertOverTime([&] { return !acquire2.IsSet(); });
}

TEST(TParallelReaderMemoryManagerTest, TestChunkReaderMemoryManagerRevokesMemory)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(50);
    reader1->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });
    EXPECT_EQ(reader1->GetFreeMemorySize(), 100);

    {
        auto acquire1 = reader1->AsyncAcquire(100);
        NConcurrency::WaitFor(acquire1)
            .ValueOrThrow();
    }

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(50);
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 50; });
    EXPECT_EQ(reader2->GetReservedMemorySize(), 50);
    EXPECT_EQ(reader1->GetFreeMemorySize(), 50);
    EXPECT_EQ(reader2->GetFreeMemorySize(), 50);

    auto acquire2 = reader2->AsyncAcquire(51);
    AssertOverTime([&] { return !acquire2.IsSet(); });
}

TEST(TParallelReaderMemoryManagerTest, TestChunkReaderMemoryManagerUnregister)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetPrefetchMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetPrefetchMemorySize(100);
    AssertOverTime([&] { return reader2->GetReservedMemorySize() == 0; });

    {
        auto allocation = reader1->AsyncAcquire(100);
        NConcurrency::WaitFor(allocation).ValueOrThrow();
        YT_UNUSED_FUTURE(reader1->Finalize());
        AssertOverTime([&] {
            return reader1->GetReservedMemorySize() == 100 && reader2->GetReservedMemorySize() == 0;
        });
    }

    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TParallelReaderMemoryManagerTest, TestMemoryManagerAllocatesAsMuchAsPossible)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 120,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();

    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 0; });

    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    reader1->SetPrefetchMemorySize(234);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 120; });

    EXPECT_EQ(memoryManager->GetRequiredMemorySize(), 120);
    EXPECT_EQ(memoryManager->GetDesiredMemorySize(), 334);
    EXPECT_EQ(memoryManager->GetReservedMemorySize(), 120);
}

TEST(TParallelReaderMemoryManagerTest, TestMemoryManagerFreesMemoryAfterUnregister)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(80);
    reader2->SetPrefetchMemorySize(80);
    AssertOverTime([&] {
        return reader1->GetReservedMemorySize() == 100 && reader2->GetReservedMemorySize() == 0;
    });

    YT_UNUSED_FUTURE(reader1->Finalize());
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TParallelReaderMemoryManagerTest, TestMemoryManagerBalancing1)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(50);
    reader1->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(50);
    reader2->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 50; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 50; });

    YT_UNUSED_FUTURE(reader1->Finalize());
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TParallelReaderMemoryManagerTest, TestMemoryManagerBalancing2)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(80);
    reader1->SetPrefetchMemorySize(100'000);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(50);
    reader2->SetPrefetchMemorySize(100'000);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 80; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 20; });

    auto holder3 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader3 = holder3->Get();
    reader3->SetRequiredMemorySize(50);
    reader3->SetPrefetchMemorySize(100'000);
    AssertOverTime([&] { return reader3->GetReservedMemorySize() == 0; });

    YT_UNUSED_FUTURE(reader2->Finalize());
    WaitTestPredicate([&] { return reader3->GetReservedMemorySize() == 20; });

    auto holder4 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader4 = holder4->Get();
    reader4->SetRequiredMemorySize(50);
    reader4->SetPrefetchMemorySize(100'000);
    AssertOverTime([&] { return reader4->GetReservedMemorySize() == 0; });

    YT_UNUSED_FUTURE(reader1->Finalize());
    WaitTestPredicate([&] { return reader3->GetReservedMemorySize() == 50; });
    WaitTestPredicate([&] { return reader4->GetReservedMemorySize() == 50; });
}


TEST(TParallelReaderMemoryManagerTest, TestInitialMemorySize)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 60
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager(1);
    auto reader1 = holder1->Get();
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 1; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager(100);
    auto reader2 = holder2->Get();
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 60; });

    auto holder3 = memoryManager->CreateChunkReaderMemoryManager(50);
    auto reader3 = holder3->Get();
    WaitTestPredicate([&] { return reader3->GetReservedMemorySize() == 39; });
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 1; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 60; });
}

TEST(TParallelReaderMemoryManagerTest, TestTotalSize)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(100);
    reader1->SetTotalSize(70);

    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 70; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 30; });
}

TEST(TParallelReaderMemoryManagerTest, TestFreeMemorySize)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    EXPECT_EQ(memoryManager->GetFreeMemorySize(), 100);

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(50);
    WaitTestPredicate([&] { return memoryManager->GetFreeMemorySize() == 50; });

    reader1->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] { return memoryManager->GetFreeMemorySize() == 0; });

    YT_UNUSED_FUTURE(reader1->Finalize());
    WaitTestPredicate([&] { return memoryManager->GetFreeMemorySize() == 100; });
}

TEST(TParallelReaderMemoryManagerTest, TestRequiredMemorySizeNeverDecreases)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 100,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });

    reader1->SetRequiredMemorySize(50);
    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(50);

    AssertOverTime([&] {
        return reader1->GetReservedMemorySize() == 100 && reader2->GetReservedMemorySize() == 0;
    });
}

TEST(TParallelReaderMemoryManagerTest, PerformanceAndStressTest)
{
    constexpr auto ReaderCount = 200'000;
    std::mt19937 rng(12345);

    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 10'000'000,
            .MaxInitialReaderReservedMemory = 10'000'000
        },
        actionQueue->GetInvoker());

    std::vector<TChunkReaderMemoryManagerHolderPtr> holders;
    std::vector<TChunkReaderMemoryManagerPtr> readers;
    holders.reserve(ReaderCount);
    readers.reserve(ReaderCount);

    for (size_t readerIndex = 0; readerIndex < ReaderCount; ++readerIndex) {
        holders.push_back(memoryManager->CreateChunkReaderMemoryManager());
        readers.push_back(holders.back()->Get());
        readers.back()->SetRequiredMemorySize(rng() % 100);
        readers.back()->SetPrefetchMemorySize(rng() % 100);
    }

    while (!readers.empty()) {
        if (rng() % 3 == 0) {
            YT_UNUSED_FUTURE(readers.back()->Finalize());
            readers.pop_back();
        } else {
            auto readerIndex = rng() % readers.size();
            readers[readerIndex]->SetRequiredMemorySize(rng() % 100);
            readers[readerIndex]->SetPrefetchMemorySize(rng() % 100);
        }
    }
}

TEST(TParallelReaderMemoryManagerTest, TestManyHeavyRebalancings)
{
    constexpr auto ReaderCount = 100'000;
    constexpr auto RebalancingIterations = 500;

    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 200'000,
            .MaxInitialReaderReservedMemory = 200'000
        },
        actionQueue->GetInvoker());

    std::vector<TChunkReaderMemoryManagerHolderPtr> holders;
    std::vector<TChunkReaderMemoryManagerPtr> readers;
    holders.resize(ReaderCount + 1);
    readers.resize(ReaderCount + 1);

    for (size_t readerIndex = 0; readerIndex < ReaderCount; ++readerIndex) {
        holders.push_back(memoryManager->CreateChunkReaderMemoryManager());
        readers.push_back(holders.back()->Get());
        readers.back()->SetRequiredMemorySize(1);
        readers.back()->SetPrefetchMemorySize(1);
    }

    // Each rebalancing iteration revokes unit memory from each reader to give
    // new reader required memory size and then returns this memory back to readers,
    // so rebalancing works slow here.
    for (size_t iteration = 0; iteration < RebalancingIterations; ++iteration) {
        holders.push_back(memoryManager->CreateChunkReaderMemoryManager());
        readers.push_back(holders.back()->Get());
        readers.back()->SetRequiredMemorySize(ReaderCount);

        // All rebalancings except the first should be fast.
        if (iteration == 0) {
            WaitForPredicate([&] { return readers.back()->GetReservedMemorySize() == ReaderCount; }, 1000, WaitIterationDuration);
        } else {
            WaitTestPredicate([&] { return readers.back()->GetReservedMemorySize() == ReaderCount; });
        }
        YT_UNUSED_FUTURE(readers.back()->Finalize());
        readers.pop_back();
    }
}

TEST(TParallelReaderMemoryManagerTest, TestDynamicReservedMemory)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 200,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto holder1 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader1 = holder1->Get();
    reader1->SetRequiredMemorySize(100);
    reader1->SetPrefetchMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 200; });

    auto holder2 = memoryManager->CreateChunkReaderMemoryManager();
    auto reader2 = holder2->Get();
    reader2->SetRequiredMemorySize(100);
    reader2->SetPrefetchMemorySize(100);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 100; });

    memoryManager->SetReservedMemorySize(456);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 200; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 200; });

    memoryManager->SetReservedMemorySize(200);
    WaitTestPredicate([&] { return reader1->GetReservedMemorySize() == 100; });
    WaitTestPredicate([&] { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TParallelReaderMemoryManagerTest, TestMemoryManagersTree)
{
    /*
     *          mm11
     *          / \
     *         /   \
     *        /     \
     *      mm21    mm22
     *      / \     / \
     *     r1 r2   r3 r4
     *
     */

    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto mm11 = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 6,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto mm21 = mm11->CreateMultiReaderMemoryManager();
    auto mm22 = mm11->CreateMultiReaderMemoryManager();

    auto h1 = mm21->CreateChunkReaderMemoryManager();
    auto r1 = h1->Get();
    auto h2 = mm21->CreateChunkReaderMemoryManager();
    auto r2 = h2->Get();
    auto h3 = mm22->CreateChunkReaderMemoryManager();
    auto r3 = h3->Get();
    auto h4 = mm22->CreateChunkReaderMemoryManager();
    auto r4 = h4->Get();

    auto memoryRequirementsSatisfied = [&] {
        for (const auto& reader : {r1, r2, r3, r4}) {
            if (reader->GetReservedMemorySize() < reader->GetRequiredMemorySize()) {
                return false;
            }
        }

        return true;
    };

    r1->SetRequiredMemorySize(1);
    r1->SetPrefetchMemorySize(1);
    WaitTestPredicate([&] { return memoryRequirementsSatisfied(); });

    r2->SetRequiredMemorySize(1);
    r2->SetPrefetchMemorySize(1);
    WaitTestPredicate([&] { return memoryRequirementsSatisfied(); });

    r3->SetRequiredMemorySize(1);
    r3->SetPrefetchMemorySize(1);
    WaitTestPredicate([&] { return memoryRequirementsSatisfied(); });

    r4->SetRequiredMemorySize(1);
    r4->SetPrefetchMemorySize(1);
    WaitTestPredicate([&] { return memoryRequirementsSatisfied(); });
    EXPECT_EQ(r1->GetReservedMemorySize() + r2->GetReservedMemorySize() + r3->GetReservedMemorySize() + r4->GetReservedMemorySize(), 6);

    mm11->SetReservedMemorySize(10);
    WaitTestPredicate([&] { return r1->GetReservedMemorySize() == 2; });
    WaitTestPredicate([&] { return r2->GetReservedMemorySize() == 2; });
    WaitTestPredicate([&] { return r3->GetReservedMemorySize() == 2; });
    WaitTestPredicate([&] { return r4->GetReservedMemorySize() == 2; });

    mm11->SetReservedMemorySize(4);
    WaitTestPredicate([&] { return r1->GetReservedMemorySize() == 1; });
    WaitTestPredicate([&] { return r2->GetReservedMemorySize() == 1; });
    WaitTestPredicate([&] { return r3->GetReservedMemorySize() == 1; });
    WaitTestPredicate([&] { return r4->GetReservedMemorySize() == 1; });
}

TEST(TParallelReaderMemoryManagerTest, TParallelReaderMemoryManagerTestFinalize)
{
    /*
     *          mm11
     *          / \
     *         /   \
     *        /     \
     *      mm21    mm22
     *       |       |
     *       r1      r2
     *
     */

    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto mm11 = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions{
            .TotalReservedMemorySize = 10,
            .MaxInitialReaderReservedMemory = 0
        },
        actionQueue->GetInvoker());

    auto mm21 = mm11->CreateMultiReaderMemoryManager(5);
    auto mm22 = mm11->CreateMultiReaderMemoryManager();

    auto h1 = mm21->CreateChunkReaderMemoryManager();
    auto r1 = h1->Get();
    r1->SetRequiredMemorySize(5);
    auto h2 = mm22->CreateChunkReaderMemoryManager();
    auto r2 = h2->Get();
    r2->SetRequiredMemorySize(5);
    r2->SetPrefetchMemorySize(5);

    WaitTestPredicate([&] { return r1->GetReservedMemorySize() == 5; });
    WaitTestPredicate([&] { return r2->GetReservedMemorySize() == 5; });

    YT_UNUSED_FUTURE(r1->Finalize());
    YT_UNUSED_FUTURE(mm21->Finalize());
    WaitTestPredicate([&] { return r2->GetReservedMemorySize() == 10; });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NChunkClient
