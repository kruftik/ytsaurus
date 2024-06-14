#include <yt/cpp/mapreduce/tests/yt_unittest_lib/yt_unittest_lib.h>

#include <yt/cpp/mapreduce/tests/native/proto_lib/all_types.pb.h>
#include <yt/cpp/mapreduce/tests/native/proto_lib/all_types_proto3.pb.h>
#include <yt/cpp/mapreduce/tests/native/proto_lib/clashing_enums.pb.h>
#include <yt/cpp/mapreduce/tests/native/proto_lib/row.pb.h>

#include <yt/cpp/mapreduce/interface/config.h>

#include <yt/cpp/mapreduce/interface/errors.h>
#include <yt/cpp/mapreduce/interface/io.h>
#include <yt/cpp/mapreduce/interface/serialize.h>

#include <yt/cpp/mapreduce/http/abortable_http_response.h>

#include <yt/cpp/mapreduce/io/proto_table_reader.h>

#include <library/cpp/testing/gtest/gtest.h>

#include <library/cpp/yson/node/node_io.h>

#include <util/generic/scope.h>

#include <type_traits>

using namespace NYT;
using namespace NYT::NTesting;

template <typename TRow>
static NTi::TTypePtr GetRowType();

template <>
NTi::TTypePtr GetRowType<TUrlRow>()
{
    static auto type = NTi::Struct({
        {"Host", NTi::String()},
        {"Path", NTi::String()},
        {"HttpCode", NTi::Int32()},
    });
    return type;
}

template <>
NTi::TTypePtr GetRowType<TUrlRowWithColumnNames>()
{
    static auto type = NTi::Struct({
        {"Host_ColumnName", NTi::String()},
        {"Path_KeyColumnName", NTi::String()},
        {"HttpCode", NTi::Int32()},
    });
    return type;
}

template <>
void Out<TRowWithTypeOptions_Color>(IOutputStream& o, TRowWithTypeOptions_Color color)
{
    o << TRowWithTypeOptions::Color_Name(color);
}

class TProtobufTableIoTest
    : public ::testing::TestWithParam<bool>
{ };

TEST_P(TProtobufTableIoTest, ReadingWritingProtobufAllTypesProto3)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    auto path = TRichYPath(workingDir + "/proto_table");
    TAllTypesMessageProto3 message;
    message.SetDoubleField(42.4242);
    message.SetFloatField(3.14159);
    message.SetInt64Field(-4200);
    // OmittedInt64Field is not set deliberately.
    message.SetUint64Field(4200);
    message.SetSint64Field(-4242);
    message.SetFixed64Field(432101234);
    message.SetSfixed64Field(41112222);
    message.SetInt32Field(-3124232);
    message.SetUint32Field(12321342);
    message.SetSint32Field(-42442);
    message.SetFixed32Field(2134242);
    message.SetSfixed32Field(422142);
    message.SetBoolField(true);
    message.SetStringField("42");
    message.SetBytesField("36 popugayev");
    message.SetEnumField(EEnumProto3::OneProto3);
    message.MutableMessageField()->SetKey("key");
    message.MutableMessageField()->SetValue("value");

    {
        auto writer = client->CreateTableWriter<TAllTypesMessageProto3>(path);
        writer->AddRow(message);
        writer->Finish();
    }
    {
        auto reader = client->CreateTableReader<TAllTypesMessageProto3>(path);
        EXPECT_TRUE(reader->IsValid());
        const auto& row = reader->GetRow();
        ASSERT_NEAR(message.GetDoubleField(), row.GetDoubleField(), 1e-6);
        ASSERT_NEAR(message.GetFloatField(), row.GetFloatField(), 1e-6);
        EXPECT_EQ(message.GetInt64Field(), row.GetInt64Field());
        EXPECT_EQ(message.GetOmittedInt64Field(), row.GetOmittedInt64Field());
        EXPECT_EQ(message.GetOmittedInt64Field(), 0);
        EXPECT_EQ(message.GetUint64Field(), row.GetUint64Field());
        EXPECT_EQ(message.GetSint64Field(), row.GetSint64Field());
        EXPECT_EQ(message.GetFixed64Field(), row.GetFixed64Field());
        EXPECT_EQ(message.GetSfixed64Field(), row.GetSfixed64Field());
        EXPECT_EQ(message.GetInt32Field(), row.GetInt32Field());
        EXPECT_EQ(message.GetUint32Field(), row.GetUint32Field());
        EXPECT_EQ(message.GetSint32Field(), row.GetSint32Field());
        EXPECT_EQ(message.GetFixed32Field(), row.GetFixed32Field());
        EXPECT_EQ(message.GetSfixed32Field(), row.GetSfixed32Field());
        EXPECT_EQ(message.GetBoolField(), row.GetBoolField());
        EXPECT_EQ(message.GetStringField(), row.GetStringField());
        EXPECT_EQ(message.GetBytesField(), row.GetBytesField());
        EXPECT_EQ(message.GetEnumField(), row.GetEnumField());
        EXPECT_EQ(message.GetMessageField().GetKey(), row.GetMessageField().GetKey());
        EXPECT_EQ(message.GetMessageField().GetValue(), row.GetMessageField().GetValue());
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }
}

