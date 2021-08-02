#pragma once

#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/server/lib/tablet_node/config.h>
#include <yt/yt/server/node/tablet_node/sorted_dynamic_store.h>
#include <yt/yt/server/node/tablet_node/sorted_store_manager.h>
#include <yt/yt/server/node/tablet_node/ordered_dynamic_store.h>
#include <yt/yt/server/node/tablet_node/ordered_store_manager.h>
#include <yt/yt/server/node/tablet_node/tablet.h>
#include <yt/yt/server/node/tablet_node/tablet_manager.h>
#include <yt/yt/server/node/tablet_node/transaction.h>
#include <yt/yt/server/node/tablet_node/automaton.h>
#include <yt/yt/server/node/tablet_node/structured_logger.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_options.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/config.h>
#include <yt/yt/ytlib/chunk_client/memory_reader.h>
#include <yt/yt/ytlib/chunk_client/memory_writer.h>

#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/versioned_reader.h>
#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/writer.h>
#include <yt/yt/client/table_client/helpers.h>

#include <yt/yt/ytlib/table_client/schemaful_chunk_reader.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/client/transaction_client/timestamp_provider.h>

#include <yt/yt/ytlib/query_client/column_evaluator.h>
#include <yt/yt/ytlib/query_client/config.h>

#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/misc/optional.h>

namespace NYT::NTabletNode {
namespace {

using namespace NHydra;
using namespace NObjectClient;
using namespace NQueryClient;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

class TDynamicStoreTestBase
    : public ::testing::Test
    , public ITabletContext
{
protected:
    // ITabletContext implementation.
    virtual TCellId GetCellId() override
    {
        return NullCellId;
    }

    virtual const TString& GetTabletCellBundleName() override
    {
        const static TString TabletCellBundleName;
        return TabletCellBundleName;
    }

    virtual EPeerState GetAutomatonState() override
    {
        return EPeerState::Leading;
    }

    virtual IColumnEvaluatorCachePtr GetColumnEvaluatorCache() override
    {
        return ColumnEvaluatorCache_;
    }

    virtual NTabletNode::IRowComparerProviderPtr GetRowComparerProvider() override
    {
        return RowComparerProvider_;
    }

    virtual TObjectId GenerateId(EObjectType /*type*/) override
    {
        return TObjectId::Create();
    }

    virtual IStorePtr CreateStore(
        TTablet* tablet,
        EStoreType type,
        TStoreId storeId,
        const NTabletNode::NProto::TAddStoreDescriptor* /*descriptor*/) override
    {
        switch (type) {
            case EStoreType::SortedDynamic:
                return New<TSortedDynamicStore>(
                    New<TTabletManagerConfig>(),
                    storeId,
                    tablet);
            case EStoreType::OrderedDynamic:
                return New<TOrderedDynamicStore>(
                    New<TTabletManagerConfig>(),
                    storeId,
                    tablet);
            default:
                YT_ABORT();
        }
    }

    virtual THunkChunkPtr CreateHunkChunk(
        TTablet* /*tablet*/,
        TChunkId /*chunkId*/,
        const NTabletNode::NProto::TAddHunkChunkDescriptor* /*descriptor*/) override
    {
        YT_ABORT();
    }

    virtual TTransactionManagerPtr GetTransactionManager() override
    {
        return nullptr;
    }

    virtual NRpc::IServerPtr GetLocalRpcServer() override
    {
        return nullptr;
    }

    virtual NNodeTrackerClient::TNodeDescriptor GetLocalDescriptor() override
    {
        return NNodeTrackerClient::NullNodeDescriptor();
    }

    virtual NClusterNode::TNodeMemoryTrackerPtr GetMemoryUsageTracker() override
    {
        return nullptr;
    }

    virtual IStoreManagerPtr CreateStoreManager(TTablet* /*tablet*/)
    {
        return nullptr;
    }


    virtual void SetUp() override
    {
        auto schema = GetSchema();

        NameTable_ = TNameTable::FromSchema(*schema);

        bool sorted = schema->IsSorted();
        if (!sorted) {
            QueryNameTable_ = TNameTable::FromSchema(*schema->ToQuery());
        }

        ChunkReadOptions_.ChunkReaderStatistics = New<NChunkClient::TChunkReaderStatistics>();

        Tablet_ = std::make_unique<TTablet>(
            NullTabletId,
            TTableSettings::CreateNew(),
            0,
            NullObjectId,
            "ut",
            this,
            /*schemaId*/ NullObjectId,
            schema,
            sorted ? MinKey() : TLegacyOwningKey(),
            sorted ? MaxKey() : TLegacyOwningKey(),
            GetAtomicity(),
            GetCommitOrdering(),
            TTableReplicaId(),
            0);
        Tablet_->SetStructuredLogger(CreateMockPerTabletStructuredLogger(Tablet_.get()));

        auto storeManager = CreateStoreManager(Tablet_.get());
        Tablet_->SetStoreManager(storeManager);

        SetupTablet();
    }

    virtual void SetupTablet() = 0;

    virtual TTableSchemaPtr GetSchema() const = 0;

    virtual void CreateDynamicStore()
    { }

    virtual IDynamicStorePtr GetDynamicStore()
    {
        YT_ABORT();
    }

    virtual EAtomicity GetAtomicity() const
    {
        return EAtomicity::Full;
    }

    virtual ECommitOrdering GetCommitOrdering() const
    {
        return ECommitOrdering::Weak;
    }


    TTimestamp GenerateTimestamp()
    {
        return CurrentTimestamp_++;
    }

    std::unique_ptr<TTransaction> StartTransaction(TTimestamp startTimestamp = NullTimestamp)
    {
        auto transaction = std::make_unique<TTransaction>(TTransactionId::Create());
        transaction->SetStartTimestamp(startTimestamp == NullTimestamp ? GenerateTimestamp() : startTimestamp);
        transaction->SetState(ETransactionState::Active);
        return transaction;
    }

    void PrepareTransaction(TTransaction* transaction)
    {
        EXPECT_EQ(ETransactionState::Active, transaction->GetState());
        transaction->SetPrepareTimestamp(GenerateTimestamp());
        transaction->SetState(ETransactionState::TransientCommitPrepared);
    }

    NTransactionClient::TTimestamp CommitTransaction(TTransaction* transaction)
    {
        EXPECT_EQ(ETransactionState::TransientCommitPrepared, transaction->GetState());
        transaction->SetCommitTimestamp(GenerateTimestamp());
        transaction->SetState(ETransactionState::Committed);
        return transaction->GetCommitTimestamp();
    }

    void AbortTransaction(TTransaction* transaction)
    {
        transaction->SetState(ETransactionState::Aborted);
    }


    TUnversionedOwningRow BuildRow(const TString& yson, bool treatMissingAsNull = true)
    {
        return NTableClient::YsonToSchemafulRow(yson, *Tablet_->GetPhysicalSchema(), treatMissingAsNull);
    }

    TUnversionedOwningRow BuildKey(const TString& yson)
    {
        return NTableClient::YsonToKey(yson);
    }


    bool AreRowsEqual(TUnversionedRow row, const TString& yson)
    {
        return AreRowsEqual(row, yson.c_str());
    }

    bool AreRowsEqual(TUnversionedRow row, const char* yson)
    {
        return AreRowsEqual(row, yson, NameTable_);
    }

    bool AreQueryRowsEqual(TUnversionedRow row, const TString& yson)
    {
        return AreQueryRowsEqual(row, yson.c_str());
    }

    bool AreQueryRowsEqual(TUnversionedRow row, const char* yson)
    {
        return AreRowsEqual(row, yson, QueryNameTable_);
    }

    static bool AreRowsEqual(TUnversionedRow row, const char* yson, const TNameTablePtr& nameTable)
    {
        if (!row && !yson) {
            return true;
        }

        if (!row || !yson) {
            return false;
        }

        auto expectedRowParts = ConvertTo<THashMap<TString, INodePtr>>(
            TYsonString(TString(yson), EYsonType::MapFragment));

        for (int index = 0; index < static_cast<int>(row.GetCount()); ++index) {
            const auto& value = row[index];
            const auto& name = nameTable->GetName(value.Id);
            auto it = expectedRowParts.find(name);
            switch (value.Type) {
                case EValueType::Int64:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<i64>() != value.Data.Int64) {
                        return false;
                    }
                    break;

                case EValueType::Uint64:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<ui64>() != value.Data.Uint64) {
                        return false;
                    }
                    break;

                case EValueType::Double:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<double>() != value.Data.Double) {
                        return false;
                    }
                    break;

                case EValueType::String:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<TString>() != TString(value.Data.String, value.Length)) {
                        return false;
                    }
                    break;

