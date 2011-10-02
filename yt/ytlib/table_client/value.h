﻿#pragma once

#include "../misc/common.h"
#include "../misc/ptr.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Array of bytes. The only data type stored inside tables (column names and values)
class TValue
{
public:
    TValue();
    TValue(const TSharedRef& data);
    TValue(TBlob& data);

    const char* GetData() const;
    size_t GetSize() const;

    const char* Begin() const;
    const char* End() const;

    bool IsEmpty() const;
    bool IsNull() const;

    Stroka ToString() const;
    TBlob ToBlob() const;

    static TValue Null();

private:
    TSharedRef Data;
};

bool operator==(const TValue& lhs, const TValue& rhs);
bool operator!=(const TValue& lhs, const TValue& rhs);
bool operator< (const TValue& lhs, const TValue& rhs);
bool operator> (const TValue& lhs, const TValue& rhs);
bool operator<=(const TValue& lhs, const TValue& rhs);
bool operator>=(const TValue& lhs, const TValue& rhs);

// construct from POD types
template <class T>
TValue Value(const T& data)
{
    TBlob blob(&data, &data + sizeof(T));
    return TValue(blob);
}

template <>
TValue Value(const Stroka& data)
{
    TBlob blob(~data, ~data + data.Size());
    return TValue(blob);
}

TValue NextValue(const TValue& value);

////////////////////////////////////////////////////////////////////////////////

typedef yhash_map<TValue, TValue> TTableRow;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

template<>
struct THash<NYT::TValue>
{
    size_t operator()(const NYT::TValue& value) const
    {
        return MurmurHash<ui32>(value.GetData(), value.GetSize());
    }
};

