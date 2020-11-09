#include "comparator.h"

#include "key_bound.h"
#include "serialize.h"

#include <yt/core/logging/log.h>

namespace NYT::NTableClient {

using namespace NLogging;

//! Used only for YT_LOG_FATAL below.
static const TLogger Logger("TableClientComparator");

////////////////////////////////////////////////////////////////////////////////

TComparator::TComparator(std::vector<ESortOrder> sortOrders)
    : SortOrders_(std::move(sortOrders))
{ }

void TComparator::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, SortOrders_);
}

int TComparator::GetLength() const
{
    return SortOrders_.size();
}

void TComparator::ValidateKey(const TKey& key) const
{
    YT_LOG_FATAL_IF(
        key.GetCount() != GetLength(),
        "Comparator is used with key of different length (Key: %v, Comparator: %v)",
        key.AsRow(),
        *this);
}

void TComparator::ValidateKeyBound(const TKeyBound& keyBound) const
{
    YT_LOG_FATAL_IF(
        keyBound.Prefix.GetCount() > GetLength(),
        "Comparator is used with longer key bound (KeyBound: %v, Comparator: %v)",
        keyBound,
        *this);
}

int TComparator::CompareValues(int /* index */, const TUnversionedValue& lhs, const TUnversionedValue& rhs) const
{
    int valueComparisonResult = CompareRowValues(lhs, rhs);
    // TODO(max42): if sort order == descending, valueComparisonResult = -valueComparisonResult.
    return valueComparisonResult;
}

TKeyBound TComparator::StrongerKeyBound(const TKeyBound& lhs, const TKeyBound& rhs) const
{
    YT_VERIFY(lhs.IsUpper == rhs.IsUpper);
    auto comparisonResult = CompareKeyBounds(lhs, rhs);
    if (lhs.IsUpper) {
        comparisonResult = -comparisonResult;
    }

    return (comparisonResult <= 0) ? rhs : lhs;
}

bool TComparator::IsRangeEmpty(const TKeyBound& lowerBound, const TKeyBound& upperBound) const
{
    YT_VERIFY(!lowerBound.IsUpper);
    YT_VERIFY(upperBound.IsUpper);
    return CompareKeyBounds(lowerBound, upperBound, /* lowerVsUpper */ 1) >= 0;
}

bool TComparator::TestKey(const TKey& key, const TKeyBound& keyBound) const
{
    ValidateKey(key);
    ValidateKeyBound(keyBound);

    int comparisonResult = 0;

    for (int index = 0; index < keyBound.Prefix.GetCount(); ++index) {
        const auto& keyValue = key[index];
        const auto& keyBoundValue = keyBound.Prefix[index];
        comparisonResult = CompareValues(index, keyValue, keyBoundValue);
        if (comparisonResult != 0) {
            break;
        }
    }

    if (keyBound.IsUpper) {
        comparisonResult = -comparisonResult;
    }

    // Now:
    // - comparisonResult > 0 means that key is strictly inside ray (i.e. test is positive);
    // - comparisonResult == 0 means that key starts with key bound prefix (i.e. we should consider inclusiveness);
    // - comparisonResult < 0 means that key is strictly outside ray (i.e. test is negative).

    return comparisonResult > 0 || (comparisonResult == 0 && keyBound.IsInclusive);
}

int TComparator::CompareKeyBounds(const TKeyBound& lhs, const TKeyBound& rhs, int lowerVsUpper) const
{
    ValidateKeyBound(lhs);
    ValidateKeyBound(rhs);

    int comparisonResult = 0;

    // In case when one key bound is a proper prefix of another, points to the shorter one.
    const TKeyBound* shorter = nullptr;

    for (int index = 0; ; ++index) {
        if (index >= lhs.Prefix.GetCount() && index >= rhs.Prefix.GetCount()) {
            // Prefixes coincide. Check if key bounds are indeed at the same point.
            {
                auto lhsInclusivenessAsUpper = (lhs.IsUpper && lhs.IsInclusive) || (!lhs.IsUpper && !lhs.IsInclusive);
                auto rhsInclusivenessAsUpper = (rhs.IsUpper && rhs.IsInclusive) || (!rhs.IsUpper && !rhs.IsInclusive);
                if (lhsInclusivenessAsUpper != rhsInclusivenessAsUpper) {
                    return lhsInclusivenessAsUpper - rhsInclusivenessAsUpper;
                }
            }

            // Ok, they are indeed at the same point. How do we break ties?
            if (lowerVsUpper == 0) {
                // We are asked not to break ties.
                return 0;
            }

            // Break ties using #upperFirst.
            comparisonResult = lhs.IsUpper - rhs.IsUpper;

            if (lowerVsUpper > 0) {
                comparisonResult = -comparisonResult;
            }
            return comparisonResult;
        } else if (index >= lhs.Prefix.GetCount()) {
            shorter = &lhs;
            break;
        } else if (index >= rhs.Prefix.GetCount()) {
            shorter = &rhs;
            break;
        } else {
            const auto& lhsValue = lhs.Prefix[index];
            const auto& rhsValue = rhs.Prefix[index];
            comparisonResult = CompareValues(index, lhsValue, rhsValue);
            if (comparisonResult != 0) {
                return comparisonResult;
            }
        }
    }
    YT_VERIFY(shorter);

    // By this moment, longer operand is strictly between shorter operand and toggleInclusiveness(shorter operand).
    // Thus we have to check if shorter operand is "largest" among itself and its toggleInclusiveness counterpart.
    if ((shorter->IsUpper && shorter->IsInclusive) || (!shorter->IsUpper && !shorter->IsInclusive)) {
        comparisonResult = -1;
    } else {
        comparisonResult = 1;
    }

    // By now comparisonResult expresses if longer < shorter. Now check which hand is actually shorter.
    if (shorter == &lhs) {
        comparisonResult = -comparisonResult;
    }

    return comparisonResult;
}

int TComparator::CompareKeys(const TKey& lhs, const TKey& rhs) const
{
    ValidateKey(lhs);
    ValidateKey(rhs);

    for (int index = 0; index < lhs.GetCount(); ++index) {
        auto valueComparisonResult = CompareValues(index, lhs[index], rhs[index]);
        if (valueComparisonResult != 0) {
            return valueComparisonResult;
        }
    }

    return 0;
}

void FormatValue(TStringBuilderBase* builder, const TComparator& comparator, TStringBuf /* spec */)
{
    builder->AppendFormat("{Length: %v}", comparator.GetLength());
}

TString ToString(const TComparator& comparator)
{
    return ToStringViaBuilder(comparator);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
