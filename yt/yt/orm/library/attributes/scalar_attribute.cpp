#include "scalar_attribute.h"

#include "helpers.h"
#include "proto_visitor.h"

#include <yt/yt/core/misc/error.h>

#include <yt/yt/core/ypath/tokenizer.h>

#include <yt/yt/core/yson/protobuf_interop.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/serialize.h>
#include <yt/yt/core/ytree/ypath_client.h>

#include <library/cpp/yt/misc/cast.h>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/unknown_field_set.h>
#include <google/protobuf/util/message_differencer.h>
#include <google/protobuf/wire_format.h>

#include <variant>

namespace NYT::NOrm::NAttributes {

namespace {

////////////////////////////////////////////////////////////////////////////////

using ::google::protobuf::Descriptor;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::Reflection;
using ::google::protobuf::UnknownField;
using ::google::protobuf::UnknownFieldSet;
using ::google::protobuf::internal::WireFormatLite;
using ::google::protobuf::io::CodedOutputStream;
using ::google::protobuf::io::StringOutputStream;
using ::google::protobuf::util::FieldComparator;
using ::google::protobuf::util::MessageDifferencer;

using namespace NYson;
using namespace NYTree;

constexpr int ProtobufMapKeyFieldNumber = 1;
constexpr int ProtobufMapValueFieldNumber = 2;

////////////////////////////////////////////////////////////////////////////////

template <bool IsMutable>
struct TMutabilityTraits;

template <>
struct TMutabilityTraits</*IsMutable*/ true>
{
    using TGenericMessage = NProtoBuf::Message;

    static TGenericMessage* GetMessage(TGenericMessage* root)
    {
        return const_cast<TGenericMessage*>(root);
    }

    static TGenericMessage* GetRepeatedMessage(TGenericMessage* root, const FieldDescriptor* field, int index)
    {
        return root->GetReflection()->MutableRepeatedMessage(
            const_cast<TGenericMessage*>(root),
            field,
            index);
    }

    static TGenericMessage* GetMessage(TGenericMessage* root, const FieldDescriptor* field)
    {
        return root->GetReflection()->MutableMessage(const_cast<TGenericMessage*>(root), field);
    }
};

template <>
struct TMutabilityTraits</*IsMutable*/ false>
{
    using TGenericMessage = const NProtoBuf::Message;

    static TGenericMessage* GetMessage(const TGenericMessage* root)
    {
        return root;
    }

    static TGenericMessage* GetRepeatedMessage(TGenericMessage* root, const FieldDescriptor* field, int index)
    {
        return &root->GetReflection()->GetRepeatedMessage(
            *root,
            field,
            index);
    }

    static TGenericMessage* GetMessage(TGenericMessage* root, const FieldDescriptor* field)
    {
        return &root->GetReflection()->GetMessage(*root, field);
    }
};

////////////////////////////////////////////////////////////////////////////////

const TString& GetKey(
    const NProtoBuf::Message& message,
    const FieldDescriptor* field,
    TString& scratch)
{
    const auto* reflection = message.GetReflection();
    switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
            scratch = ToString(reflection->GetInt32(message, field));
            return scratch;
        case FieldDescriptor::CPPTYPE_INT64:
            scratch = ToString(reflection->GetInt64(message, field));
            return scratch;
        case FieldDescriptor::CPPTYPE_UINT32:
            scratch = ToString(reflection->GetUInt32(message, field));
            return scratch;
        case FieldDescriptor::CPPTYPE_UINT64:
            scratch = ToString(reflection->GetUInt64(message, field));
            return scratch;
        case FieldDescriptor::CPPTYPE_BOOL:
            scratch = ToString(reflection->GetBool(message, field));
            return scratch;
        case FieldDescriptor::CPPTYPE_STRING:
            return reflection->GetStringReference(message, field, &scratch);
        default:
            break;
    }
    THROW_ERROR_EXCEPTION("Unexpected map key type %v",
        static_cast<int>(field->cpp_type()));
}


void SetKey(
    NProtoBuf::Message* message,
    const FieldDescriptor* field,
    const TString& key)
{
    const auto* reflection = message->GetReflection();
    switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
            reflection->SetInt32(message, field, FromString<i32>(key));
            return;
        case FieldDescriptor::CPPTYPE_INT64:
            reflection->SetInt64(message, field, FromString<i64>(key));
            return;
        case FieldDescriptor::CPPTYPE_UINT32:
            reflection->SetUInt32(message, field, FromString<ui32>(key));
            return;
        case FieldDescriptor::CPPTYPE_UINT64:
            reflection->SetUInt64(message, field, FromString<ui64>(key));
            return;
        case FieldDescriptor::CPPTYPE_BOOL:
            reflection->SetBool(message, field, FromString<bool>(key));
            return;
        case FieldDescriptor::CPPTYPE_STRING:
            reflection->SetString(message, field, key);
            return;
        default:
            break;
    }
    THROW_ERROR_EXCEPTION("Unexpected map key type %v",
        static_cast<int>(field->cpp_type()));
}

template <bool IsMutable>
struct TLookupMapItemResult
{
    typename TMutabilityTraits<IsMutable>::TGenericMessage* Message;
    int Index;
};