TEST_P(TProtobufTableIoTest, ReadingWritingProtobufAllTypes)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    auto path = TRichYPath(workingDir + "/proto_table");
    TAllTypesMessage message;
    message.SetDoubleField(42.4242);
    message.SetFloatField(3.14159);
    message.SetInt64Field(-4200);
    message.SetUint64Field(4200);
    message.SetSint64Field(-4242);
    message.SetFixed64Field(432101234);
    message.SetSfixed64Field(41112222);
    message.SetInt32Field(-3124232);
    message.SetUint32Field(12321342);
    message.SetSint32Field(-42442);
    message.SetFixed32Field(2134242);
    message.SetSfixed32Field(422142);
    message.SetBoolField(true);
    message.SetStringField("42");
    message.SetBytesField("36 popugayev");
    message.SetEnumField(EEnum::One);
    message.MutableMessageField()->SetKey("key");
    message.MutableMessageField()->SetValue("value");

    {
        auto writer = client->CreateTableWriter<TAllTypesMessage>(path);
        writer->AddRow(message);
        writer->Finish();
    }
    {
        auto reader = client->CreateTableReader<TAllTypesMessage>(path);
        EXPECT_TRUE(reader->IsValid());
        const auto& row = reader->GetRow();
        ASSERT_NEAR(message.GetDoubleField(), row.GetDoubleField(), 1e-6);
        ASSERT_NEAR(message.GetFloatField(), row.GetFloatField(), 1e-6);
        EXPECT_EQ(message.GetInt64Field(), row.GetInt64Field());
        EXPECT_EQ(message.GetUint64Field(), row.GetUint64Field());
        EXPECT_EQ(message.GetSint64Field(), row.GetSint64Field());
        EXPECT_EQ(message.GetFixed64Field(), row.GetFixed64Field());
        EXPECT_EQ(message.GetSfixed64Field(), row.GetSfixed64Field());
        EXPECT_EQ(message.GetInt32Field(), row.GetInt32Field());
        EXPECT_EQ(message.GetUint32Field(), row.GetUint32Field());
        EXPECT_EQ(message.GetSint32Field(), row.GetSint32Field());
        EXPECT_EQ(message.GetFixed32Field(), row.GetFixed32Field());
        EXPECT_EQ(message.GetSfixed32Field(), row.GetSfixed32Field());
        EXPECT_EQ(message.GetBoolField(), row.GetBoolField());
        EXPECT_EQ(message.GetStringField(), row.GetStringField());
        EXPECT_EQ(message.GetBytesField(), row.GetBytesField());
        EXPECT_EQ(message.GetEnumField(), row.GetEnumField());
        EXPECT_EQ(message.GetMessageField().GetKey(), row.GetMessageField().GetKey());
        EXPECT_EQ(message.GetMessageField().GetValue(), row.GetMessageField().GetValue());
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }
}

TEST_P(TProtobufTableIoTest, UntypedProtobufWriter)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    {
        TUrlRow row;
        row.SetHost("http://www.example.com");
        row.SetPath("/index.php");
        row.SetHttpCode(200);
        const Message* ptrWithoutType = &row;

        auto writer = client->CreateTableWriter(workingDir + "/urls", *TUrlRow::descriptor());
        writer->AddRow(*ptrWithoutType);
        writer->Finish();
    }

    auto reader = client->CreateTableReader<TUrlRow>(workingDir + "/urls");
    EXPECT_TRUE(reader->IsValid());
    {
        const auto& row = reader->GetRow();
        EXPECT_EQ(row.GetHost(), "http://www.example.com");
        EXPECT_EQ(row.GetPath(), "/index.php");
        EXPECT_EQ(row.GetHttpCode(), 200);
    }
    reader->Next();
    EXPECT_TRUE(!reader->IsValid());
}