                case EValueType::Null:
                    if (it != expectedRowParts.end()) {
                        return false;
                    }
                    break;

                default:
                    YT_ABORT();
            }
        }

        return true;
    }


    using TStoreSnapshot = std::pair<TString, TCallback<void(TSaveContext&)>>;

    TStoreSnapshot BeginReserializeStore()
    {
        auto store = GetDynamicStore();

        TString buffer;

        TStringOutput output(buffer);
        TSaveContext saveContext;
        saveContext.SetOutput(&output);
        store->Save(saveContext);

        return std::make_pair(buffer, store->AsyncSave());
    }

    void EndReserializeStore(const TStoreSnapshot& snapshot)
    {
        auto store = GetDynamicStore();
        auto buffer = snapshot.first;

        TStringOutput output(buffer);
        TSaveContext saveContext;
        saveContext.SetOutput(&output);
        snapshot.second.Run(saveContext);

        TStringInput input(buffer);
        TLoadContext loadContext;
        loadContext.SetInput(&input);

        CreateDynamicStore();
        store = GetDynamicStore();
        store->Load(loadContext);
        store->AsyncLoad(loadContext);
    }

    void ReserializeStore()
    {
        EndReserializeStore(BeginReserializeStore());
    }



    const IColumnEvaluatorCachePtr ColumnEvaluatorCache_ = CreateColumnEvaluatorCache(
        New<TColumnEvaluatorCacheConfig>());

    const NTabletNode::IRowComparerProviderPtr RowComparerProvider_ = CreateRowComparerProvider(New<TSlruCacheConfig>());

    TNameTablePtr NameTable_;
    TNameTablePtr QueryNameTable_;
    std::unique_ptr<TTablet> Tablet_;
    TTimestamp CurrentTimestamp_ = 10000; // some reasonable starting point
    NChunkClient::TClientChunkReadOptions ChunkReadOptions_;
};

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
class TStoreManagerTestBase
    : public TBase
{
protected:
    virtual IStoreManagerPtr GetStoreManager() = 0;

    virtual void SetupTablet() override
    {
        auto storeManager = GetStoreManager();
        storeManager->StartEpoch(nullptr);
        storeManager->Mount({}, {}, true, NProto::TMountHint{});
    }

    void RotateStores()
    {
        auto storeManager = GetStoreManager();
        storeManager->ScheduleRotation();
        storeManager->Rotate(true);
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTabletNode

