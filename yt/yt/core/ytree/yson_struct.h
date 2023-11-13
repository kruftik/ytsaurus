#pragma once

#include "node.h"
#include "yson_serialize_common.h"

#include <yt/yt/core/misc/error.h>
#include <yt/yt/core/misc/mpl.h>
#include <yt/yt/core/misc/property.h>

#include <yt/yt/core/yson/public.h>
#include <yt/yt/library/syncmap/map.h>

#include <util/generic/algorithm.h>

#include <functional>
#include <optional>

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

//! Base class for structs that are meant to be serialized to or deserialized from YSON.
/*!
 * Usually this class is used for various configs.
 * To use it inherit TYsonStruct and add `REGISTER_YSON_STRUCT(TYourClass)` to your class declaration
 * for ref-counted struct or inherit TYsonStructLite and add `REGISTER_YSON_STRUCT_LITE(TYourClass);`
 * for non-ref-counted struct. Then configure fields in static method TYourClass::Register(TRegistrar registrar).
 * Various examples can be found in yt/yt/core/ytree/unittests/yson_struct_ut.cpp
 * Machinery behind this class will cache all configuration data in global variables during first constructor call
 * and will use it for serialization to and deserialization from YSON.
 * Initialization of fields with default values takes place outside of constructor because the machinery
 * behind this class caches dynamic casts and a cache cannot be used for an object under construction
 * since object layout depends on actual class under construction (most derived)
 * and in base class constructor it is impossible to determine which class is constructed.
 * https://en.cppreference.com/w/cpp/language/typeid
 * "If typeid is used on an object under construction or destruction ...
 * then the std::type_info object referred to by this typeid represents the class
 * that is being constructed or destroyed even if it is not the most-derived class."
 * Ref-counted structs are initialized in New<...> (InitializeRefCounted method is called).
 * Non-ref-counted structs are initialized in factory method TYourClass::Create()
 * which is generated by the macro REGISTER_YSON_STRUCT_LITE.
 *
 * In order to speed up compilation it is possible to use DECLARE_YSON_STRUCT(TYourClass) in the class body
 * and supplement it with DEFINE_YSON_STRUCT(TYourClass) in the .cpp file. Similar DECLARE_YSON_STRUCT_LITE
 * macro is available for non-ref-counted structs.
 *
 * The key difference from TYsonSerializable is that the latter builds the whole meta every time
 * an instance of the class is being constructed
 * while TYsonStruct builds meta only once just before construction of the first instance.
 */
class TYsonStructBase
{
public:
    using TPostprocessor = std::function<void()>;
    using TPreprocessor = std::function<void()>;

    virtual ~TYsonStructBase() = default;

    void Load(
        NYTree::INodePtr node,
        bool postprocess = true,
        bool setDefaults = true,
        const NYPath::TYPath& path = {});

    void Load(
        NYson::TYsonPullParserCursor* cursor,
        bool postprocess = true,
        bool setDefaults = true,
        const NYPath::TYPath& path = {});

    void Load(IInputStream* input);

    void Postprocess(const NYPath::TYPath& path = {});

    void SetDefaults();

    void Save(NYson::IYsonConsumer* consumer) const;

    void Save(IOutputStream* output) const;

    IMapNodePtr GetLocalUnrecognized() const;
    IMapNodePtr GetRecursiveUnrecognized() const;

    void SetUnrecognizedStrategy(EUnrecognizedStrategy strategy);

    THashSet<TString> GetRegisteredKeys() const;
    int GetParameterCount() const;

    // TODO(renadeen): remove this methods.
    void SaveParameter(const TString& key, NYson::IYsonConsumer* consumer) const;
    void LoadParameter(const TString& key, const NYTree::INodePtr& node, EMergeStrategy mergeStrategy);
    void ResetParameter(const TString& key);

    std::vector<TString> GetAllParameterAliases(const TString& key) const;

private:
    template <class TValue>
    friend class TYsonStructParameter;

    friend class TYsonStructRegistry;

    friend class TYsonStructMeta;

    friend class TYsonStruct;

    IYsonStructMeta* Meta_ = nullptr;

    // Unrecognized parameters of this struct (not recursive).
    NYTree::IMapNodePtr LocalUnrecognized_;
    std::optional<EUnrecognizedStrategy> InstanceUnrecognizedStrategy_;

    bool CachedDynamicCastAllowed_ = false;
};

////////////////////////////////////////////////////////////////////////////////

class TYsonStruct
    : public TRefCounted
    , public TYsonStructBase
{
public:
    void InitializeRefCounted();
};

////////////////////////////////////////////////////////////////////////////////

class TYsonStructFinalClassHolder
{
protected:
    explicit TYsonStructFinalClassHolder(std::type_index typeIndex);

    // This constructor is only declared but not defined as it never is called.
    // If we delete it default constructor of TYsonStructLite will be implicitly deleted as well and compilation will fail.
    TYsonStructFinalClassHolder();

    std::type_index FinalType_;
};

////////////////////////////////////////////////////////////////////////////////

class TYsonStructLite
    : public virtual TYsonStructFinalClassHolder
    , public TYsonStructBase
{ };

////////////////////////////////////////////////////////////////////////////////

class TYsonStructRegistry
{
public:
    static TYsonStructRegistry* Get();

    static bool InitializationInProgress();

    template <class TStruct>
    void InitializeStruct(TStruct* target);

private:
    static inline YT_THREAD_LOCAL(IYsonStructMeta*) CurrentlyInitializingMeta_ = nullptr;

