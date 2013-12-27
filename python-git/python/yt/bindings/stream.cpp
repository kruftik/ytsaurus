#include "stream.h"

#include "gil.h"

#include <util/stream/input.h>
#include <util/stream/output.h>

#include <contrib/libs/pycxx/Objects.hxx>

#include <string>
#include <iostream>

namespace NYT {
namespace NPython {

TInputStreamWrap::TInputStreamWrap(const Py::Object& inputStream)
    : InputStream_(inputStream)
{ }
    
TInputStreamWrap::~TInputStreamWrap() throw()
{ }

size_t TInputStreamWrap::DoRead(void* buf, size_t len)
{
    TGILLock lock;

    auto args = Py::TupleN(Py::Int(static_cast<long>(len)));
    Py::Object result = InputStream_.callMemberFunction("read", args);
    if (!result.isString()) {
        throw Py::RuntimeError("Read returns non-string object");
    }
    auto data = PyString_AsString(*result);
    auto length = PyString_Size(*result);
    std::copy(data, data + length, (char*)buf);
    return length;
}


TOutputStreamWrap::TOutputStreamWrap(const Py::Object& outputStream)
    : OutputStream_(outputStream)
{ }

TOutputStreamWrap::~TOutputStreamWrap() throw()
{ }

void TOutputStreamWrap::DoWrite(const void* buf, size_t len) {
    TGILLock lock;
    //std::string str((const char*)buf, len);
    OutputStream_.callMemberFunction("write", Py::TupleN(Py::String((const char*)buf, len)));
}

} // namespace NPython
} // namespace NYT