template <bool IsMutable>
std::optional<TLookupMapItemResult<IsMutable>> LookupMapItem(
    typename TMutabilityTraits<IsMutable>::TGenericMessage* message,
    const FieldDescriptor* field,
    const TString& key)
{
    YT_VERIFY(field->is_map());
    const auto* keyType = field->message_type()->map_key();

    const auto* reflection = message->GetReflection();
    int count = reflection->FieldSize(*message, field);

    TString tmp;
    for (int index = 0; index < count; ++index) {
        auto* item = TMutabilityTraits<IsMutable>::GetRepeatedMessage(message, field, index);
        const TString& mapKey = GetKey(*item, keyType, tmp);
        if (mapKey == key) {
            return TLookupMapItemResult<IsMutable>{item, index};
        }
    }
    return std::nullopt;
}

NProtoBuf::Message* AddMapItem(NProtoBuf::Message* message, const FieldDescriptor* field, const TString& key)
{
    YT_VERIFY(field->is_map());
    const auto* keyType = field->message_type()->map_key();

    auto* item = message->GetReflection()->AddMessage(message, field);
    YT_VERIFY(item);

    SetKey(item, keyType, key);
    return item;
}

template <bool IsMutable>
std::optional<typename TMutabilityTraits<IsMutable>::TGenericMessage*> LookupMapItemValue(
    typename TMutabilityTraits<IsMutable>::TGenericMessage* message,
    const FieldDescriptor* field,
    const TString& key)
{
    auto itemIfPresent = LookupMapItem<IsMutable>(message, field, key);

    if (itemIfPresent) {
        auto item = itemIfPresent->Message;
        YT_ASSERT(field->message_type()->map_value()->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE);
        const auto* fieldDescriptor = field->message_type()->map_value();
        return TMutabilityTraits<IsMutable>::GetMessage(item, fieldDescriptor);
    } else {
        return std::nullopt;
    }
}

NProtoBuf::Message* AddMapItemValue(NProtoBuf::Message* message, const FieldDescriptor* field, const TString& key)
{
    auto* item = AddMapItem(message, field, key);
    YT_ASSERT(field->message_type()->map_value()->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE);
    return TMutabilityTraits</*IsMutable*/ true>::GetMessage(item, field->message_type()->map_value());
}

////////////////////////////////////////////////////////////////////////////////

std::optional<int> LookupUnknownYsonFieldsItem(UnknownFieldSet* unknownFields, TStringBuf key, TString& value)
{
    int count = unknownFields->field_count();
    for (int index = 0; index < count; ++index) {
        auto* field = unknownFields->mutable_field(index);
        if (field->number() == UnknownYsonFieldNumber) {
            UnknownFieldSet tmpItem;
            THROW_ERROR_EXCEPTION_UNLESS(field->type() == UnknownField::TYPE_LENGTH_DELIMITED,
                "Unexpected type %v of item within yson unknown field set",
                static_cast<int>(field->type()));
            THROW_ERROR_EXCEPTION_UNLESS(tmpItem.ParseFromString(field->length_delimited()),
                "Cannot parse UnknownYsonFields item");
            THROW_ERROR_EXCEPTION_UNLESS(tmpItem.field_count() == 2,
                "Unexpected field count %v in item within yson unknown field set",
                tmpItem.field_count());
            auto* keyField = tmpItem.mutable_field(0);
            auto* valueField = tmpItem.mutable_field(1);
            if (keyField->number() != ProtobufMapKeyFieldNumber) {
                std::swap(keyField, valueField);
            }
            THROW_ERROR_EXCEPTION_UNLESS(keyField->number() == ProtobufMapKeyFieldNumber,
                "Unexpected key tag %v of item within yson unknown field set",
                keyField->number());
            THROW_ERROR_EXCEPTION_UNLESS(valueField->number() == ProtobufMapValueFieldNumber,
                "Unexpected value tag %v of item within yson unknown field set",
                valueField->number());
            THROW_ERROR_EXCEPTION_UNLESS(keyField->type() == UnknownField::TYPE_LENGTH_DELIMITED,
                "Unexpected key type %v of item within yson unknown field set",
                static_cast<int>(keyField->type()));
            THROW_ERROR_EXCEPTION_UNLESS(valueField->type() == UnknownField::TYPE_LENGTH_DELIMITED,
                "Unexpected value type %v of item within yson unknown field set",
                static_cast<int>(valueField->type()));
            if (keyField->length_delimited() == key) {
                value = std::move(*valueField->mutable_length_delimited());
                return index;
            }
        }
    }
    return std::nullopt;
}