TEST_P(TProtobufTableIoTest, ProtobufVersions)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    {
        const auto writer = client->CreateTableWriter<TRowVer1>(workingDir + "/ver1");
        TRowVer1 data;
        data.SetString_1("Ver1_String_1");
        data.SetUint32_2(0x12);
        writer->AddRow(data);
        writer->Finish();
    }

    //V1 as V2
    {
        const auto reader = client->CreateTableReader<TRowVer2>(workingDir + "/ver1");
        EXPECT_TRUE(reader->IsValid());
        {
            const auto& data = reader->GetRow();
            EXPECT_TRUE(data.HasString_1());
            EXPECT_EQ(data.GetString_1(), "Ver1_String_1");
            EXPECT_TRUE(data.HasUint32_2());
            EXPECT_EQ(data.GetUint32_2(), static_cast<ui32>(0x12));
            EXPECT_TRUE(!data.HasFixed64_3());
            EXPECT_EQ(data.unknown_fields().field_count(), 0);
        }
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }

    {
        const auto writer = client->CreateTableWriter<TRowVer2>(workingDir + "/ver2");
        TRowVer2 data;
        data.SetString_1("Ver2_String_1");
        data.SetUint32_2(0x22);
        data.SetFixed64_3(0x23);
        writer->AddRow(data);
        writer->Finish();
    }

    //V2 as V1
    {
        const auto reader = client->CreateTableReader<TRowVer1>(workingDir + "/ver2");
        EXPECT_TRUE(reader->IsValid());
        {
            const auto& data = reader->GetRow();
            EXPECT_TRUE(data.HasString_1());
            EXPECT_EQ(data.GetString_1(), "Ver2_String_1");
            EXPECT_TRUE(data.HasUint32_2());
            EXPECT_EQ(data.GetUint32_2(), static_cast<ui32>(0x22));
            //no unknown fields supported
            EXPECT_EQ(data.unknown_fields().field_count(), 0);
        }
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }
}

struct TTestColumnNames
{
    TString Host;
    TString Path;
    TString HttpCode;
};

template <typename TRow, typename TElement>
void TestProtobufSerializationModes(
    EWrapperFieldFlag::Enum mode1,
    EWrapperFieldFlag::Enum mode2,
    bool WithDescriptors,
    const TTestColumnNames& columnNames = {"Host", "Path", "HttpCode"})
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = WithDescriptors;

    auto setType = [] (EWrapperFieldFlag::Enum mode, TColumnSchema& column) {
        switch (mode) {
            case EWrapperFieldFlag::SERIALIZATION_YT:
                column.Type(GetRowType<TElement>());
                return;
            case EWrapperFieldFlag::SERIALIZATION_PROTOBUF:
                column.Type(EValueType::VT_STRING);
                return;
            default:
                Y_ABORT();
        }
    };

    auto schema = TTableSchema();
    auto column1 = TColumnSchema().Name("UrlRow_1");
    setType(mode1, column1);
    auto column2 = TColumnSchema().Name("UrlRow_2");
    setType(mode2, column2);
    schema.AddColumn(column1).AddColumn(column2);

    TVector<std::tuple<TString, TString, int>> firstValuesInRows{
        {"http://www.example.com", "/", 302},
        {"http://www.example.com", "/index.php", 200},
    };

    TVector<TVector<TString>> protobufSerializedValues;
    TVector<TVector<TNode>> nodeSerializedValues;
    {
        auto writer = client->CreateTableWriter<TRow>(
            TRichYPath(workingDir + "/table").Schema(schema));
        for (const auto& [host, path, code] : firstValuesInRows) {
            TRow row;
            auto& embedded1 = *row.MutableUrlRow_1();
            embedded1.SetHost(host);
            embedded1.SetPath(path);
            embedded1.SetHttpCode(code);
            auto& embedded2 = *row.MutableUrlRow_2();
            embedded2.SetHost(host);
            embedded2.SetPath(path);
            embedded2.SetHttpCode(code + 1);
            writer->AddRow(row);

            TString embedded1Serialized;
            Y_PROTOBUF_SUPPRESS_NODISCARD embedded1.SerializeToString(&embedded1Serialized);
            TString embedded2Serialized;
            Y_PROTOBUF_SUPPRESS_NODISCARD embedded2.SerializeToString(&embedded2Serialized);
            protobufSerializedValues.push_back(TVector<TString>{embedded1Serialized, embedded2Serialized});

            nodeSerializedValues.push_back(TVector<TNode>{
                TNode()(columnNames.Host, host)(columnNames.Path, path)(columnNames.HttpCode, code),
                TNode()(columnNames.Host, host)(columnNames.Path, path)(columnNames.HttpCode, code + 1),
            });
        }
        writer->Finish();
    }

    auto checkValue = [&] (const TNode& value, EWrapperFieldFlag::Enum mode, int rowIndex, int valueIndex) {
        switch (mode) {
            case EWrapperFieldFlag::SERIALIZATION_YT:
                EXPECT_EQ(value, nodeSerializedValues[rowIndex][valueIndex]);
                return;
            case EWrapperFieldFlag::SERIALIZATION_PROTOBUF:
                EXPECT_EQ(value.GetType(), TNode::EType::String);
                EXPECT_EQ(value.AsString(), protobufSerializedValues[rowIndex][valueIndex]);
                return;
            default:
                Y_ABORT();
        }
    };

    // Check that all the values were written correctly.
    {
        auto reader = client->CreateTableReader<TNode>(workingDir + "/table");
        EXPECT_TRUE(reader->IsValid());
        EXPECT_TRUE(reader->GetRow().HasKey("UrlRow_1"));
        EXPECT_TRUE(reader->GetRow().HasKey("UrlRow_2"));
        checkValue(reader->GetRow()["UrlRow_1"], mode1, 0, 0);
        checkValue(reader->GetRow()["UrlRow_2"], mode2, 0, 1);
        reader->Next();
        EXPECT_TRUE(reader->IsValid());
        EXPECT_TRUE(reader->GetRow().HasKey("UrlRow_1"));
        EXPECT_TRUE(reader->GetRow().HasKey("UrlRow_2"));
        checkValue(reader->GetRow()["UrlRow_1"], mode1, 1, 0);
        checkValue(reader->GetRow()["UrlRow_2"], mode2, 1, 1);
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }

    // Check that all the values can be read by protobuf reader.
    auto reader = client->CreateTableReader<TRow>(workingDir + "/table");
    for (const auto& [host, path, code] : firstValuesInRows) {
        EXPECT_TRUE(reader->IsValid());
        const auto& row = reader->GetRow();
        const auto& embedded1 = row.GetUrlRow_1();
        EXPECT_EQ(embedded1.GetHost(), host);
        EXPECT_EQ(embedded1.GetPath(), path);
        EXPECT_EQ(embedded1.GetHttpCode(), code);
        const auto& embedded2 = row.GetUrlRow_2();
        EXPECT_EQ(embedded2.GetHost(), host);
        EXPECT_EQ(embedded2.GetPath(), path);
        EXPECT_EQ(embedded2.GetHttpCode(), code + 1);
        reader->Next();
    }
    EXPECT_TRUE(!reader->IsValid());
}

