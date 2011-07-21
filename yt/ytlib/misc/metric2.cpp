#include "metric2.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TMetric2::TMetric2(double minValue, double maxValue, int bucketCount)
    : MinValue(minValue)
    , MaxValue(maxValue)
    , BucketCount(bucketCount)
    , ValueCount(0)
    , Sum(0)
    , SumSquares(0)
    , MinBucket(0)
    , MaxBucket(0)
    , Buckets(bucketCount)
{
    Delta = (MaxValue - MinValue) / BucketCount;
}

void TMetric2::AddValue(double value)
{
    Sum += value;
    SumSquares += value * value;
    ValueCount++;

    IncrementBuckets(value);
}

double TMetric2::GetMean() const
{
    if (ValueCount == 0) {
        return 0;
    }
    return Sum / ValueCount;
}

double TMetric2::GetStd() const
{
    if (ValueCount == 0) {
        return 0;
    }
    double mean = GetMean();
    return sqrt(abs(SumSquares / ValueCount - mean * mean));
}

Stroka TMetric2::GetDebugInfo() const
{
    return Sprintf("Mean: %lf, Std: %lf", GetMean(), GetStd());
}

void TMetric2::IncrementBuckets(double value)
{
    if (value < MinValue) {
        MinBucket += 1;
    } else if (value > MaxValue) {
        MaxBucket += 1;
    } else {
        int BucketId = (value - MinValue) / Delta;

        // in case of roundings errors
        if (BucketId < 0) BucketId = 0;
        if (BucketId >= BucketCount) BucketId = BucketCount - 1;

        Buckets[BucketId] += 1;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
