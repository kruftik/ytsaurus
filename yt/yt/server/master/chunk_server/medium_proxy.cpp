#include "chunk_manager.h"
#include "config.h"
#include "medium.h"
#include "medium_proxy.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/master/object_server/object_detail.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NChunkServer {

using namespace NYTree;
using namespace NYson;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TMediumProxy
    : public TNonversionedObjectProxyBase<TMedium>
{
public:
    using TNonversionedObjectProxyBase::TNonversionedObjectProxyBase;

private:
    using TBase = TNonversionedObjectProxyBase<TMedium>;

    void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Name)
            .SetWritable(true)
            .SetReplicated(true)
            .SetMandatory(true));
        descriptors->push_back(EInternedAttributeKey::Index);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Transient)
            .SetReplicated(true)
            .SetMandatory(true));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Cache)
            .SetReplicated(true)
            .SetMandatory(true));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Priority)
            .SetWritable(true)
            .SetReplicated(true));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Config)
            .SetWritable(true)
            .SetReplicated(true));
    }

    bool GetBuiltinAttribute(TInternedAttributeKey key, NYson::IYsonConsumer* consumer) override
    {
        const auto* medium = GetThisImpl();

        switch (key) {
            case EInternedAttributeKey::Name:
                BuildYsonFluently(consumer)
                    .Value(medium->GetName());
                return true;

            case EInternedAttributeKey::Index:
                BuildYsonFluently(consumer)
                    .Value(medium->GetIndex());
                return true;

            case EInternedAttributeKey::Transient:
                BuildYsonFluently(consumer)
                    .Value(medium->GetTransient());
                return true;

            case EInternedAttributeKey::Cache:
                BuildYsonFluently(consumer)
                    .Value(medium->GetCache());
                return true;

            case EInternedAttributeKey::Priority:
                BuildYsonFluently(consumer)
                    .Value(medium->GetPriority());
                return true;

            case EInternedAttributeKey::Config:
                BuildYsonFluently(consumer)
                    .Value(medium->Config());
                return true;

            default:
                break;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    bool SetBuiltinAttribute(TInternedAttributeKey key, const TYsonString& value) override
    {
        auto* medium = GetThisImpl();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        switch (key) {
            case EInternedAttributeKey::Name: {
                auto newName = ConvertTo<TString>(value);
                chunkManager->RenameMedium(medium, newName);
                return true;
            }

            case EInternedAttributeKey::Priority: {
                auto newPriority = ConvertTo<int>(value);
                chunkManager->SetMediumPriority(medium, newPriority);
                return true;
            }

            case EInternedAttributeKey::Config: {
                auto config = ConvertTo<TMediumConfigPtr>(value);
                chunkManager->SetMediumConfig(medium, std::move(config));
                return true;
            }

            default:
                break;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }
};

IObjectProxyPtr CreateMediumProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TMedium* medium)
{
    return New<TMediumProxy>(bootstrap, metadata, medium);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