TEST_P(TProtobufTableIoTest, ProtobufSerializationMode_FieldOption)
{
    TestProtobufSerializationModes<TRowFieldSerializationOption, TUrlRow>(
        EWrapperFieldFlag::SERIALIZATION_YT,
        EWrapperFieldFlag::SERIALIZATION_PROTOBUF,
        GetParam());
}

TEST_P(TProtobufTableIoTest, ProtobufSerializationMode_MessageOption)
{
    TestProtobufSerializationModes<TRowMessageSerializationOption, TUrlRow>(
        EWrapperFieldFlag::SERIALIZATION_YT,
        EWrapperFieldFlag::SERIALIZATION_YT,
        GetParam());
}

TEST_P(TProtobufTableIoTest, ProtobufSerializationMode_MixedOptions)
{
    TestProtobufSerializationModes<TRowMixedSerializationOptions, TUrlRow>(
        EWrapperFieldFlag::SERIALIZATION_YT,
        EWrapperFieldFlag::SERIALIZATION_PROTOBUF,
        GetParam());
}

TEST_P(TProtobufTableIoTest, ProtobufSerializationMode_MixedOptions_ColumnNames)
{
    TestProtobufSerializationModes<TRowMixedSerializationOptions_ColumnNames, TUrlRowWithColumnNames>(
        EWrapperFieldFlag::SERIALIZATION_YT,
        EWrapperFieldFlag::SERIALIZATION_PROTOBUF,
        GetParam(),
        {"Host_ColumnName", "Path_KeyColumnName", "HttpCode"});
}

