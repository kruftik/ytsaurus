#pragma once

#include "common.h"
#include "leader_lookup.h"
#include "election_manager_proxy.h"

#include <ytlib/meta_state/cell_manager.h>
#include <ytlib/misc/delayed_invoker.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/actions/invoker.h>
#include <ytlib/rpc/client.h>
#include <ytlib/rpc/server.h>
#include <ytlib/misc/configurable.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

struct IElectionCallbacks
    : public virtual TRefCountedBase
{
    typedef TIntrusivePtr<IElectionCallbacks> TPtr;

    virtual void OnStartLeading(const TEpoch& epoch) = 0;
    virtual void OnStopLeading() = 0;
    virtual void OnStartFollowing(TPeerId leaderId, const TEpoch& epoch) = 0;
    virtual void OnStopFollowing() = 0;

    virtual TPeerPriority GetPriority() = 0;
    virtual Stroka FormatPriority(TPeerPriority priority) = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TElectionManager
    : public NRpc::TServiceBase
{
public:
    typedef TIntrusivePtr<TElectionManager> TPtr;

    struct TConfig
        : public TConfigurable
    {
        typedef TIntrusivePtr<TConfig> TPtr;

        TDuration RpcTimeout;
        TDuration FollowerPingInterval;
        TDuration FollowerPingTimeout;
        TDuration ReadyToFollowTimeout;
        TDuration PotentialFollowerTimeout;
        
        TConfig()
        {
            Register("rpc_timeout", RpcTimeout)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(1000));
            Register("follower_ping_interval", FollowerPingInterval)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(1000));
            Register("follower_ping_timeout", FollowerPingTimeout)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(5000));
            Register("ready_to_follow_timeout", ReadyToFollowTimeout)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(5000));
            Register("potential_follower_timeout", PotentialFollowerTimeout)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(5000));
        }
    };

    TElectionManager(
        TConfig* config,
        NMetaState::TCellManager* cellManager,
        IInvoker* controlInvoker,
        IElectionCallbacks* electionCallbacks);

    /*!
     * \note Thread affinity: any.
     */
    void Start();

    /*!
     * \note Thread affinity: any.
     */
    void Stop();

    /*!
     * \note Thread affinity: any.
     */
    void Restart();

    //! Gets info for monitoring in YSON format.
    /*!
     * \note Thread affinity: any.
     */
    void GetMonitoringInfo(NYTree::IYsonConsumer* consumer);
    
private:
    typedef TElectionManager TThis;
    typedef TElectionManagerProxy TProxy;
    typedef TProxy::EErrorCode EErrorCode;

    class TVotingRound;
    class TFollowerPinger;

    TProxy::EState State;
    
    // Voting parameters.
    TPeerId VoteId;
    TEpoch VoteEpoch;

    // Epoch parameters.
    TPeerId LeaderId;
    TGuid Epoch;
    TInstant EpochStart;
    TCancelableInvoker::TPtr ControlEpochInvoker;
    
    typedef yhash_set<TPeerId> TPeerSet;
    TPeerSet AliveFollowers;
    TPeerSet PotentialFollowers;

    TDelayedInvoker::TCookie PingTimeoutCookie;
    TIntrusivePtr<TFollowerPinger> FollowerPinger;

    TConfig::TPtr Config;
    NMetaState::TCellManager::TPtr CellManager;
    IInvoker::TPtr ControlInvoker;
    IElectionCallbacks::TPtr ElectionCallbacks;

    // Corresponds to #ControlInvoker.
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    DECLARE_RPC_SERVICE_METHOD(NElection::NProto, PingFollower);
    DECLARE_RPC_SERVICE_METHOD(NElection::NProto, GetStatus);

    void Reset();
    void OnLeaderPingTimeout();

    void DoStart();
    void DoStop();

    void StartVotingRound();
    void StartVoteFor(TPeerId voteId, const TEpoch& voteEpoch);
    void StartVoteForSelf();

    void StartLeading();
    void StartFollowing(TPeerId leaderId, const TEpoch& epoch);
    void StopLeading();
    void StopFollowing();

    void StartEpoch(TPeerId leaderId, const TEpoch& epoch);
    void StopEpoch();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
