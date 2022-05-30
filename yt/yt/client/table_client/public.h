#pragma once

#include <yt/yt/client/chunk_client/public.h>

#include <yt/yt/client/cypress_client/public.h>

#include <yt/yt/client/transaction_client/public.h>

#include <yt/yt/core/misc/enum.h>
#include <yt/yt/core/misc/public.h>

#include <initializer_list>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TNameTableExt;
class TLogicalType;
class TColumnSchema;
class TTableSchemaExt;
class TKeyColumnsExt;
class TSortColumnsExt;
class TBoundaryKeysExt;
class TBlockIndexesExt;
class TDataBlockMetaExt;
class TColumnarStatisticsExt;
class TDataBlockMeta;
class TSimpleVersionedBlockMeta;
class TSchemaDictionary;
class TColumnFilter;
class TReqLookupRows;
class TColumnRenameDescriptor;
class THunkChunkRef;
class TColumnMetaExt;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using TRefCountedDataBlockMeta = TRefCountedProto<NProto::TDataBlockMetaExt>;
using TRefCountedDataBlockMetaPtr = TIntrusivePtr<TRefCountedDataBlockMeta>;

////////////////////////////////////////////////////////////////////////////////

using TRefCountedColumnMeta = TRefCountedProto<NProto::TColumnMetaExt>;
using TRefCountedColumnMetaPtr = TIntrusivePtr<TRefCountedColumnMeta>;

////////////////////////////////////////////////////////////////////////////////

using NTransactionClient::TTimestamp;
using NTransactionClient::NullTimestamp;
using NTransactionClient::MinTimestamp;
using NTransactionClient::MaxTimestamp;
using NTransactionClient::SyncLastCommittedTimestamp;
using NTransactionClient::AsyncLastCommittedTimestamp;
using NTransactionClient::AllCommittedTimestamp;
using NTransactionClient::NotPreparedTimestamp;

using TKeyColumns = std::vector<TString>;

////////////////////////////////////////////////////////////////////////////////

// Keep values below consistent with https://wiki.yandex-team.ru/yt/userdoc/tables.
constexpr int MaxKeyColumnCount = 256;
constexpr int TypicalColumnCount = 64;
constexpr int MaxColumnLockCount = 32;
constexpr int MaxColumnNameLength = 256;
constexpr int MaxColumnLockLength = 256;
constexpr int MaxColumnGroupLength = 256;

// Only for dynamic tables.
constexpr int MaxValuesPerRow = 1024;
constexpr int MaxRowsPerRowset = 5 * 1024 * 1024;
constexpr i64 MaxStringValueLength = 16_MB;
constexpr i64 MaxAnyValueLength = 16_MB;
constexpr i64 MaxCompositeValueLength = 16_MB;
constexpr i64 MaxServerVersionedRowDataWeight = 512_MB;
constexpr i64 MaxClientVersionedRowDataWeight = 128_MB;
constexpr int MaxKeyColumnCountInDynamicTable = 32;
constexpr int MaxTimestampCountPerRow = std::numeric_limits<ui16>::max();

static_assert(
    MaxTimestampCountPerRow <= std::numeric_limits<ui16>::max(),
    "Max timestamp count cannot be larger than UINT16_MAX");

// Only for static tables.
constexpr i64 MaxRowWeightLimit = 128_MB;
constexpr i64 MaxKeyWeightLimit = 256_KB;

// NB(psushin): increasing this parameter requires rewriting all chunks,
// so one probably should never want to do it.
constexpr int MaxSampleSize = 64_KB;

// This is a hard limit for static tables,
// imposed by Id field size (16-bit) in TUnversionedValue.
constexpr int MaxColumnId = 32 * 1024;

constexpr int MaxSchemaTotalTypeComplexity = MaxColumnId;
constexpr int MaxSchemaDepth = 32;

extern const TString SystemColumnNamePrefix;
extern const TString TableIndexColumnName;
extern const TString RowIndexColumnName;
extern const TString RangeIndexColumnName;
extern const TString TabletIndexColumnName;
extern const TString TimestampColumnName;
extern const TString CumulativeDataWeightColumnName;
extern const TString PrimaryLockName;