TEST_P(TProtobufTableIoTest, ProtobufRepeatedSerialization)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    auto schema = TTableSchema()
        .AddColumn(TColumnSchema().Name("Ints").Type(NTi::List(NTi::Int64())))
        .AddColumn(TColumnSchema().Name("UrlRows").Type(NTi::List(GetRowType<TUrlRow>())))
        .AddColumn(TColumnSchema().Name("PackedInts").Type(NTi::List(NTi::Int64())));

    {
        auto writer = client->CreateTableWriter<TRowSerializedRepeatedFields>(
            TRichYPath(workingDir + "/table").Schema(schema));
        {
            TRowSerializedRepeatedFields row;
            row.AddInts(1);
            row.AddInts(2);
            row.AddInts(3);
            row.AddPackedInts(-1);
            row.AddPackedInts(-2);
            row.AddPackedInts(-3);
            auto& urlRow1 = *row.AddUrlRows();
            urlRow1.SetHost("http://www.example.com");
            urlRow1.SetPath("/");
            urlRow1.SetHttpCode(303);
            auto& urlRow2 = *row.AddUrlRows();
            urlRow2.SetHost("http://www.example.com");
            urlRow2.SetPath("/");
            urlRow2.SetHttpCode(307);
            writer->AddRow(row);
        }
        {
            TRowSerializedRepeatedFields row;
            row.AddInts(101);
            row.AddInts(201);
            row.AddInts(301);
            row.AddPackedInts(-101);
            row.AddPackedInts(-201);
            row.AddPackedInts(-301);
            auto& urlRow1 = *row.AddUrlRows();
            urlRow1.SetHost("http://www.example.com");
            urlRow1.SetPath("/index.php");
            urlRow1.SetHttpCode(200);
            auto& urlRow2 = *row.AddUrlRows();
            urlRow2.SetHost("http://www.example.com");
            urlRow2.SetPath("/index.php");
            urlRow2.SetHttpCode(201);
            writer->AddRow(row);
        }
        writer->Finish();
    }

    {
        auto reader = client->CreateTableReader<TNode>(workingDir + "/table");
        EXPECT_TRUE(reader->IsValid());
        EXPECT_EQ(
            reader->GetRow(),
            TNode()
                ("Ints", TNode().Add(1).Add(2).Add(3))
                ("PackedInts", TNode().Add(-1).Add(-2).Add(-3))
                ("UrlRows", TNode()
                    .Add(TNode()("Host", "http://www.example.com")("Path", "/")("HttpCode", 303))
                    .Add(TNode()("Host", "http://www.example.com")("Path", "/")("HttpCode", 307))));
        reader->Next();
        EXPECT_TRUE(reader->IsValid());
        EXPECT_EQ(
            reader->GetRow(),
            TNode()
                ("Ints", TNode().Add(101).Add(201).Add(301))
                ("PackedInts", TNode().Add(-101).Add(-201).Add(-301))
                ("UrlRows", TNode()
                    .Add(TNode()("Host", "http://www.example.com")("Path", "/index.php")("HttpCode", 200))
                    .Add(TNode()("Host", "http://www.example.com")("Path", "/index.php")("HttpCode", 201))));
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }

    auto reader = client->CreateTableReader<TRowSerializedRepeatedFields>(workingDir + "/table");
    EXPECT_TRUE(reader->IsValid());
    {
        const auto& row = reader->GetRow();
        TVector<int> ints(row.GetInts().begin(), row.GetInts().end());
        EXPECT_EQ(ints, (TVector<int>{1, 2, 3}));
        TVector<int> packedInts(row.GetPackedInts().begin(), row.GetPackedInts().end());
        EXPECT_EQ(packedInts, (TVector<int>{-1, -2, -3}));
        EXPECT_EQ(static_cast<int>(row.UrlRowsSize()), 2);
        const auto& urlRow1 = row.GetUrlRows(0);
        EXPECT_EQ(urlRow1.GetHost(), "http://www.example.com");
        EXPECT_EQ(urlRow1.GetPath(), "/");
        EXPECT_EQ(urlRow1.GetHttpCode(), 303);
        const auto& urlRow2 = row.GetUrlRows(1);
        EXPECT_EQ(urlRow2.GetHost(), "http://www.example.com");
        EXPECT_EQ(urlRow2.GetPath(), "/");
        EXPECT_EQ(urlRow2.GetHttpCode(), 307);
    }
    reader->Next();
    EXPECT_TRUE(reader->IsValid());
    {
        const auto& row = reader->GetRow();
        TVector<int> ints(row.GetInts().begin(), row.GetInts().end());
        EXPECT_EQ(ints, (TVector<int>{101, 201, 301}));
        TVector<int> packedInts(row.GetPackedInts().begin(), row.GetPackedInts().end());
        EXPECT_EQ(packedInts, (TVector<int>{-101, -201, -301}));
        EXPECT_EQ(static_cast<int>(row.UrlRowsSize()), 2);
        const auto& urlRow1 = row.GetUrlRows(0);
        EXPECT_EQ(urlRow1.GetHost(), "http://www.example.com");
        EXPECT_EQ(urlRow1.GetPath(), "/index.php");
        EXPECT_EQ(urlRow1.GetHttpCode(), 200);
        const auto& urlRow2 = row.GetUrlRows(1);
        EXPECT_EQ(urlRow2.GetHost(), "http://www.example.com");
        EXPECT_EQ(urlRow2.GetPath(), "/index.php");
        EXPECT_EQ(urlRow2.GetHttpCode(), 201);
    }
    reader->Next();
    EXPECT_TRUE(!reader->IsValid());
}

TEST_P(TProtobufTableIoTest, ForbiddenRepeatedInProtobuf)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    auto schema = TTableSchema()
        .AddColumn(TColumnSchema().Name("Ints").Type(NTi::List(NTi::Int64())));

    auto run = [&] {
        auto writer = client->CreateTableWriter<TBadProtobufSerializedRow>(
            TRichYPath(workingDir + "/table").Schema(schema));
        writer->Finish();
    };
    EXPECT_THROW_MESSAGE_HAS_SUBSTR(
        run(),
        yexception,
        "Repeated field \"NYT.NTesting.TBadProtobufSerializedRow.Ints\" must have flag \"SERIALIZATION_YT\"");
}