TString SerializeUnknownYsonFieldsItem(TStringBuf key, TStringBuf value)
{
    TString output;
    StringOutputStream outputStream(&output);
    CodedOutputStream codedOutputStream(&outputStream);
    codedOutputStream.WriteTag(
        WireFormatLite::MakeTag(ProtobufMapKeyFieldNumber, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
    codedOutputStream.WriteVarint64(key.length());
    codedOutputStream.WriteRaw(key.data(), static_cast<int>(key.length()));
    codedOutputStream.WriteTag(
        WireFormatLite::MakeTag(ProtobufMapValueFieldNumber, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
    codedOutputStream.WriteVarint64(value.length());
    codedOutputStream.WriteRaw(value.data(), static_cast<int>(value.length()));
    return output;
}

////////////////////////////////////////////////////////////////////////////////

int ConvertToEnumValue(const INodePtr& node, const FieldDescriptor* field)
{
    YT_VERIFY(field->cpp_type() == FieldDescriptor::CPPTYPE_ENUM);
    if (node->GetType() == ENodeType::Entity) {
        return field->default_value_enum()->number();
    }
    return ConvertToProtobufEnumValue<int>(ReflectProtobufEnumType(field->enum_type()), node);
}

template <typename T>
T ConvertToProtobufValue(const INodePtr& node, const T& defaultValue)
{
    if (node->GetType() == ENodeType::Entity) {
        return defaultValue;
    }
    return ConvertTo<T>(node);
}

[[noreturn]] void ThrowUnsupportedCppType(google::protobuf::FieldDescriptor::CppType cppType, TStringBuf path)
{
    THROW_ERROR_EXCEPTION("Unsupported cpp_type %v for attribute %Qv",
        static_cast<int>(cppType),
        path);
}

////////////////////////////////////////////////////////////////////////////////

template <bool IsMutableParameter>
class IHandler
{
protected:
    constexpr static bool IsMutable = IsMutableParameter;
    using TGenericMessage = typename TMutabilityTraits<IsMutable>::TGenericMessage;

public:
    virtual ~IHandler() = default;

    //! Handle regular field.
    virtual void HandleRegular(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TStringBuf path) const = 0;

    //! Handle list (repeated) field item.
    virtual void HandleListItem(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TIndexParseResult parseIndexResult,
        TStringBuf path) const = 0;

    virtual void HandleListExpansion(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TStringBuf prefixPath,
        TStringBuf suffixPath) const = 0;

    //! Handle map item.
    virtual void HandleMapItem(
        TGenericMessage* message,
        const FieldDescriptor* field,
        const TString& key,
        TStringBuf path) const = 0;

    //! The field is not found.
    virtual void HandleUnknown(TGenericMessage* message, NYPath::TTokenizer& tokenizer) const = 0;

    //! Handle empty intermediate parents.
    //! If returns false, traverse stops, otherwise creates parent initialized with default values.
    virtual bool HandleMissing(TStringBuf path) const = 0;
};

////////////////////////////////////////////////////////////////////////////////

template <bool IsMutable>
void Traverse(
    typename TMutabilityTraits<IsMutable>::TGenericMessage* root,
    const NYPath::TYPath& path,
    const IHandler<IsMutable>& handler)
{
    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();

    while (true) {
        tokenizer.Expect(NYPath::ETokenType::Slash);
        tokenizer.Advance();
        tokenizer.Expect(NYPath::ETokenType::Literal);
        auto* descriptor = root->GetDescriptor();
        const auto* field = descriptor->FindFieldByName(tokenizer.GetLiteralValue());
        if (!field) {
            handler.HandleUnknown(root, tokenizer);
            break;
        }
        if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
            handler.HandleRegular(root, field, tokenizer.GetPrefix());
            break;
        }
        if (field->is_map()) {
            const auto* mapType = field->message_type();
            tokenizer.Expect(NYPath::ETokenType::Slash);
            tokenizer.Advance();
            tokenizer.Expect(NYPath::ETokenType::Literal);
            TString key = tokenizer.GetLiteralValue();
            if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
                handler.HandleMapItem(root, field, key, tokenizer.GetPrefix());
                break;
            } else {
                THROW_ERROR_EXCEPTION_UNLESS(
                    mapType->map_value()->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE,
                    "Unexpected type %v for map value at %Qv",
                    mapType->map_value()->type_name(),
                    tokenizer.GetPrefix());

                auto valueIfPresent = LookupMapItemValue<IsMutable>(root, field, key);
                if (!valueIfPresent) {
                    if (handler.HandleMissing(tokenizer.GetPrefix())) {
                        if constexpr (IsMutable) {
                            valueIfPresent.emplace(AddMapItemValue(
                                TMutabilityTraits<IsMutable>::GetMessage(root),
                                field,
                                key));
                            THROW_ERROR_EXCEPTION_UNLESS(*valueIfPresent,
                                "Could not add map item with key %Q", key);
                        } else {
                            THROW_ERROR_EXCEPTION("Cannot add map item to immutable field %Qv",
                                root->GetTypeName());
                        }
                    } else {
                        break;
                    }
                }
                YT_VERIFY(valueIfPresent && *valueIfPresent);
                root = *valueIfPresent;
            }
        } else if (field->is_repeated()) {
            tokenizer.Expect(NYPath::ETokenType::Slash);
            auto prefix = tokenizer.GetPrefix();
            tokenizer.Advance();
            if (tokenizer.GetType() == NYPath::ETokenType::Asterisk) {
                handler.HandleListExpansion(root, field, prefix, tokenizer.GetSuffix());
                break;
            }
            tokenizer.ExpectListIndex();

            TString indexToken = tokenizer.GetLiteralValue();
            auto* reflection = root->GetReflection();
            int count = reflection->FieldSize(*root, field);
            auto parseIndexResult = ParseListIndex(indexToken, count);
            if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
                handler.HandleListItem(root, field, parseIndexResult, tokenizer.GetPrefix());
                break;
            } else {
                THROW_ERROR_EXCEPTION_UNLESS(field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE,
                    "Unexpected type %v of attribute %Qv",
                    field->type_name(),
                    tokenizer.GetPrefixPlusToken());
                if (parseIndexResult.IsOutOfBounds(count)) {
                    THROW_ERROR_EXCEPTION_IF(handler.HandleMissing(tokenizer.GetPrefix()),
                        "Cannot add default values to repeated field %Qv", tokenizer.GetPrefix());
                    break;
                } else {
                    auto index = parseIndexResult.Index;
                    root = TMutabilityTraits<IsMutable>::GetRepeatedMessage(root, field, index);
                }
            }
        } else if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
            const auto* reflection = root->GetReflection();
            if (reflection->HasField(*root, field)) {
                root = TMutabilityTraits<IsMutable>::GetMessage(root, field);
            } else if (handler.HandleMissing(tokenizer.GetPrefix())) {
                THROW_ERROR_EXCEPTION_UNLESS(IsMutable,
                    "Cannot add message %Qv to immutable field %Qv",
                    field->type_name(),
                    root->GetTypeName());
                root = TMutabilityTraits<IsMutable>::GetMessage(root, field);
            } else {
                break;
            }
        } else {
            THROW_ERROR_EXCEPTION("Unexpected type %v of attribute %Qv",
                field->type_name(),
                tokenizer.GetPrefixPlusToken());
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

class TClearVisitor final
    : public TProtoVisitor<Message*>
{
public:
    TClearVisitor(bool allowMissing)
    {
        AllowMissing_ = allowMissing;
        AllowAsterisk_ = true;
    }

protected:
    void VisitWholeMessage(
        Message* message,
        EVisitReason reason) override
    {
        if (PathComplete()) {
            // Asterisk means clear all fields but keep the message present.
            message->Clear();
            return;
        }

        TProtoVisitor::VisitWholeMessage(message, reason);
    }

    void VisitWholeMapField(
        Message* message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason) override
    {
        if (PathComplete()) {
            // User supplied a useless trailing asterisk. Avoid quardatic deletion.
            VisitField(message, fieldDescriptor, EVisitReason::Manual);
            return;
        }

        TProtoVisitor::VisitWholeMapField(message, fieldDescriptor, reason);
    }

    void VisitWholeRepeatedField(
        Message* message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason) override
    {
        if (PathComplete()) {
            // User supplied a useless trailing asterisk. Avoid quardatic deletion.
            VisitField(message, fieldDescriptor, EVisitReason::Manual);
            return;
        }

        TProtoVisitor::VisitWholeRepeatedField(message, fieldDescriptor, reason);
    }

    void VisitUnrecognizedField(
        Message* message,
        const Descriptor* descriptor,
        TString name,
        EVisitReason reason) override
    {
        auto* unknownFields = message->GetReflection()->MutableUnknownFields(message);

        TString value;
        auto index = LookupUnknownYsonFieldsItem(unknownFields, name, value);

        if (index.has_value()) {
            if (PathComplete()) {
                unknownFields->DeleteSubrange(*index, 1);
                return;
            }
            if (RemoveFromNodeByPath(value)) {
                auto* item = unknownFields->mutable_field(*index)->mutable_length_delimited();
                *item = SerializeUnknownYsonFieldsItem(name, value);
                return;
            }
        }

        TProtoVisitor::VisitUnrecognizedField(message, descriptor, std::move(name), reason);
    }

    void VisitField(
        Message* message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason) override
    {
        auto* reflection = message->GetReflection();
        if (PathComplete()) {
            if (!fieldDescriptor->has_presence() ||
                reflection->HasField(*message, fieldDescriptor))
            {
                reflection->ClearField(message, fieldDescriptor);
                return;
            } // Else let the basic implementation of AllowMissing do the check.
        }

        TProtoVisitor::VisitField(message, fieldDescriptor, reason);
    }

    void VisitMapFieldEntry(
        Message* message,
        const FieldDescriptor* fieldDescriptor,
        Message* entryMessage,
        TString key,
        EVisitReason reason) override
    {
        if (PathComplete()) {
            int index = LocateMapEntry(message, fieldDescriptor, entryMessage).Value();
            DeleteRepeatedFieldEntry(message, fieldDescriptor, index);
            return;
        }

        TProtoVisitor::VisitMapFieldEntry(
            message,
            fieldDescriptor,
            entryMessage,
            std::move(key),
            reason);
    }

    void VisitRepeatedFieldEntry(
        Message* message,
        const FieldDescriptor* fieldDescriptor,
        int index,
        EVisitReason reason) override
    {
        if (PathComplete()) {
            DeleteRepeatedFieldEntry(message, fieldDescriptor, index);
            return;
        }

        TProtoVisitor::VisitRepeatedFieldEntry(message, fieldDescriptor, index, reason);
    }

    bool RemoveFromNodeByPath(TString& nodeString)
    {
        auto root = ConvertToNode(TYsonStringBuf{nodeString});
        Expect(NYPath::ETokenType::Slash);
        if (RemoveNodeByYPath(root, NYPath::TYPath{Tokenizer_.GetInput()})) {
            nodeString = ConvertToYsonString(root).ToString();
            return true;
        }
        return false;
    }

    void DeleteRepeatedFieldEntry(
        Message* message,
        const FieldDescriptor* fieldDescriptor,
        int index)
    {
        auto* reflection = message->GetReflection();
        int size = reflection->FieldSize(*message, fieldDescriptor);
        for (++index; index < size; ++index) {
            reflection->SwapElements(message, fieldDescriptor, index - 1, index);
        }
        reflection->RemoveLast(message, fieldDescriptor);
    }
}; //TClearVisitor

////////////////////////////////////////////////////////////////////////////////

class TSetHandler
    : public IHandler</*IsMutable*/ true>
{
public:
    TSetHandler(
        const INodePtr& value,
        const TProtobufWriterOptions& options,
        bool recursive)
        : Value_(value)
        , Options_(options)
        , Recursive_(recursive)
    { }

    void HandleRegular(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TStringBuf path) const final
    {
        if (field->is_map()) {
            HandleMap(message, field, path);
        } else if (field->is_repeated()) {
            message->GetReflection()->ClearField(message, field);
            if (Value_->GetType() != ENodeType::Entity) {
                AppendValues(message, field, path, Value_->AsList()->GetChildren());
            }
        } else {
            SetValue(message, field, path, Value_);
        }
    }

    void HandleListItem(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TIndexParseResult parseIndexResult,
        TStringBuf path) const final
    {
        YT_VERIFY(field->is_repeated());
        const auto* reflection = message->GetReflection();
        int count = reflection->FieldSize(*message, field);

        switch (parseIndexResult.IndexType) {
            case EListIndexType::Absolute: {
                parseIndexResult.EnsureIndexIsWithinBounds(count, path);
                SetValue(message, field, parseIndexResult.Index, path, Value_);
                break;
            }
            case EListIndexType::Relative: {
                // Index may be pointing past end of the list
                // as Set with index currently works as `insert before index`.
                parseIndexResult.EnsureIndexIsWithinBounds(count + 1, path);
                int beforeIndex = parseIndexResult.Index;
                AppendValues(message, field, path, {Value_});
                for (int index = beforeIndex; index < count; ++index) {
                    reflection->SwapElements(message, field, index, count);
                }
                break;
            }
        }
    }

    void HandleListExpansion(
        TGenericMessage* /*message*/,
        const FieldDescriptor* /*field*/,
        TStringBuf /*prefixPath*/,
        TStringBuf /*suffixPath*/) const final
    {
        THROW_ERROR_EXCEPTION("Set handler does not support list expansions");
    }

    void HandleMapItem(
        TGenericMessage* message,
        const FieldDescriptor* field,
        const TString& key,
        TStringBuf path) const final
    {
        YT_VERIFY(field->is_map());

        auto itemOrError = LookupMapItem<IsMutable>(message, field, key);
        TGenericMessage* item = itemOrError
            ? itemOrError->Message
            : AddMapItem(message, field, key);
        SetValue(item, field->message_type()->map_value(), path, Value_);
    }

    void HandleUnknown(TGenericMessage* message, NYPath::TTokenizer& tokenizer) const final
    {
        switch (Options_.UnknownYsonFieldModeResolver(NYPath::TYPath{tokenizer.GetPrefixPlusToken()})) {
            case EUnknownYsonFieldsMode::Skip:
                return;
            // Forward in a object type handler attribute leaf is interpreted as Fail.
            case EUnknownYsonFieldsMode::Forward:
            case EUnknownYsonFieldsMode::Fail:
                THROW_ERROR_EXCEPTION("Attribute %Qv is unknown", tokenizer.GetPrefixPlusToken());
            case EUnknownYsonFieldsMode::Keep:
                break;
        }
        TString key = tokenizer.GetLiteralValue();
        TString value;
        TString* item = nullptr;
        auto* unknownFields = message->GetReflection()->MutableUnknownFields(message);
        auto index = LookupUnknownYsonFieldsItem(unknownFields, key, value);
        if (!index.has_value()) {
            THROW_ERROR_EXCEPTION_UNLESS(Recursive_ || tokenizer.GetSuffix().empty(),
                "Attribute %Qv is missing",
                tokenizer.GetPrefixPlusToken());
            item = unknownFields->AddLengthDelimited(UnknownYsonFieldNumber);
        } else {
            item = unknownFields->mutable_field(*index)->mutable_length_delimited();
        }

        if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
            value = ConvertToYsonString(Value_).ToString();
        } else {
            SetNodeValueByPath(value, tokenizer, Value_, Recursive_);
        }
        *item = SerializeUnknownYsonFieldsItem(key, value);
    }

    bool HandleMissing(TStringBuf path) const final
    {
        THROW_ERROR_EXCEPTION_UNLESS(Recursive_, "Attribute %Qv is missing", path);
        return true;
    }

private:
    const INodePtr& Value_;
    const TProtobufWriterOptions& Options_;
    const bool Recursive_;

    void HandleMap(TGenericMessage* message, const FieldDescriptor* field, TStringBuf path) const
    {
        YT_VERIFY(field->is_map());
        const auto* reflection = message->GetReflection();
        reflection->ClearField(message, field);

        if (Value_->GetType() == ENodeType::Entity) {
            return;
        }
        for (const auto& [key, value] : Value_->AsMap()->GetChildren()) {
            TString fullPath = Format("%v/%v", path, key);
            auto* item = reflection->AddMessage(message, field);
            SetKey(item, field->message_type()->map_key(), key);
            SetValue(item, field->message_type()->map_value(), fullPath, value);
        }
    }

    void SetValue(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TStringBuf path,
        const INodePtr& node) const
    {
        const auto* reflection = message->GetReflection();
        switch (field->cpp_type()) {
            case FieldDescriptor::CPPTYPE_INT32:
                reflection->SetInt32(message, field, ConvertToProtobufValue(node, field->default_value_int32()));
                return;
            case FieldDescriptor::CPPTYPE_INT64:
                reflection->SetInt64(message, field, ConvertToProtobufValue(node, field->default_value_int64()));
                return;
            case FieldDescriptor::CPPTYPE_UINT32:
                reflection->SetUInt32(message, field, ConvertToProtobufValue(node, field->default_value_uint32()));
                return;
            case FieldDescriptor::CPPTYPE_UINT64:
                reflection->SetUInt64(message, field, ConvertToProtobufValue(node, field->default_value_uint64()));
                return;
            case FieldDescriptor::CPPTYPE_DOUBLE:
                reflection->SetDouble(message, field, ConvertToProtobufValue(node, field->default_value_double()));
                return;
            case FieldDescriptor::CPPTYPE_FLOAT:
                reflection->SetFloat(
                    message,
                    field,
                    CheckedIntegralCast<float>(ConvertToProtobufValue<double>(node, field->default_value_float())));
                return;
            case FieldDescriptor::CPPTYPE_BOOL:
                reflection->SetBool(message, field, ConvertToProtobufValue(node, field->default_value_bool()));
                return;
            case FieldDescriptor::CPPTYPE_ENUM:
                reflection->SetEnumValue(message, field, ConvertToEnumValue(node, field));
                return;
            case FieldDescriptor::CPPTYPE_STRING:
                reflection->SetString(message, field, ConvertToProtobufValue(node, field->default_value_string()));
                return;
            case FieldDescriptor::CPPTYPE_MESSAGE:
                DeserializeProtobufMessage(
                    *TMutabilityTraits<IsMutable>::GetMessage(message, field),
                    ReflectProtobufMessageType(field->message_type()),
                    node,
                    GetOptionsByPath(path));
                return;
        }
        ThrowUnsupportedCppType(field->cpp_type(), path);
    }

    void SetValue(
        TGenericMessage* message,
        const FieldDescriptor* field,
        int index,
        TStringBuf path,
        const INodePtr& node) const
    {
        YT_VERIFY(field->is_repeated());
        const auto* reflection = message->GetReflection();

        switch (field->cpp_type()) {
            case FieldDescriptor::CPPTYPE_INT32:
                reflection->SetRepeatedInt32(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_int32()));
                return;
            case FieldDescriptor::CPPTYPE_INT64:
                reflection->SetRepeatedInt64(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_int64()));
                return;
            case FieldDescriptor::CPPTYPE_UINT32:
                reflection->SetRepeatedUInt32(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_uint32()));
                return;
            case FieldDescriptor::CPPTYPE_UINT64:
                reflection->SetRepeatedUInt64(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_uint64()));
                return;
            case FieldDescriptor::CPPTYPE_DOUBLE:
                reflection->SetRepeatedDouble(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_double()));
                return;
            case FieldDescriptor::CPPTYPE_FLOAT:
                reflection->SetRepeatedFloat(
                    message,
                    field,
                    index,
                    CheckedIntegralCast<float>(ConvertToProtobufValue<double>(node, field->default_value_float())));
                return;
            case FieldDescriptor::CPPTYPE_BOOL:
                reflection->SetRepeatedBool(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_bool()));
                return;
            case FieldDescriptor::CPPTYPE_ENUM:
                reflection->SetRepeatedEnumValue(message, field, index, ConvertToEnumValue(node, field));
                return;
            case FieldDescriptor::CPPTYPE_STRING:
                reflection->SetRepeatedString(
                    message,
                    field,
                    index,
                    ConvertToProtobufValue(node, field->default_value_string()));
                return;
            case FieldDescriptor::CPPTYPE_MESSAGE:
                DeserializeProtobufMessage(
                    *TMutabilityTraits<IsMutable>::GetRepeatedMessage(message, field, index),
                    ReflectProtobufMessageType(field->message_type()),
                    node,
                    GetOptionsByPath(path));
                return;
        }
        ThrowUnsupportedCppType(field->cpp_type(), path);
    }

    void AppendValues(
        TGenericMessage* message,
        const FieldDescriptor* field,
        TStringBuf path,
        const std::vector<INodePtr>& nodes) const
    {
        YT_VERIFY(field->is_repeated());
        const auto* reflection = message->GetReflection();
        auto doForEach = [&] (const auto& func) {
            for (const auto& node : nodes) {
                func(node);
            }
        };

        switch (field->cpp_type()) {
            case FieldDescriptor::CPPTYPE_INT32:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddInt32(message, field, ConvertToProtobufValue(node, field->default_value_int32()));
                });
                return;
            case FieldDescriptor::CPPTYPE_INT64:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddInt64(message, field, ConvertToProtobufValue(node, field->default_value_int64()));
                });
                return;
            case FieldDescriptor::CPPTYPE_UINT32:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddUInt32(message, field, ConvertToProtobufValue(node, field->default_value_uint32()));
                });
                return;
            case FieldDescriptor::CPPTYPE_UINT64:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddUInt64(message, field, ConvertToProtobufValue(node, field->default_value_uint64()));
                });
                return;
            case FieldDescriptor::CPPTYPE_DOUBLE:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddDouble(message, field, ConvertToProtobufValue(node, field->default_value_double()));
                });
                return;
            case FieldDescriptor::CPPTYPE_FLOAT:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddFloat(
                        message,
                        field,
                        CheckedIntegralCast<float>(ConvertToProtobufValue<double>(node, field->default_value_float())));
                });
                return;
            case FieldDescriptor::CPPTYPE_BOOL:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddBool(message, field, ConvertToProtobufValue(node, field->default_value_bool()));
                });
                return;
            case FieldDescriptor::CPPTYPE_ENUM:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddEnumValue(message, field, ConvertToEnumValue(node, field));
                });
                return;
            case FieldDescriptor::CPPTYPE_STRING:
                doForEach([&] (const INodePtr& node) {
                    reflection->AddString(message, field, ConvertToProtobufValue(node, field->default_value_string()));
                });
                return;
            case FieldDescriptor::CPPTYPE_MESSAGE:
                doForEach([&, options = GetOptionsByPath(path)] (const INodePtr& node) {
                    DeserializeProtobufMessage(
                        *reflection->AddMessage(message, field),
                        ReflectProtobufMessageType(field->message_type()),
                        node,
                        options);
                });
                return;
        }
        ThrowUnsupportedCppType(field->cpp_type(), path);
    }

    static void SetNodeValueByPath(
        TString& nodeString,
        NYPath::TTokenizer& tokenizer,
        const INodePtr& value,
        bool recursive)
    {
        tokenizer.Expect(NYPath::ETokenType::Slash);
        auto root = !nodeString.empty()
            ? ConvertToNode(TYsonStringBuf{nodeString})
            : GetEphemeralNodeFactory()->CreateMap();

        try {
            SyncYPathSet(
                root,
                NYPath::TYPath{tokenizer.GetInput()},
                ConvertToYsonString(value),
                recursive);
            nodeString = ConvertToYsonString(root).ToString();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Cannot set unknown attribute \"%v%v\"",
                tokenizer.GetPrefixPlusToken(),
                tokenizer.GetSuffix()) << ex;
        }
    }

    TProtobufWriterOptions GetOptionsByPath(TStringBuf basePath) const
    {
        TProtobufWriterOptions options = Options_;
        options.UnknownYsonFieldModeResolver = [=, this] (const NYPath::TYPath& path) {
            return Options_.UnknownYsonFieldModeResolver(basePath + path);
        };
        return options;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TComparisonVisitor final
    : public TProtoVisitor<const std::pair<const Message*, const Message*>&>
{
public:
    TComparisonVisitor()
    {
        AllowAsterisk_ = true;
        VisitEverythingAfterPath_ = true;
    }

    bool Equal_ = true;

protected:
    void NotEqual()
    {
        Equal_ = false;
        StopIteration_ = true;
    }

    void OnDescriptorError(
        const std::pair<const Message*, const Message*>& message,
        EVisitReason reason,
        TError error) override
    {
        if (error.GetCode() == EErrorCode::Empty) {
            // Both messages are null.
            return;
        }

        TProtoVisitor::OnDescriptorError(message, reason, std::move(error));
    }

    void OnKeyError(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        std::unique_ptr<NProtoBuf::Message> keyMessage,
        TString key,
        EVisitReason reason,
        TError error) override
    {
        if (error.GetCode() == EErrorCode::MissingKey) {
            // Both fields are equally missing.
            return;
        }
        if (error.GetCode() == EErrorCode::MismatchingKeys) {
            // One present, one missing.
            NotEqual();
            return;
        }

        TProtoVisitor::OnKeyError(
            message,
            fieldDescriptor,
            std::move(keyMessage),
            std::move(key),
            reason,
            std::move(error));
    }

    void VisitRepeatedFieldEntry(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        int index,
        EVisitReason reason) override
    {
        if (PathComplete()
            && fieldDescriptor->type() != NProtoBuf::FieldDescriptor::TYPE_MESSAGE)
        {
            if (CompareRepeatedFieldEntries(
                message.first,
                fieldDescriptor,
                index,
                message.second,
                fieldDescriptor,
                index) != std::partial_ordering::equivalent)
            {
                NotEqual();
            }
            return;
        }

        TProtoVisitor::VisitRepeatedFieldEntry(message, fieldDescriptor, index, reason);
    }

    void OnSizeError(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason,
        TError error) override
    {
        if (error.GetCode() == EErrorCode::MismatchingSize) {
            if (reason == EVisitReason::Path) {
                // The caller wants to pinpoint a specific entry in two arrays of different sizes...
                // let's try!
                auto sizes = ValueOrThrow(TTraits::Combine(
                    TTraits::TSubTraits::GetRepeatedFieldSize(message.first, fieldDescriptor),
                    TTraits::TSubTraits::GetRepeatedFieldSize(message.second, fieldDescriptor),
                    EErrorCode::MismatchingSize));

                // Negative index may result in different parsed values!
                auto errorOrIndexParseResults = TTraits::Combine(
                    ParseCurrentListIndex(sizes.first),
                    ParseCurrentListIndex(sizes.second),
                    EErrorCode::MismatchingSize);


                if (errorOrIndexParseResults.GetCode() == EErrorCode::MismatchingSize) {
                    // Probably just one is out of bounds.
                    NotEqual();
                    return;
                }
                if (errorOrIndexParseResults.GetCode() == EErrorCode::OutOfBounds) {
                    // Equally out of bounds.
                    return;
                }
                auto indexParseResults = ValueOrThrow(errorOrIndexParseResults);

                if (indexParseResults.first.IndexType != EListIndexType::Relative ||
                    indexParseResults.second.IndexType != EListIndexType::Relative)
                {
                    Throw(EErrorCode::MalformedPath,
                        "Unexpected relative path specifier %v",
                        Tokenizer_.GetToken());
                }
                Tokenizer_.Advance();

                if (fieldDescriptor->type() == NProtoBuf::FieldDescriptor::TYPE_MESSAGE) {
                    auto next = TTraits::Combine(
                        TTraits::TSubTraits::GetMessageFromRepeatedField(
                            message.first,
                            fieldDescriptor,
                            indexParseResults.first.Index),
                        TTraits::TSubTraits::GetMessageFromRepeatedField(
                            message.second,
                            fieldDescriptor,
                            indexParseResults.second.Index));
                    VisitMessage(next, EVisitReason::Manual);
                } else {
                    if (CompareRepeatedFieldEntries(
                            message.first,
                            fieldDescriptor,
                            indexParseResults.first.Index,
                            message.second,
                            fieldDescriptor,
                            indexParseResults.second.Index) != std::partial_ordering::equivalent)
                        {
                            NotEqual();
                        }
                }
            } else {
                // Not a specific path request and mismatching size... done.
                NotEqual();
            }

            return;
        }

        TProtoVisitor::OnSizeError(message, fieldDescriptor, reason, std::move(error));
    }

    void OnIndexError(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason,
        TError error) override
    {
        if (error.GetCode() == EErrorCode::OutOfBounds) {
            // Equally misplaced path. Would have been a size error if it were a mismatch.
            return;
        }

        TProtoVisitor::OnIndexError(message, fieldDescriptor, reason, std::move(error));
    }

    void VisitPresentSingularField(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason) override
    {
        if (PathComplete()
            && fieldDescriptor->type() != NProtoBuf::FieldDescriptor::TYPE_MESSAGE)
        {
            if (CompareScalarFields(
                message.first,
                fieldDescriptor,
                message.second,
                fieldDescriptor) != std::partial_ordering::equivalent)
            {
                NotEqual();
            }
            return;
        }

        TProtoVisitor::VisitPresentSingularField(message, fieldDescriptor, reason);
    }

    void VisitMissingSingularField(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason) override
    {
        Y_UNUSED(message);
        Y_UNUSED(fieldDescriptor);
        Y_UNUSED(reason);

        // Both fields are equally missing.
    }

    void OnPresenceError(
        const std::pair<const Message*, const Message*>& message,
        const FieldDescriptor* fieldDescriptor,
        EVisitReason reason,
        TError error) override
    {
        if (error.GetCode() == EErrorCode::MismatchingPresence) {
            if (!PathComplete()
                && fieldDescriptor->type() == NProtoBuf::FieldDescriptor::TYPE_MESSAGE)
            {
                // Try to check that the actual field is absent in both messages.
                auto next = TTraits::Combine(
                    TTraits::TSubTraits::IsSingularFieldPresent(
                        message.first,
                        fieldDescriptor).Value()
                    ? TTraits::TSubTraits::GetMessageFromSingularField(
                        message.first,
                        fieldDescriptor)
                    : nullptr,
                    TTraits::TSubTraits::IsSingularFieldPresent(
                        message.second,
                        fieldDescriptor).Value()
                    ? TTraits::TSubTraits::GetMessageFromSingularField(
                        message.second,
                        fieldDescriptor)
                    : nullptr);
                    VisitMessage(next, EVisitReason::Manual);
            } else {
                // One present, one missing.
                NotEqual();
            }
            return;
        }

        TProtoVisitor::OnPresenceError(message, fieldDescriptor, reason, std::move(error));
    }
}; // TComparisonVisitor

} // namespace

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

bool AreProtoMessagesEqualByPath(
    const google::protobuf::Message& lhs,
    const google::protobuf::Message& rhs,
    const NYPath::TYPath& path)
{
    TComparisonVisitor visitor;
    visitor.Visit(std::pair(&lhs, &rhs), path);
    return visitor.Equal_;
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

void ClearProtobufFieldByPath(
    google::protobuf::Message& message,
    const NYPath::TYPath& path,
    bool skipMissing)
{
    if (path.empty()) {
        // Skip visitor machinery in the simple use case.
        message.Clear();
    } else {
        TClearVisitor(skipMissing).Visit(&message, path);
    }
}

void SetProtobufFieldByPath(
    google::protobuf::Message& message,
    const NYPath::TYPath& path,
    const INodePtr& value,
    const TProtobufWriterOptions& options,
    bool recursive)
{
    if (path.empty()) {
        DeserializeProtobufMessage(
            message,
            ReflectProtobufMessageType(message.GetDescriptor()),
            value,
            options);
    } else {
        Traverse(&message, path, TSetHandler{value, options, recursive});
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NOrm::NAttributes