constexpr int TypicalHunkColumnCount = 8;

////////////////////////////////////////////////////////////////////////////////

// Do not change these values since they are stored in the master snapshot.
DEFINE_ENUM(ETableSchemaMode,
    ((Weak)      (0))
    ((Strong)    (1))
);

DEFINE_ENUM(EOptimizeFor,
    ((Lookup)  (0))
    ((Scan)    (1))
);

YT_DEFINE_ERROR_ENUM(
    ((SortOrderViolation)         (301))
    ((InvalidDoubleValue)         (302))
    ((IncomparableType)           (303))
    ((UnhashableType)             (304))
    // E.g. name table with more than #MaxColumnId columns (may come from legacy chunks).
    ((CorruptedNameTable)         (305))
    ((UniqueKeyViolation)         (306))
    ((SchemaViolation)            (307))
    ((RowWeightLimitExceeded)     (308))
    ((InvalidColumnFilter)        (309))
    ((InvalidColumnRenaming)      (310))
    ((IncompatibleKeyColumns)     (311))
    ((ReaderDeadlineExpired)      (312))
    ((TimestampOutOfRange)        (313))
    ((InvalidSchemaValue)         (314))
    ((FormatCannotRepresentRow)   (315))
    ((IncompatibleSchemas)        (316))
    ((InvalidPartitionedBy)       (317))
    ((MisconfiguredPartitions)    (318))
);

DEFINE_ENUM(EControlAttribute,
    (TableIndex)
    (KeySwitch)
    (RangeIndex)
    (RowIndex)
    (TabletIndex)
    (EndOfStream)
);

DEFINE_ENUM(EUnavailableChunkStrategy,
    ((ThrowError)   (0))
    ((Restore)      (1))
    ((Skip)         (2))
);

DEFINE_ENUM(ETableSchemaModification,
    ((None)                         (0))
    ((UnversionedUpdate)            (1))
    ((UnversionedUpdateUnsorted)    (2))
);

DEFINE_ENUM(EColumnarStatisticsFetcherMode,
    ((FromNodes)             (0))
    ((FromMaster)            (1))
    ((Fallback)              (2))
);

DEFINE_ENUM(EMisconfiguredPartitionTactics,
    ((Fail)     (0))
    ((Skip)     (1))
);

using TTableId = NCypressClient::TNodeId;
using TTableCollocationId = NObjectClient::TObjectId;

//! NB: |int| is important since we use negative values to indicate that
//! certain values need to be dropped. Cf. #TRowBuffer::CaptureAndPermuteRow.
using TNameTableToSchemaIdMapping = TCompactVector<int, TypicalColumnCount>;

using TIdMapping = TCompactVector<int, TypicalColumnCount>;

using THunkColumnIds = TCompactVector<int, TypicalHunkColumnCount>;

union TUnversionedValueData;

enum class ESortOrder;
enum class EValueType : ui8;
enum class ESimpleLogicalValueType : ui32;
enum class ELogicalMetatype;

using TKeyColumnTypes = TCompactVector<EValueType, 16>;

class TColumnFilter;

struct TUnversionedValue;
struct TVersionedValue;

class TUnversionedOwningValue;

struct TUnversionedRowHeader;
struct TVersionedRowHeader;

class TUnversionedRow;
class TMutableUnversionedRow;
class TUnversionedOwningRow;

class TVersionedRow;
class TMutableVersionedRow;
class TVersionedOwningRow;

class TKey;

using TLegacyKey = TUnversionedRow;
using TLegacyMutableKey = TMutableUnversionedRow;
using TLegacyOwningKey = TUnversionedOwningRow;
using TRowRange = std::pair<TUnversionedRow, TUnversionedRow>;

class TUnversionedRowBuilder;
class TUnversionedOwningRowBuilder;

struct TTypeErasedRow;

class TKeyBound;
class TOwningKeyBound;

class TKeyComparer;