TEST_P(TProtobufTableIoTest, ProtobufWithTypeOption)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    auto schema = TTableSchema()
        .Strict(false)
        .AddColumn(TColumnSchema()
            .Name("ColorIntField").Type(EValueType::VT_INT64))
        .AddColumn(TColumnSchema()
            .Name("ColorStringField").Type(EValueType::VT_STRING))
        .AddColumn(TColumnSchema()
            .Name("AnyField").Type(EValueType::VT_ANY))
        .AddColumn(TColumnSchema()
            .Name("EmbeddedField").RawTypeV3(TNode()
                ("type_name", "optional")
                ("item", TNode()
                    ("type_name", "struct")
                    ("members", TNode()
                        .Add(TNode()
                            ("name", "ColorIntField")
                            ("type", "int64"))
                        .Add(TNode()
                            ("name", "ColorStringField")
                            ("type", "string"))
                        .Add(TNode()
                            ("name", "AnyField")
                            ("type", TNode()
                                ("type_name", "optional")
                                ("item", "yson")))))))
        .AddColumn(TColumnSchema()
            .Name("RepeatedEnumIntField").RawTypeV3(TNode()
                ("type_name", "list")
                ("item", "int64")))
        .AddColumn(TColumnSchema()
            .Name("UnknownSchematizedColumn").Type(EValueType::VT_BOOLEAN));

    auto node1 = TNode()("x", TNode()("y", 12));
    auto node2 = TNode()("key", "value");
    auto node3 = TNode().Add(1).Add("string").Add(true);
    auto node4 = TNode("hooray");

    {
        auto writer = client->CreateTableWriter<TRowWithTypeOptions>(
            TRichYPath(workingDir + "/table").Schema(schema));
        {
            TRowWithTypeOptions row;
            row.SetColorIntField(TRowWithTypeOptions::RED);
            row.SetColorStringField(TRowWithTypeOptions::BLUE);
            row.SetAnyField(NodeToYsonString(node1));
            row.SetOtherColumnsField(NodeToYsonString(
                TNode()
                    ("UnknownSchematizedColumn", true)
                    ("UnknownUnschematizedColumn", 1234)));
            auto& embedded = *row.MutableEmbeddedField();
            embedded.SetColorIntField(TRowWithTypeOptions::WHITE);
            embedded.SetColorStringField(TRowWithTypeOptions::RED);
            embedded.SetAnyField(NodeToYsonString(node2));
            row.AddRepeatedEnumIntField(TRowWithTypeOptions::WHITE);
            row.AddRepeatedEnumIntField(TRowWithTypeOptions::BLUE);
            row.AddRepeatedEnumIntField(TRowWithTypeOptions::RED);
            writer->AddRow(row);
        }
        {
            TRowWithTypeOptions row;
            row.SetColorIntField(TRowWithTypeOptions::WHITE);
            row.SetColorStringField(TRowWithTypeOptions::RED);
            row.SetAnyField(NodeToYsonString(node3));
            row.SetOtherColumnsField(NodeToYsonString(
                TNode()
                    ("UnknownSchematizedColumn", false)
                    ("UnknownUnschematizedColumn", "some-string")));
            auto& embedded = *row.MutableEmbeddedField();
            embedded.SetColorIntField(TRowWithTypeOptions::RED);
            embedded.SetColorStringField(TRowWithTypeOptions::WHITE);
            embedded.SetAnyField(NodeToYsonString(node4));
            row.AddRepeatedEnumIntField(TRowWithTypeOptions::BLUE);
            writer->AddRow(row);
        }
        writer->Finish();
    }

    {
        auto reader = client->CreateTableReader<TNode>(workingDir + "/table");
        EXPECT_TRUE(reader->IsValid());
        EXPECT_EQ(
            reader->GetRow(),
            TNode()
                ("ColorIntField", -1)
                ("ColorStringField", "BLUE")
                ("AnyField", node1)
                ("UnknownSchematizedColumn", true)
                ("UnknownUnschematizedColumn", 1234)
                ("EmbeddedField", TNode()
                    ("ColorIntField", 0)
                    ("ColorStringField", "RED")
                    ("AnyField", node2))
                ("RepeatedEnumIntField", TNode()
                    .Add(0)
                    .Add(1)
                    .Add(-1)));
        reader->Next();
        EXPECT_TRUE(reader->IsValid());
        EXPECT_EQ(
            reader->GetRow(),
            TNode()
                ("ColorIntField", 0)
                ("ColorStringField", "RED")
                ("AnyField", node3)
                ("UnknownSchematizedColumn", false)
                ("UnknownUnschematizedColumn", "some-string")
                ("EmbeddedField", TNode()
                    ("ColorIntField", -1)
                    ("ColorStringField", "WHITE")
                    ("AnyField", node4))
                ("RepeatedEnumIntField", TNode().Add(1)));
        reader->Next();
        EXPECT_TRUE(!reader->IsValid());
    }

    auto reader = client->CreateTableReader<TRowWithTypeOptions>(workingDir + "/table");
    EXPECT_TRUE(reader->IsValid());
    {
        const auto& row = reader->GetRow();
        EXPECT_EQ(row.GetColorIntField(), TRowWithTypeOptions::RED);
        EXPECT_EQ(row.GetColorStringField(), TRowWithTypeOptions::BLUE);
        EXPECT_EQ(NodeFromYsonString(row.GetAnyField()), node1);
        auto otherColumns = NodeFromYsonString(row.GetOtherColumnsField());
        EXPECT_TRUE(otherColumns.HasKey("UnknownSchematizedColumn"));
        EXPECT_EQ(otherColumns["UnknownSchematizedColumn"], true);
        EXPECT_TRUE(otherColumns.HasKey("UnknownUnschematizedColumn"));
        EXPECT_EQ(otherColumns["UnknownUnschematizedColumn"], 1234);
        const auto& embedded = row.GetEmbeddedField();
        EXPECT_EQ(embedded.GetColorIntField(), TRowWithTypeOptions::WHITE);
        EXPECT_EQ(embedded.GetColorStringField(), TRowWithTypeOptions::RED);
        EXPECT_EQ(NodeFromYsonString(embedded.GetAnyField()), node2);

        TVector<TRowWithTypeOptions::Color> colors;
        for (auto intColor : row.GetRepeatedEnumIntField()) {
            colors.push_back(static_cast<TRowWithTypeOptions::Color>(intColor));
        }
        EXPECT_EQ(
            colors,
            (TVector<TRowWithTypeOptions::Color>{
                TRowWithTypeOptions::WHITE,
                TRowWithTypeOptions::BLUE,
                TRowWithTypeOptions::RED,
            }));
    }
    reader->Next();
    EXPECT_TRUE(reader->IsValid());
    {
        const auto& row = reader->GetRow();
        EXPECT_EQ(row.GetColorIntField(), TRowWithTypeOptions::WHITE);
        EXPECT_EQ(row.GetColorStringField(), TRowWithTypeOptions::RED);
        EXPECT_EQ(NodeFromYsonString(row.GetAnyField()), node3);
        auto otherColumns = NodeFromYsonString(row.GetOtherColumnsField());
        EXPECT_TRUE(otherColumns.HasKey("UnknownSchematizedColumn"));
        EXPECT_EQ(otherColumns["UnknownSchematizedColumn"], false);
        EXPECT_TRUE(otherColumns.HasKey("UnknownUnschematizedColumn"));
        EXPECT_EQ(otherColumns["UnknownUnschematizedColumn"], "some-string");
        const auto& embedded = row.GetEmbeddedField();
        EXPECT_EQ(embedded.GetColorIntField(), TRowWithTypeOptions::RED);
        EXPECT_EQ(embedded.GetColorStringField(), TRowWithTypeOptions::WHITE);
        EXPECT_EQ(NodeFromYsonString(embedded.GetAnyField()), node4);

        TVector<TRowWithTypeOptions::Color> colors;
        for (auto intColor : row.GetRepeatedEnumIntField()) {
            colors.push_back(static_cast<TRowWithTypeOptions::Color>(intColor));
        }
        EXPECT_EQ(
            colors,
            (TVector<TRowWithTypeOptions::Color>{TRowWithTypeOptions::BLUE}));
    }
    reader->Next();
    EXPECT_TRUE(!reader->IsValid());
}

