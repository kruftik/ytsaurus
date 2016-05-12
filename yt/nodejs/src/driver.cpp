#include "driver.h"
#include "config.h"
#include "error.h"
#include "future.h"
#include "input_stack.h"
#include "node.h"
#include "output_stack.h"
#include "uv_invoker.h"

#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/ytlib/driver/config.h>
#include <yt/ytlib/driver/driver.h>

#include <yt/ytlib/formats/format.h>

#include <yt/core/actions/bind_helpers.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/format.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/node.h>

#include <util/memory/tempbuf.h>

#include <string>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

COMMON_V8_USES

using namespace NRpc;
using namespace NYTree;
using namespace NDriver;
using namespace NFormats;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

namespace {

NLogging::TLogger Logger("HttpProxy");

static Persistent<String> DescriptorName;
static Persistent<String> DescriptorInputType;
static Persistent<String> DescriptorInputTypeAsInteger;
static Persistent<String> DescriptorOutputType;
static Persistent<String> DescriptorOutputTypeAsInteger;
static Persistent<String> DescriptorIsVolatile;
static Persistent<String> DescriptorIsHeavy;

class TResponseParametersConsumer
    : public TForwardingYsonConsumer
{
public:
    TResponseParametersConsumer(const Persistent<Function>& callback)
        : Callback_(callback)
    {
        THREAD_AFFINITY_IS_V8();
    }

    ~TResponseParametersConsumer()
    {
        THREAD_AFFINITY_IS_V8();
    }

    virtual void OnMyKeyedItem(const TStringBuf& keyRef) override
    {
        THREAD_AFFINITY_IS_ANY();

        auto builder = CreateBuilderFromFactory(CreateEphemeralNodeFactory());
        auto builder_ = builder.get();

        builder_->BeginTree();
        Forward(
            builder_,
            BIND(&TResponseParametersConsumer::DoSavePair,
                this,
                Passed(Stroka(keyRef)),
                Passed(std::move(builder))));
    }

private:
    const Persistent<Function>& Callback_;

    void DoSavePair(Stroka key, std::unique_ptr<ITreeBuilder> builder)
    {
        auto pair = std::make_pair(std::move(key), builder->EndTree());
        auto future =
            BIND([this, pair = std::move(pair)] () {
                THREAD_AFFINITY_IS_V8();
                HandleScope scope;

                auto keyHandle = String::New(pair.first.c_str());
                auto valueHandle = TNodeWrap::ConstructorTemplate->GetFunction()->NewInstance();
                TNodeWrap::Unwrap(valueHandle)->SetNode(pair.second);
                Invoke(Callback_, keyHandle, valueHandle);
            })
            .AsyncVia(GetUVInvoker())
            .Run();

        // Await for the future, see YT-1095.
        WaitFor(std::move(future));
    }
};

class TExecuteRequest
{
private:
    TDriverWrap* Wrap_;
    TDriverRequest Request_;

    std::unique_ptr<TNodeJSInputStack> InputStack_;
    std::unique_ptr<TNodeJSOutputStack> OutputStack_;

    Persistent<Function> ExecuteCallback_;
    Persistent<Function> ParameterCallback_;

    TResponseParametersConsumer ResponseParametersConsumer_;

    NTracing::TTraceContext TraceContext_;

public:
    TExecuteRequest(
        TDriverWrap* wrap,
        TInputStreamWrap* inputStream,
        TOutputStreamWrap* outputStream,
        Handle<Function> executeCallback,
        Handle<Function> parameterCallback)
        : Wrap_(wrap)
        , InputStack_(std::make_unique<TNodeJSInputStack>(inputStream))
        , OutputStack_(std::make_unique<TNodeJSOutputStack>(outputStream))
        , ExecuteCallback_(Persistent<Function>::New(executeCallback))
        , ParameterCallback_(Persistent<Function>::New(parameterCallback))
        , ResponseParametersConsumer_(ParameterCallback_)
    {
        THREAD_AFFINITY_IS_V8();

        Wrap_->Ref();
    }

    ~TExecuteRequest()
    {
        THREAD_AFFINITY_IS_V8();

        ExecuteCallback_.Dispose();
        ExecuteCallback_.Clear();

        ParameterCallback_.Dispose();
        ParameterCallback_.Clear();

        Wrap_->Unref();
    }