struct TColumnRenameDescriptor;
using TColumnRenameDescriptors = std::vector<TColumnRenameDescriptor>;

class TStableName;

class TColumnSchema;

struct TColumnSortSchema;
using TSortColumns = std::vector<TColumnSortSchema>;

class TTableSchema;
using TTableSchemaPtr = TIntrusivePtr<TTableSchema>;

class TLegacyLockMask;
using TLegacyLockBitmap = ui64;

class TLockMask;
using TLockBitmap = TCompactVector<ui64, 1>;

class TComparator;

DECLARE_REFCOUNTED_CLASS(TNameTable)
class TNameTableReader;
class TNameTableWriter;

DECLARE_REFCOUNTED_CLASS(TRowBuffer)

DECLARE_REFCOUNTED_STRUCT(ISchemalessUnversionedReader)
DECLARE_REFCOUNTED_STRUCT(ISchemafulUnversionedReader)
DECLARE_REFCOUNTED_STRUCT(IUnversionedWriter)
DECLARE_REFCOUNTED_STRUCT(IUnversionedRowsetWriter)

using TSchemalessWriterFactory = std::function<IUnversionedRowsetWriterPtr(
    TNameTablePtr,
    TTableSchemaPtr)>;

DECLARE_REFCOUNTED_STRUCT(IVersionedReader)
DECLARE_REFCOUNTED_STRUCT(IVersionedWriter)

DECLARE_REFCOUNTED_CLASS(TChunkWriterTestingOptions)

DECLARE_REFCOUNTED_CLASS(TChunkReaderConfig)
DECLARE_REFCOUNTED_CLASS(TChunkWriterConfig)

DECLARE_REFCOUNTED_CLASS(TTableReaderConfig)
DECLARE_REFCOUNTED_CLASS(TTableWriterConfig)

DECLARE_REFCOUNTED_CLASS(TRetentionConfig)

DECLARE_REFCOUNTED_CLASS(TTypeConversionConfig)
DECLARE_REFCOUNTED_CLASS(TInsertRowsFormatConfig)

DECLARE_REFCOUNTED_CLASS(TChunkReaderOptions)
DECLARE_REFCOUNTED_CLASS(TChunkWriterOptions)

class TSaveContext;
class TLoadContext;
using TPersistenceContext = TCustomPersistenceContext<TSaveContext, TLoadContext>;

struct IWireProtocolReader;
struct IWireProtocolWriter;

using TSchemaData = std::vector<ui32>;

DECLARE_REFCOUNTED_STRUCT(IWireProtocolRowsetReader)
DECLARE_REFCOUNTED_STRUCT(IWireProtocolRowsetWriter)

DECLARE_REFCOUNTED_STRUCT(IUnversionedRowBatch)
DECLARE_REFCOUNTED_STRUCT(IUnversionedColumnarRowBatch)
DECLARE_REFCOUNTED_STRUCT(IVersionedRowBatch)

struct IValueConsumer;
struct IFlushableValueConsumer;

class TComplexTypeFieldDescriptor;

DECLARE_REFCOUNTED_CLASS(TLogicalType)
class TSimpleLogicalType;
class TDecimalLogicalType;
class TOptionalLogicalType;
class TListLogicalType;
class TStructLogicalType;
class TTupleLogicalType;
class TVariantTupleLogicalType;
class TVariantStructLogicalType;
class TDictLogicalType;
class TTaggedLogicalType;

struct TStructField;

//
// Enumeration is used to describe compatibility of two schemas (or logical types).
// Such compatibility tests are performed before altering table schema or before merge operation.
DEFINE_ENUM(ESchemaCompatibility,
    // Values are incompatible.
    // E.g. Int8 and String.
    (Incompatible)

    // Values that satisfy old schema MIGHT satisfy new schema, dynamic check is required.
    // E.g. Optional<Int8> and Int8, in this case we must check that value of old type is not NULL.
    (RequireValidation)

    // Values that satisfy old schema ALWAYS satisfy new schema.
    // E.g. Int32 and Int64
    (FullyCompatible)
)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