void TestProtobufSchemaInferring(bool setWriterOptions, bool WithDescriptors)
{
    TTableWriterOptions options;
    TConfigSaverGuard configGuard;
    TConfig::Get()->ProtobufFormatWithDescriptors = WithDescriptors;

    if (setWriterOptions) {
        options.InferSchema(true);
    } else {
        TConfig::Get()->InferTableSchema = true;
    }

    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();

    {
        auto writer = client->CreateTableWriter<TRowVer2>(workingDir + "/table", options);
        TRowVer2 row;
        row.SetString_1("abc");
        row.SetUint32_2(40 + 2);
        row.SetFixed64_3(8888);
        writer->AddRow(row);
        writer->Finish();
    }

    TTableSchema actualSchema;
    Deserialize(actualSchema, client->Get(workingDir + "/table/@schema"));
    EXPECT_TRUE(AreSchemasEqual(
        actualSchema,
        TTableSchema()
            .AddColumn(TColumnSchema().Name("String_1").Type(EValueType::VT_STRING))
            .AddColumn(TColumnSchema().Name("Uint32_2").Type(EValueType::VT_UINT32))
            .AddColumn(TColumnSchema().Name("Fixed64_3").Type(EValueType::VT_UINT64))));
}

TEST_P(TProtobufTableIoTest, ProtobufSchemaInferring_Config)
{
    TestProtobufSchemaInferring(false, GetParam());
}