    void SetCommand(
        Stroka commandName,
        Stroka authenticatedUser,
        INodePtr parameters,
        ui64 requestId)
    {
        Request_.Id = requestId;
        Request_.CommandName = std::move(commandName);
        Request_.AuthenticatedUser = std::move(authenticatedUser);
        Request_.Parameters = parameters->AsMap();

        auto trace = Request_.Parameters->FindChild("trace");
        if (trace && ConvertTo<bool>(trace)) {
            TraceContext_ = NTracing::CreateRootTraceContext();
            if (requestId) {
                TraceContext_ = NTracing::TTraceContext(
                    requestId,
                    TraceContext_.GetSpanId(),
                    TraceContext_.GetParentSpanId());
            }
        }
    }

    void SetInputCompression(ECompression compression)
    {
        InputStack_->AddCompression(compression);
    }

    void SetOutputCompression(ECompression compression)
    {
        OutputStack_->AddCompression(compression);
    }

    Handle<Value> Run(std::unique_ptr<TExecuteRequest> this_)
    {
        THREAD_AFFINITY_IS_V8();

        // TODO(sandello): YASSERTT
        YCHECK(this == this_.get());

        auto compressionInvoker =
            NChunkClient::TDispatcher::Get()->GetCompressionPoolInvoker();
        Request_.InputStream = CreateAsyncAdapter(InputStack_.get(), compressionInvoker);
        Request_.OutputStream = CreateAsyncAdapter(OutputStack_.get(), compressionInvoker);
        Request_.ResponseParametersConsumer = &ResponseParametersConsumer_;
 
        TFuture<void> future;
        auto wrappedFuture = TFutureWrap::ConstructorTemplate->GetFunction()->NewInstance();

        if (Y_LIKELY(!Wrap_->IsEcho())) {
            NTracing::TTraceContextGuard guard(TraceContext_);
            future = Wrap_->GetDriver()->Execute(Request_);
       } else {
            future =
                BIND([this] () {
                    TTempBuf buffer;
                    auto inputStream = CreateSyncAdapter(Request_.InputStream);
                    auto outputStream = CreateSyncAdapter(Request_.OutputStream);

                    while (size_t length = inputStream->Load(buffer.Data(), buffer.Size())) {
                        outputStream->Write(buffer.Data(), length);
                    }
                })
                .AsyncVia(compressionInvoker)
                .Run();
        }

        future.Subscribe(
            BIND(&TExecuteRequest::OnResponse, Owned(this_.release()))
                .Via(GetUVInvoker()));

        TFutureWrap::Unwrap(wrappedFuture)->SetFuture(std::move(future));

        return wrappedFuture;
    }

private:
    void OnResponse(const TErrorOr<void>& response)
    {
        THREAD_AFFINITY_IS_V8();

        try {
            OutputStack_->Finish();
        } catch (const std::exception& ex) {
            LOG_DEBUG(TError(ex), "Ignoring exception while closing driver output stream");
        }

        // XXX(sandello): We cannot represent ui64 precisely in V8, because there
        // is no native ui64 integer type. So we convert ui64 to double (v8::Number)
        // to precisely represent all integers up to 2^52
        // (see http://en.wikipedia.org/wiki/Double_precision).
        double bytesIn = InputStack_->GetBytes();
        InputStack_.reset();
        double bytesOut = OutputStack_->GetBytes();
        OutputStack_.reset();

        Invoke(
            ExecuteCallback_,
            ConvertErrorToV8(response),
            Number::New(bytesIn),
            Number::New(bytesOut));
    }
};

// Assuming presence of outer HandleScope.
Local<Object> ConvertCommandDescriptorToV8Object(const TCommandDescriptor& descriptor)
{
    Local<Object> result = Object::New();
    result->Set(
        DescriptorName,
        String::New(descriptor.CommandName.c_str()),
        v8::ReadOnly);
    result->Set(
        DescriptorInputType,
        String::New(to_lower(ToString(descriptor.InputType)).c_str()),
        v8::ReadOnly);
    result->Set(
        DescriptorInputTypeAsInteger,
        Integer::New(static_cast<int>(descriptor.InputType)),
        static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum));
    result->Set(
        DescriptorOutputType,
        String::New(to_lower(ToString(descriptor.OutputType)).c_str()),
        v8::ReadOnly);
    result->Set(
        DescriptorOutputTypeAsInteger,
        Integer::New(static_cast<int>(descriptor.OutputType)),
        static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum));
    result->Set(
        DescriptorIsVolatile,
        Boolean::New(descriptor.IsVolatile),
        v8::ReadOnly);
    result->Set(
        DescriptorIsHeavy,
        Boolean::New(descriptor.IsHeavy),
        v8::ReadOnly);
    return result;
}