    template <class TStruct>
    friend class TYsonStructRegistrar;

    template <class TStruct, class TValue>
    friend class TYsonFieldAccessor;

    template <class TStruct, class TValue>
    friend class TUniversalYsonParameterAccessor;

    //! Performs dynamic cast using thread safe cache.
    /*!
     * We need a lot of dynamic casts and they can be expensive for large type hierarchies.
     * This method casts from TYsonStructBase* to TTargetStruct* via thread-safe cache.
     * Cache has two keys — TTargetStruct and typeid(source) — and offset from source to target as value.
     * Due to virtual inheritance, offset between source and target depends on actual type of source.
     * We get actual type using `typeid(...)` but this function has limitation
     * that it doesn't return actual type in constructors and destructors (https://en.cppreference.com/w/cpp/language/typeid).
     * So we cannot use this function in constructors and destructors.
     */
    template <class TTargetStruct>
    TTargetStruct* CachedDynamicCast(const TYsonStructBase* source);

    class TForbidCachedDynamicCastGuard
    {
    public:
        explicit TForbidCachedDynamicCastGuard(TYsonStructBase* target);
        ~TForbidCachedDynamicCastGuard();

    private:
        TYsonStructBase* const Target_;
    };
};

////////////////////////////////////////////////////////////////////////////////

template <class TValue>
class TYsonStructParameter;

////////////////////////////////////////////////////////////////////////////////

template <class TStruct>
class TYsonStructRegistrar
{
public:
    explicit TYsonStructRegistrar(IYsonStructMeta* meta);

    template <class TValue>
    TYsonStructParameter<TValue>& Parameter(const TString& key, TValue(TStruct::*field));

    template <class TBase, class TValue>
    TYsonStructParameter<TValue>& BaseClassParameter(const TString& key, TValue(TBase::*field));

    template <class TValue>
    TYsonStructParameter<TValue>& ParameterWithUniversalAccessor(const TString& key, std::function<TValue&(TStruct*)> accessor);

    void Preprocessor(std::function<void(TStruct*)> preprocessor);

    void Postprocessor(std::function<void(TStruct*)> postprocessor);

    void UnrecognizedStrategy(EUnrecognizedStrategy strategy);

    template<class TBase>
    operator TYsonStructRegistrar<TBase>();

private:
    IYsonStructMeta* const Meta_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
TIntrusivePtr<T> CloneYsonStruct(const TIntrusivePtr<const T>& obj);
template <class T>
TIntrusivePtr<T> CloneYsonStruct(const TIntrusivePtr<T>& obj);
template <class T>
std::vector<TIntrusivePtr<T>> CloneYsonStructs(const std::vector<TIntrusivePtr<T>>& objs);
template <class T>
THashMap<TString, TIntrusivePtr<T>> CloneYsonStructs(const THashMap<TString, TIntrusivePtr<T>>& objs);

void Serialize(const TYsonStructBase& value, NYson::IYsonConsumer* consumer);
void Deserialize(TYsonStructBase& value, INodePtr node);
void Deserialize(TYsonStructBase& value, NYson::TYsonPullParserCursor* cursor);

template <class T>
TIntrusivePtr<T> UpdateYsonStruct(
    const TIntrusivePtr<T>& obj,
    const INodePtr& patch);

template <class T>
TIntrusivePtr<T> UpdateYsonStruct(
    const TIntrusivePtr<T>& obj,
    const NYson::TYsonString& patch);

template <class T>
bool ReconfigureYsonStruct(
    const TIntrusivePtr<T>& config,
    const NYson::TYsonString& newConfigYson);

template <class T>
bool ReconfigureYsonStruct(
    const TIntrusivePtr<T>& config,
    const TIntrusivePtr<T>& newConfig);

template <class T>
bool ReconfigureYsonStruct(
    const TIntrusivePtr<T>& config,
    const INodePtr& newConfigNode);

template <class TSrc, class TDst>
void UpdateYsonStructField(TDst& dst, const std::optional<TSrc>& src);
template <class TSrc, class TDst>
void UpdateYsonStructField(TIntrusivePtr<TDst>& dst, const TIntrusivePtr<TSrc>& src);

////////////////////////////////////////////////////////////////////////////////

//! Declare Yson Struct auxiliary methods and fields. Must be supplemented
//! by DEFINE_YSON_STRUCT.
#define DECLARE_YSON_STRUCT(TStruct)

//! Declare Yson Struct auxiliary methods and define them inplace.
#define REGISTER_YSON_STRUCT(TStruct)

//! Declare non-ref-counted Yson Struct auxiliary methods and fields. Must be
//! supplemented by DEFINE_YSON_STRUCT.
#define DECLARE_YSON_STRUCT_LITE(TStruct)

//! Declare non-ref-counted Yson Struct auxiliary methods and define them inplace.
#define REGISTER_YSON_STRUCT_LITE(TStruct)

//! Define Yson Struct auxiliary methods out of class.
#define DEFINE_YSON_STRUCT(TStruct)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TBinaryYsonStructSerializer
{
    static void Save(TStreamSaveContext& context, const NYTree::TYsonStructBase& obj);
    static void Load(TStreamLoadContext& context, NYTree::TYsonStructBase& obj);
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename std::enable_if_t<std::is_convertible_v<T&, NYTree::TYsonStructBase&>>>
{
    using TSerializer = TBinaryYsonStructSerializer;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define YSON_STRUCT_INL_H_
#include "yson_struct-inl.h"
#undef YSON_STRUCT_INL_H_
