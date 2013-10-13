#pragma once

#include "public.h"
#include "automaton.h"

#include <core/logging/tagged_logger.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TSaveContext
    : public NYT::TStreamSaveContext
{ };

////////////////////////////////////////////////////////////////////////////////

class TLoadContext
    : public NYT::TStreamLoadContext
{
public:
    DEFINE_BYVAL_RW_PROPERTY(int, Version);

public:
    TLoadContext();

};

////////////////////////////////////////////////////////////////////////////////

class TCompositeAutomatonPart
    : public virtual TRefCounted
{
public:
    TCompositeAutomatonPart(
        IHydraManagerPtr hydraManager,
        TCompositeAutomatonPtr automaton);

protected:
    IHydraManagerPtr HydraManager;
    TCompositeAutomaton* Automaton;

    void RegisterSaver(
        int priority,
        const Stroka& name,
        TClosure saver);

    void RegisterLoader(
        const Stroka& name,
        TClosure loader);

    void RegisterSaver(
        int priority,
        const Stroka& name,
        TCallback<void(TSaveContext&)> saver);

    void RegisterLoader(
        const Stroka& name,
        TCallback<void(TLoadContext&)> loader);

    template <class TRequest, class TResponse>
    void RegisterMethod(TCallback<TResponse(const TRequest&)> handler);

    bool IsLeader() const;
    bool IsFollower() const;
    bool IsRecovery() const;

    virtual void Clear();

    virtual void OnBeforeSnapshotLoaded();
    virtual void OnAfterSnapshotLoaded();

    virtual void OnStartLeading();
    virtual void OnLeaderRecoveryComplete();
    virtual void OnLeaderActive();
    virtual void OnStopLeading();

    virtual void OnStartFollowing();
    virtual void OnFollowerRecoveryComplete();
    virtual void OnStopFollowing();

    virtual void OnRecoveryStarted();
    virtual void OnRecoveryComplete();

private:
    typedef TCompositeAutomatonPart TThis;
    friend class TCompositeAutomaton;

    template <class TRequest, class TResponse>
    struct TThunkTraits;

};

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ESerializationPriority,
    (Keys)
    (Values)
);

class TCompositeAutomaton
    : public IAutomaton
{
public:
    void RegisterPart(TCompositeAutomatonPartPtr part);

protected:
    NLog::TTaggedLogger Logger;


    TCompositeAutomaton();

    virtual TSaveContext& SaveContext() = 0;
    virtual TLoadContext& LoadContext() = 0;

    virtual bool ValidateSnapshotVersion(int version);
    virtual int GetCurrentSnapshotVersion();

private:
    friend class TCompositeAutomatonPart;

    struct TSaverInfo
    {
        int Priority;
        Stroka Name;
        TClosure Saver;

        TSaverInfo(int priority, const Stroka& name, TClosure saver);
    };

    struct TLoaderInfo
    {
        Stroka Name;
        TClosure Loader;

        TLoaderInfo(const Stroka& name, TClosure loader);
    };

    yhash_map<Stroka, TCallback<void(TMutationContext* context)>> Methods;

    std::vector<TCompositeAutomatonPartPtr> Parts;

    yhash_map<Stroka, TLoaderInfo> Loaders;
    yhash_map<Stroka, TSaverInfo>  Savers;



    virtual void SaveSnapshot(TOutputStream* output) override;
    virtual void LoadSnapshot(TInputStream* input) override;

    virtual void ApplyMutation(TMutationContext* context) override;

    virtual void Clear() override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

#define COMPOSITE_AUTOMATON_INL_H_
#include "composite_automaton-inl.h"
#undef COMPOSITE_AUTOMATON_INL_H_