// Assuming presence of outer HandleScope.
template <class E>
void ExportEnumeration(
    const Handle<Object>& target,
    const char* name)
{
    auto values = TEnumTraits<E>::GetDomainValues();
    Local<Array> mapping = Array::New();

    for (auto value : values) {
        auto key = Stroka::Join(name, "_", TEnumTraits<E>::FindLiteralByValue(value)->data());
        auto keyHandle = String::NewSymbol(key.c_str());
        auto valueHandle = Integer::New(static_cast<int>(value));
        target->Set(
            keyHandle,
            valueHandle,
            static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
        mapping->Set(valueHandle, keyHandle);
    }
    target->Set(String::NewSymbol(name), mapping);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

Persistent<FunctionTemplate> TDriverWrap::ConstructorTemplate;

TDriverWrap::TDriverWrap(bool echo, Handle<Object> configObject)
    : node::ObjectWrap()
    , Echo(echo)
{
    THREAD_AFFINITY_IS_V8();

    INodePtr configNode = ConvertV8ValueToNode(configObject);
    if (!configNode) {
        Message = "Error converting from V8 to YSON";
        return;
    }

    NNodeJS::THttpProxyConfigPtr config;
    try {
        // Qualify namespace to avoid collision with class method New().
        config = NYT::New<NYT::NNodeJS::THttpProxyConfig>();
        config->Load(configNode);
    } catch (const std::exception& ex) {
        Message = Format("Error loading configuration\n%v", ex.what());
        return;
    }

    try {
        Driver = CreateDriver(config->Driver);
    } catch (const std::exception& ex) {
        Message = Format("Error initializing driver instance\n%v", ex.what());
        return;
    }
}

TDriverWrap::~TDriverWrap()
{
    THREAD_AFFINITY_IS_V8();
}

////////////////////////////////////////////////////////////////////////////////

void TDriverWrap::Initialize(Handle<Object> target)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    DescriptorName = NODE_PSYMBOL("name");
    DescriptorInputType = NODE_PSYMBOL("input_type");
    DescriptorInputTypeAsInteger = NODE_PSYMBOL("input_type_as_integer");
    DescriptorOutputType = NODE_PSYMBOL("output_type");
    DescriptorOutputTypeAsInteger = NODE_PSYMBOL("output_type_as_integer");
    DescriptorIsVolatile = NODE_PSYMBOL("is_volatile");
    DescriptorIsHeavy = NODE_PSYMBOL("is_heavy");

    ConstructorTemplate = Persistent<FunctionTemplate>::New(
        FunctionTemplate::New(TDriverWrap::New));

    ConstructorTemplate->InstanceTemplate()->SetInternalFieldCount(1);
    ConstructorTemplate->SetClassName(String::NewSymbol("TDriverWrap"));

    NODE_SET_PROTOTYPE_METHOD(ConstructorTemplate, "Execute", TDriverWrap::Execute);
    NODE_SET_PROTOTYPE_METHOD(ConstructorTemplate, "FindCommandDescriptor", TDriverWrap::FindCommandDescriptor);
    NODE_SET_PROTOTYPE_METHOD(ConstructorTemplate, "GetCommandDescriptors", TDriverWrap::GetCommandDescriptors);

    target->Set(
        String::NewSymbol("TDriverWrap"),
        ConstructorTemplate->GetFunction());

    ExportEnumeration<ECompression>(target, "ECompression");
    ExportEnumeration<EDataType>(target, "EDataType");
}

bool TDriverWrap::HasInstance(Handle<Value> value)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    return
        value->IsObject() &&
        ConstructorTemplate->HasInstance(value->ToObject());
}

