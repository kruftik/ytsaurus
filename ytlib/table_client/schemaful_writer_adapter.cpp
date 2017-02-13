#include "schemaful_writer_adapter.h"
#include "name_table.h"
#include "schema.h"
#include "schemaful_writer.h"
#include "schemaless_row_reorderer.h"
#include "schemaless_writer.h"

#include <yt/core/misc/chunked_memory_pool.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TSchemafulWriterAdapter)

class TSchemafulWriterAdapter
    : public ISchemafulWriter
{
public:
    explicit TSchemafulWriterAdapter(ISchemalessWriterPtr underlyingWriter)
        : UnderlyingWriter_(std::move(underlyingWriter))
    { }

    virtual bool Write(const TRange<TUnversionedRow>& rows) override
    {
        return UnderlyingWriter_->Write(rows);
    }

    virtual TFuture<void> Close() override
    {
        return UnderlyingWriter_->Close();
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return UnderlyingWriter_->GetReadyEvent();
    }

private:
    const ISchemalessWriterPtr UnderlyingWriter_;

};

DEFINE_REFCOUNTED_TYPE(TSchemafulWriterAdapter)

ISchemafulWriterPtr CreateSchemafulWriterAdapter(ISchemalessWriterPtr underlyingWriter)
{
    return New<TSchemafulWriterAdapter>(underlyingWriter);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
