#include "consumer.h"

#include <yt/yt/python/yson/serialize.h>
#include <yt/yt/python/yson/pull_object_builder.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/misc/finally.h>

#include <util/stream/str.h>

namespace NYT::NPython {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

Py::Object LoadYsonFromStringBuf(TStringBuf string, const std::optional<TString>& encoding)
{
    auto input = TMemoryInput(string.Data(), string.Size());
    TYsonPullParser parser(&input, EYsonType::Node);
    TPullObjectBuilder builder(&parser, /* alwaysCreateAttributes */ false, encoding);
    return Py::Object(builder.ParseObject().release(), /* owned */ true);
}

////////////////////////////////////////////////////////////////////////////////

TPythonSkiffRecordBuilder::TPythonSkiffRecordBuilder(
    const std::vector<Py::PythonClassObject<TSkiffSchemaPython>>& schemas,
    const std::optional<TString>& encoding)
    : Schemas_(schemas)
    , Encoding_(encoding)
{ }

void TPythonSkiffRecordBuilder::OnBeginRow(ui16 schemaIndex)
{
    if (schemaIndex >= Schemas_.size()) {
        THROW_ERROR_EXCEPTION("Invalid schema index")
            << TErrorAttribute("schema_index", schemaIndex)
            << TErrorAttribute("schema_count", Schemas_.size());
    }

    CurrentSchema_ = Schemas_[schemaIndex];
    CurrentRecord_ = Schemas_[schemaIndex].getCxxObject()->GetSchemaObject()->CreateNewRecord();
}

void TPythonSkiffRecordBuilder::OnEndRow()
{
    Py::Callable classType(TSkiffRecordPython::type());
    Py::PythonClassObject<TSkiffRecordPython> pythonObject(classType.apply(Py::TupleN(CurrentSchema_), Py::Dict()));

    auto cxxObject = pythonObject.getCxxObject();
    cxxObject->SetSkiffRecordObject(CurrentRecord_);
    Objects_.push(pythonObject);
}

void TPythonSkiffRecordBuilder::OnStringScalar(TStringBuf value, ui16 columnId)
{
    Py::Bytes bytes(value.begin(), value.size());
    // TODO(ignat): remove this copy/paste.
    if (Encoding_) {
        auto decodedString = Py::Object(
            PyUnicode_FromEncodedObject(*bytes, Encoding_->data(), "strict"),
            /* owned */ true);
#if PY_MAJOR_VERSION < 3
        auto utf8String = Py::Object(
            PyUnicode_AsUTF8String(*decodedString),
            /* owned */ true);
        CurrentRecord_->SetField(columnId, utf8String);
#else
        CurrentRecord_->SetField(columnId, decodedString);
#endif
    } else {
        CurrentRecord_->SetField(columnId, bytes);
    }
}

void TPythonSkiffRecordBuilder::OnInt64Scalar(i64 value, ui16 columnId)
{
    Py::LongLong field(value);
    CurrentRecord_->SetField(columnId, field);
}

void TPythonSkiffRecordBuilder::OnUint64Scalar(ui64 value, ui16 columnId)
{
    Py::LongLong field(value);
    CurrentRecord_->SetField(columnId, field);
}

void TPythonSkiffRecordBuilder::OnDoubleScalar(double value, ui16 columnId)
{
    Py::Float field(value);
    CurrentRecord_->SetField(columnId, field);
}

void TPythonSkiffRecordBuilder::OnBooleanScalar(bool value, ui16 columnId)
{
    Py::Boolean field(value);
    CurrentRecord_->SetField(columnId, field);
}

void TPythonSkiffRecordBuilder::OnEntity(ui16 columnId)
{
    CurrentRecord_->SetField(columnId, Py::None());
}

void TPythonSkiffRecordBuilder::OnYsonString(TStringBuf value, ui16 columnId)
{
    CurrentRecord_->SetField(columnId, LoadYsonFromStringBuf(value, Encoding_));
}

void TPythonSkiffRecordBuilder::OnOtherColumns(TStringBuf value)
{
    auto object = LoadYsonFromStringBuf(value, Encoding_);
    auto items = Py::Object(PyDict_Items(*object), true);
    auto iterator = CreateIterator(items);
    while (auto* item = PyIter_Next(*iterator)) {
        auto itemGuard = Finally([item] { Py::_XDECREF(item); });

        auto key = Py::Object(PyTuple_GetItem(item, 0), false);
        auto value = Py::Object(PyTuple_GetItem(item, 1), false);

        auto mapKey = ConvertStringObjectToString(key);
        CurrentRecord_->SetOtherField(mapKey, value);
    }
}

Py::Object TPythonSkiffRecordBuilder::ExtractObject()
{
    auto object = Objects_.front();
    Objects_.pop();
    return object;
}

bool TPythonSkiffRecordBuilder::HasObject() const
{
    return !Objects_.empty();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