Handle<Value> TDriverWrap::New(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    YCHECK(args.Length() == 2);

    EXPECT_THAT_IS(args[0], Boolean);
    EXPECT_THAT_IS(args[1], Object);

    TDriverWrap* wrap = nullptr;
    try {
        wrap = new TDriverWrap(
            args[0]->BooleanValue(),
            args[1].As<Object>());
        wrap->Wrap(args.This());

        if (wrap->Driver) {
            return args.This();
        } else {
            return ThrowException(Exception::Error(String::New(~wrap->Message)));
        }
    } catch (const std::exception& ex) {
        if (wrap) {
            delete wrap;
        }

        return ThrowException(Exception::Error(String::New(ex.what())));
    }
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TDriverWrap::FindCommandDescriptor(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    // Unwrap object.
    TDriverWrap* driver = ObjectWrap::Unwrap<TDriverWrap>(args.This());

    // Validate arguments.
    YCHECK(args.Length() == 1);

    EXPECT_THAT_IS(args[0], String);

    // Unwrap arguments.
    String::Utf8Value commandNameValue(args[0]);
    Stroka commandName(*commandNameValue, commandNameValue.length());

    // Do the work.
    return scope.Close(driver->DoFindCommandDescriptor(commandName));
}

Handle<Value> TDriverWrap::DoFindCommandDescriptor(const Stroka& commandName)
{
    auto maybeDescriptor = Driver->FindCommandDescriptor(commandName);
    if (maybeDescriptor) {
        return ConvertCommandDescriptorToV8Object(*maybeDescriptor);
    } else {
        return v8::Null();
    }
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TDriverWrap::GetCommandDescriptors(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    // Unwrap.
    TDriverWrap* driver = ObjectWrap::Unwrap<TDriverWrap>(args.This());

    // Validate arguments.
    YCHECK(args.Length() == 0);

    // Do the work.
    return scope.Close(driver->DoGetCommandDescriptors());
}

Handle<Value> TDriverWrap::DoGetCommandDescriptors()
{
    Local<Array> result = Array::New();

    auto descriptors = Driver->GetCommandDescriptors();
    for (const auto& descriptor : descriptors) {
        Local<Object> resultItem = ConvertCommandDescriptorToV8Object(descriptor);
        result->Set(result->Length(), resultItem);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

Handle<Value> TDriverWrap::Execute(const Arguments& args)
{
    THREAD_AFFINITY_IS_V8();
    HandleScope scope;

    // Validate arguments.
    YCHECK(args.Length() == 10);

    EXPECT_THAT_IS(args[0], String); // CommandName
    EXPECT_THAT_IS(args[1], String); // AuthenticatedUser

    EXPECT_THAT_HAS_INSTANCE(args[2], TInputStreamWrap); // InputStream
    EXPECT_THAT_IS(args[3], Uint32); // InputCompression

    EXPECT_THAT_HAS_INSTANCE(args[4], TOutputStreamWrap); // OutputStream
    EXPECT_THAT_IS(args[5], Uint32); // OutputCompression

    EXPECT_THAT_HAS_INSTANCE(args[6], TNodeWrap); // Parameters

    EXPECT_THAT_IS(args[8], Function); // ExecuteCallback
    EXPECT_THAT_IS(args[9], Function); // ParameterCallback

    // Unwrap arguments.
    auto* host = ObjectWrap::Unwrap<TDriverWrap>(args.This());

    String::AsciiValue commandName(args[0]);
    String::AsciiValue authenticatedUser(args[1]);

    auto* inputStream = ObjectWrap::Unwrap<TInputStreamWrap>(args[2].As<Object>());
    auto inputCompression = (ECompression)args[3]->Uint32Value();

    auto* outputStream = ObjectWrap::Unwrap<TOutputStreamWrap>(args[4].As<Object>());
    auto outputCompression = (ECompression)args[5]->Uint32Value();

    auto parameters = TNodeWrap::UnwrapNode(args[6]);

    ui64 requestId = 0;

    if (node::Buffer::HasInstance(args[7])) {
        const char* buffer = node::Buffer::Data(args[7].As<Object>());
        size_t length = node::Buffer::Length(args[7].As<Object>());
        if (length == 8) {
            requestId = __builtin_bswap64(*(ui64*)buffer);
        }
    }

    Local<Function> executeCallback = args[8].As<Function>();
    Local<Function> parameterCallback = args[9].As<Function>();

    // Build an atom of work.
    YCHECK(parameters);
    YCHECK(parameters->GetType() == ENodeType::Map);

    std::unique_ptr<TExecuteRequest> request(new TExecuteRequest(
        host,
        inputStream,
        outputStream,
        executeCallback,
        parameterCallback));

    request->SetCommand(
        Stroka(*commandName, commandName.length()),
        Stroka(*authenticatedUser, authenticatedUser.length()),
        std::move(parameters),
        requestId);

    request->SetInputCompression(inputCompression);
    request->SetOutputCompression(outputCompression);

    auto request_ = request.get();
    return request_->Run(std::move(request));
}

////////////////////////////////////////////////////////////////////////////////

IDriverPtr TDriverWrap::GetDriver() const
{
    return Driver;
}

const bool TDriverWrap::IsEcho() const
{
    return Echo;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeJS
} // namespace NYT

