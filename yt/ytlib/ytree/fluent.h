#pragma once

#include "public.h"
#include "yson_consumer.h"
#include "yson_writer.h"
#include "tree_visitor.h"

#include <ytlib/actions/callback.h>
#include <ytlib/actions/bind_helpers.h> // For TVoid
#include <ytlib/misc/foreach.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TFluentYsonBuilder
    : private TNonCopyable
{
public:
    class TFluentAny;
    template <class TParent> class TAny;
    template <class TParent> class TToAttributes;
    template <class TParent> class TAttributes;
    class TListCore;
    template <class TParent> class TList;
    class TMapCore;
    template <class TParent> class TMap;

    template <class TParent>
    class TFluentBase
    {
    public:
        TParent Do(TYsonProducer producer)
        {
            producer.Run(this->Consumer);
            return this->Parent;
        }

    protected:
        TFluentBase(IYsonConsumer* consumer, const TParent& parent)
            : Consumer(consumer)
            , Parent(parent)
        { }

        IYsonConsumer* Consumer;
        TParent Parent;

    };

    template <class TParent>
    class TAny
        : public TFluentBase<TParent>
    {
    public:
        typedef TAny<TParent> TThis;

        TAny(IYsonConsumer* consumer, const TParent& parent, bool hasAttributes)
            : TFluentBase<TParent>(consumer, parent)
            , HasAttributes(hasAttributes)
        { }

        TParent Scalar(const Stroka& value)
        {
            this->Consumer->OnStringScalar(value, HasAttributes);
            return this->Parent;
        }

        // A stupid language with a stupid type system.
        TParent Scalar(const char* value)
        {
            return Scalar(Stroka(value));
        }

        TParent Scalar(i32 value)
        {
            return Scalar(static_cast<i64>(value));
        }

        TParent Scalar(ui32 value)
        {
            return Scalar(static_cast<i64>(value));
        }

        TParent Scalar(i64 value)
        {
            this->Consumer->OnInt64Scalar(value, HasAttributes);
            return this->Parent;
        }

        TParent Scalar(float value)
        {
            return Scalar(static_cast<double>(value));
        }

        TParent Scalar(double value)
        {
            this->Consumer->OnDoubleScalar(value, HasAttributes);
            return this->Parent;
        }

        TParent Scalar(bool value)
        {
            return Scalar(value ? Stroka("true") : Stroka("false"));
        }

        TParent Entity()
        {
            this->Consumer->OnEntity(HasAttributes);
            return this->Parent;
        }

        //TODO(panin): rename
        TParent OnNode(INodePtr node)
        {
            VisitTree(~node, this->Consumer);
        }

        TList<TParent> BeginList()
        {
            this->Consumer->OnBeginList();
            return TList<TParent>(this->Consumer, this->Parent, HasAttributes);
        }

        template <class TFunc>
        TParent DoList(const TFunc& func)
        {
            this->Consumer->OnBeginList();
            func(TList<TVoid>(this->Consumer));
            this->Consumer->OnEndList();
            return this->Parent;
        }

        template <class TFunc, class TIterator>
        TParent DoListFor(const TIterator& begin, const TIterator& end, const TFunc& func)
        {
            this->Consumer->OnBeginList();
            for (auto current = begin; current != end; ++current) {
                func(TList<TVoid>(this->Consumer), current);
            }
            this->Consumer->OnEndList();
            return this->Parent;
        }

        template <class TFunc, class TCollection>
        TParent DoListFor(const TCollection& collection, const TFunc& func)
        {
            this->Consumer->OnBeginList();
            FOREACH (const auto& item, collection) {
                func(TList<TVoid>(this->Consumer), item);
            }
            this->Consumer->OnEndList();
            return this->Parent;
        }

        TMap<TParent> BeginMap()
        {
            this->Consumer->OnBeginMap();
            return TMap<TParent>(this->Consumer, this->Parent, HasAttributes);
        }

        template <class TFunc>
        TParent DoMap(const TFunc& func)
        {
            this->Consumer->OnBeginMap();
            func(TMap<TVoid>(this->Consumer));
            this->Consumer->OnEndMap();
            return this->Parent;
        }

        template <class TFunc, class TIterator>
        TParent DoMapFor(const TIterator& begin, const TIterator& end, const TFunc& func)
        {
            this->Consumer->OnBeginMap();
            for (auto current = begin; current != end; ++current) {
                func(TMap<TVoid>(this->Consumer), current);
            }
            this->Consumer->OnEndMap();
            return this->Parent;
        }

        template <class TFunc, class TCollection>
        TParent DoMapFor(const TCollection& collection, const TFunc& func)
        {
            this->Consumer->OnBeginMap();
            FOREACH (const auto& item, collection) {
                func(TMap<TVoid>(this->Consumer), item);
            }
            this->Consumer->OnEndMap();
            return this->Parent;
        }

        TAny< TToAttributes<TParent> > WithAttributes()
        {
            return TAny< TToAttributes<TParent> >(
                this->Consumer,
                TToAttributes<TParent>(this->Consumer, this->Parent),
                true);
        }

        bool HasAttributes;

    };

    template <class TParent>
    class TToAttributes
        : public TFluentBase<TParent>
    {
    public:
        TToAttributes(IYsonConsumer* consumer, const TParent& parent)
            : TFluentBase<TParent>(consumer, parent)
        { }

        TAttributes<TParent> BeginAttributes()
        {
            this->Consumer->OnBeginAttributes();
            return TAttributes<TParent>(this->Consumer, this->Parent);
        }
    };

    template <class TParent>
    class TAttributes
        : public TFluentBase<TParent>
    {
    public:
        typedef TAttributes<TParent> TThis;

        TAttributes(IYsonConsumer* consumer, const TParent& parent)
            : TFluentBase<TParent>(consumer, parent)
        { }

        TAny<TThis> Item(const Stroka& key)
        {
            this->Consumer->OnAttributesItem(key);
            return TAny<TThis>(this->Consumer, *this, false);
        }

        TParent EndAttributes()
        {
            this->Consumer->OnEndAttributes();
            return this->Parent;
        }
    };

    template <class TParent = TVoid>
    class TList
        : public TFluentBase<TParent>
    {
    public:
        typedef TList<TParent> TThis;

        TList(IYsonConsumer* consumer, const TParent& parent = TParent(), bool hasAttributes = false)
            : TFluentBase<TParent>(consumer, parent)
            , HasAttributes(hasAttributes)
        { }

        TAny<TThis> Item()
        {
            this->Consumer->OnListItem();
            return TAny<TThis>(this->Consumer, *this, false);
        }

        template <class TFunc>
        TThis& Do(const TFunc& func)
        {
            func(TList(this->Consumer));
            return *this;
        }

        template <class TFunc>
        TThis& DoIf(bool condition, const TFunc& func)
        {
            if (condition) {
                func(TList(this->Consumer));
            }
            return *this;
        }

        template <class TFunc, class TIterator>
        TThis& DoFor(const TIterator& begin, const TIterator& end, const TFunc& func)
        {
            for (auto current = begin; current != end; ++current) {
                func(TList<TVoid>(this->Consumer), current);
            }
            return *this;
        }

        template <class TFunc, class TCollection>
        TThis& DoFor(const TCollection& collection, const TFunc& func)
        {
            FOREACH (const auto& item, collection) {
                func(TList(this->Consumer), item);
            }
            return *this;
        }

        TParent EndList()
        {
            this->Consumer->OnEndList(HasAttributes);
            return this->Parent;
        }

    private:
        bool HasAttributes;

    };

    template <class TParent = TVoid>
    class TMap
        : public TFluentBase<TParent>
    {
    public:
        typedef TMap<TParent> TThis;

        TMap(IYsonConsumer* consumer, const TParent& parent = TParent(), bool hasAttributes = false)
            : TFluentBase<TParent>(consumer, parent)
            , HasAttributes(hasAttributes)
        { }

        TAny<TThis> Item(const Stroka& key)
        {
            this->Consumer->OnMapItem(key);
            return TAny<TThis>(this->Consumer, *this, false);
        }

        template <class TFunc>
        TThis& Do(const TFunc& func)
        {
            func(TMap(this->Consumer));
            return *this;
        }

        template <class TFunc>
        TThis& DoIf(bool condition, const TFunc& func)
        {
            if (condition) {
                func(TMap(this->Consumer));
            }
            return *this;
        }

        template <class TFunc, class TIterator>
        TThis& DoFor(const TIterator& begin, const TIterator& end, const TFunc& func)
        {
            for (auto current = begin; current != end; ++current) {
                func(TMap(this->Consumer), current);
            }
            return *this;
        }

        template <class TFunc, class TCollection>
        TThis& DoFor(const TCollection& collection, const TFunc& func)
        {
            FOREACH (const auto& item, collection) {
                func(TMap(this->Consumer), item);
            }
            return *this;
        }

        //TODO(panin): forbid this call for TParent = TVoid
        TParent EndMap()
        {
            this->Consumer->OnEndMap(HasAttributes);
            return this->Parent;
        }
    
    private:
        bool HasAttributes;

    };

};