TEST_P(TProtobufTableIoTest, ProtobufSchemaInferring_Options)
{
    TestProtobufSchemaInferring(true, GetParam());
}

TEST_P(TProtobufTableIoTest, ProtobufWriteRead_Enum)
{
    TTestFixture fixture;
    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();
    TConfig::Get()->UseClientProtobuf = false;

    auto table = TRichYPath(workingDir + "/table");

    const TVector<EEnum> expected{EEnum::One, EEnum::Two, EEnum::Three, EEnum::MinusFortyTwo};
    {
        auto writer = client->CreateTableWriter<TAllTypesMessage>(table);

        for (const EEnum& enumField : expected) {
            TAllTypesMessage row;
            row.SetEnumField(enumField);
            row.SetEnumIntField(enumField);
            writer->AddRow(row);
        }

        writer->Finish();
    }

    TVector<EEnum> actual;
    {
        auto reader = client->CreateTableReader<TNode>(table);

        for (; reader->IsValid(); reader->Next()) {
            TAllTypesMessage row;
            ReadMessageFromNode(reader->GetRow(), &row);

            EXPECT_EQ(row.GetEnumField(), row.GetEnumIntField());

            actual.push_back(row.GetEnumField());
        }
    }

    EXPECT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(expected[i], actual[i]);
    }
}

TEST_P(TProtobufTableIoTest, ProtoClashingEnums_YT_13714)
{
    TTestFixture fixture;
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    auto testTable = workingDir + "/table";

    TClashingEnumMessage originalRow;
    originalRow.SetEnum1(TClashingEnumMessage1::ClashingEnumValueOne);
    originalRow.SetEnum2(TClashingEnumMessage2::ClashingEnumValueTwo);
    originalRow.SetEnum3(ClashingEnumValueThree);
    {
        auto writer = client->CreateTableWriter<TClashingEnumMessage>(testTable);
        writer->AddRow(originalRow);
        writer->Finish();
    }

    TVector<TClashingEnumMessage> readValues;
    auto reader = client->CreateTableReader<TClashingEnumMessage>(testTable);
    for (const auto& cursor : *reader) {
        readValues.push_back(cursor.GetRow());
    }

    EXPECT_EQ(std::ssize(readValues), 1);
    EXPECT_EQ(readValues[0].ShortUtf8DebugString(), originalRow.ShortUtf8DebugString());
}

TEST_P(TProtobufTableIoTest, Proto3Optional)
{
    TTestFixture fixture;
    TConfig::Get()->ProtobufFormatWithDescriptors = GetParam();

    // TODO(levysotsky): Enable this test after YT-15121 is deployed.
    if (GetParam()) {
        return;
    }

    auto client = fixture.GetClient();
    auto workingDir = fixture.GetWorkingDir();
    auto testTable = workingDir + "/table";

    TWithOptional row;
    row.SetOptionalField(42);
    row.SetNonOptionalField(12);

    {
        auto writer = client->CreateTableWriter<TWithOptional>(
            TRichYPath(testTable)
                .Schema(CreateTableSchema<TWithOptional>())
        );
        writer->AddRow(row);
        writer->Finish();
    }

    TTableSchema schema;
    Deserialize(schema, client->Get(testTable + "/@schema"));

    EXPECT_EQ(schema, TTableSchema()
        .AddColumn(TColumnSchema().Name("OptionalField").Type(NTi::Optional(NTi::Int64())))
        .AddColumn(TColumnSchema().Name("Dummy").Type(
            NTi::Optional(
                NTi::Variant(
                    NTi::Struct({
                        {"FieldInsideOneof", NTi::Int64()},
                    })
                )
            )
        ))
        .AddColumn(TColumnSchema().Name("EmbeddedField").Type(
            NTi::Optional(
                NTi::Struct({
                    {"OptionalField", NTi::Optional(NTi::Int64())},
                })
            )
        ))
        .AddColumn(TColumnSchema().Name("NonOptionalField").Type(NTi::Optional(NTi::Int64())))
    );

    TVector<TWithOptional> readValues;
    auto reader = client->CreateTableReader<TWithOptional>(testTable);
    for (const auto& cursor : *reader) {
        readValues.push_back(cursor.GetRow());
    }

    EXPECT_EQ(std::ssize(readValues), 1);
    EXPECT_EQ(readValues[0].ShortUtf8DebugString(), row.ShortUtf8DebugString());
}

INSTANTIATE_TEST_SUITE_P(WithDescriptors, TProtobufTableIoTest, ::testing::Values(true));
INSTANTIATE_TEST_SUITE_P(WithoutDescriptors, TProtobufTableIoTest, ::testing::Values(false));