typedef TFluentYsonBuilder::TList<TVoid>   TFluentList;
typedef TFluentYsonBuilder::TMap<TVoid>   TFluentMap;

////////////////////////////////////////////////////////////////////////////////

inline TFluentYsonBuilder::TAny<TVoid> BuildYsonFluently(IYsonConsumer* consumer)
{
    return TFluentYsonBuilder::TAny<TVoid>(consumer, TVoid(), false);
}

inline TFluentList BuildYsonListFluently(IYsonConsumer* consumer)
{
    return TFluentList(consumer);
}

inline TFluentMap BuildYsonMapFluently(IYsonConsumer* consumer)
{
    return TFluentMap(consumer);
}

////////////////////////////////////////////////////////////////////////////////

class TFluentYsonConsumer
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TFluentYsonConsumer> TPtr;

    TFluentYsonConsumer(EYsonFormat format)
        : Writer(&Output, format)
    { }

    TYson GetYson() const
    {
        return Output.Str();
    }

    IYsonConsumer* GetConsumer()
    {
        return &Writer;
    }

private:
    TStringStream Output;
    TYsonWriter Writer;
};

class TFluentYsonHolder
{
public:
    TFluentYsonHolder(TFluentYsonConsumer::TPtr consumer)
        : Consumer(consumer)
    { }

    operator TYson() const
    {
        return Consumer->GetYson();
    }

private:
    TFluentYsonConsumer::TPtr Consumer;
};

inline TFluentYsonBuilder::TAny<TFluentYsonHolder> BuildYsonFluently(
    EYsonFormat format = EYsonFormat::Binary)
{
    auto consumer = New<TFluentYsonConsumer>(format);
    TFluentYsonHolder holder(consumer);
    return TFluentYsonBuilder::TAny<TFluentYsonHolder>(
        consumer->GetConsumer(),
        holder,
        false);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

