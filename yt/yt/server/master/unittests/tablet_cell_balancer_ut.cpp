#include "helpers.h"

#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/server/master/node_tracker_server/public.h>
#include <yt/yt/server/master/node_tracker_server/node.h>

#include <yt/yt/server/master/cell_server/area.h>
#include <yt/yt/server/master/cell_server/cell_balancer.h>
#include <yt/yt/server/master/cell_server/cell_base.h>
#include <yt/yt/server/master/cell_server/cell_bundle.h>

#include <yt/yt/server/master/tablet_server/tablet_cell.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <util/random/random.h>

namespace NYT::NCellServer {
namespace {

using namespace NYTree;
using namespace NNodeTrackerServer;
using namespace NNodeTrackerClient;
using namespace NYson;
using namespace NHydra;
using namespace NTabletServer;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

using TSettingParam = std::tuple<const char*, const char*, const char*, int, const char*>;
using TStressSettingParam = std::tuple<int, int, int, int, int>;
using TCompleteSettingParam = std::tuple<
    THashMap<TString, int>,
    THashMap<TString, std::vector<int>>,
    THashMap<TString, std::vector<TString>>,
    int,
    THashMap<TString, std::vector<int>>>;

class TSetting
    : public ICellBalancerProvider
{
public:
    TSetting(
        const THashMap<TString, int>& peersPerCell,
        const THashMap<TString, std::vector<int>>& cellLists,
        const THashMap<TString, std::vector<TString>>& nodeFeasibility,
        int tabletSlotCount,
        const THashMap<TString, std::vector<int>>& cellDistribution)
    {
        Initialize(peersPerCell, cellLists, nodeFeasibility, tabletSlotCount, cellDistribution);
    }

    explicit TSetting(const TSettingParam& param)
    {
        auto peersPerCell = ConvertTo<THashMap<TString, int>>(
            TYsonString(TString(std::get<0>(param)), EYsonType::Node));
        auto cellLists = ConvertTo<THashMap<TString, std::vector<int>>>(
            TYsonString(TString(std::get<1>(param)), EYsonType::Node));
        auto nodeFeasibility = ConvertTo<THashMap<TString, std::vector<TString>>>(
            TYsonString(TString(std::get<2>(param)), EYsonType::Node));
        auto tabletSlotCount = std::get<3>(param);
        auto cellDistribution = ConvertTo<THashMap<TString, std::vector<int>>>(
            TYsonString(TString(std::get<4>(param)), EYsonType::Node));

        Initialize(peersPerCell, cellLists, nodeFeasibility, tabletSlotCount, cellDistribution);
    }

    void Initialize(
        const THashMap<TString, int>& peersPerCell,
        const THashMap<TString, std::vector<int>>& cellLists,
        const THashMap<TString, std::vector<TString>>& nodeFeasibility,
        int tabletSlotCount,
        const THashMap<TString, std::vector<int>>& cellDistribution)
    {
        for (const auto& [bundleName, peerCount] : peersPerCell) {
            auto* bundle = GetBundle(bundleName);
            bundle->GetOptions()->PeerCount = peerCount;
        }

        for (const auto& [bundleName, list] : cellLists) {
            auto* bundle = GetBundle(bundleName);
            for (int index : list) {
                CreateCell(bundle, index);
            }
        }

        for (const auto& [nodeName, bundleNames] : nodeFeasibility) {
            auto* node = GetNode(nodeName);
            for (const auto& bundleName : bundleNames) {
                auto* bundle = GetBundle(bundleName, false);
                YT_VERIFY(FeasibilityMap_[node].insert(GetBundleArea(bundle)).second);
            }
        }

        THashSet<const TNode*> seenNodes;
        THashMap<const TCellBase*, int> peers;

        for (const auto& [nodeName, cellIndexes] : cellDistribution) {
            auto* node = GetNode(nodeName);
            YT_VERIFY(seenNodes.insert(node).second);

            TCellSet cellSet;
            for (int index : cellIndexes) {
                auto* cell = GetCell(index);
                int peer = peers[cell]++;
                cell->Peers()[peer].Descriptor = TNodeDescriptor(nodeName);
                cellSet.emplace_back(cell, peer);
            }

            NodeHolders_.emplace_back(node, tabletSlotCount, cellSet);
        }

        for (auto [nodeId, node] : NodeMap_) {
            if (!seenNodes.contains(node)) {
                seenNodes.insert(node);
                NodeHolders_.emplace_back(node, tabletSlotCount, TCellSet{});
            }
        }

        for (auto [cellId, cell] : CellMap_) {
            for (int peer = peers[cell]; peer < cell->CellBundle()->GetOptions()->PeerCount; ++peer) {
                UnassignedPeers_.emplace_back(cell, peer);
            }
        }

        PeersPerCell_ = ConvertToYsonString(peersPerCell, EYsonFormat::Text).ToString();
        CellLists_ = ConvertToYsonString(cellLists, EYsonFormat::Text).ToString();
        InitialDistribution_ = GetDistribution();
    }

    const TCellSet& GetUnassignedPeers()
    {
        return UnassignedPeers_;
    }

    void ApplyMoveDescriptors(const std::vector<TCellMoveDescriptor> descriptors)
    {
        THashMap<const NNodeTrackerServer::TNode*, TNodeHolder*> nodeToHolder;
        for (auto& holder : NodeHolders_) {
            nodeToHolder[holder.GetNode()] = &holder;
        }

        for (const auto& descriptor : descriptors) {
            if (descriptor.Source) {
                RevokePeer(nodeToHolder[descriptor.Source], descriptor.Cell, descriptor.PeerId);
            }
            if (descriptor.Target) {
                AssignPeer(nodeToHolder[descriptor.Target], descriptor.Cell, descriptor.PeerId);
            }
        }
    }

    void ValidateAssignment(const std::vector<TCellMoveDescriptor>& moveDescriptors)
    {
        ApplyMoveDescriptors(moveDescriptors);

        try {
            ValidatePeerAssignment();
            ValidateNodeFeasibility();
            ValidateSmoothness();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION(ex)
                << TErrorAttribute("peers_per_cell", PeersPerCell_)
                << TErrorAttribute("cell_lists", CellLists_)
                << TErrorAttribute("initial_distribution", InitialDistribution_)
                << TErrorAttribute("resulting_distribution", GetDistribution());
        }
    }

    TString GetDistribution()
    {
        return BuildYsonStringFluently(EYsonFormat::Text)
            .DoMapFor(NodeHolders_, [&] (TFluentMap fluent, const TNodeHolder& holder) {
                fluent
                    .Item(NodeToName_[holder.GetNode()])
                    .DoListFor(holder.GetSlots(), [&] (TFluentList fluent, const std::pair<const TCellBase*, int>& slot) {
                        fluent
                            .Item().Value(Format("(%v,%v,%v)",
                                slot.first->CellBundle()->GetName(),
                                CellToIndex_[slot.first],
                                slot.second));
                    });
            })
            .ToString();
    }

    std::vector<TNodeHolder> GetNodes() override
    {
        return NodeHolders_;
    }

    const NHydra::TReadOnlyEntityMap<TCellBundle>& CellBundles() override
    {
        return CellBundleMap_;
    }

    bool IsPossibleHost(const NNodeTrackerServer::TNode* node, const TArea* area) override
    {
        if (auto it = FeasibilityMap_.find(node)) {
            return it->second.contains(area);
        }
        return false;
    }

    bool IsVerboseLoggingEnabled() override
    {
        return true;
    }

    bool IsBalancingRequired() override
    {
        return true;
    }

private:
    TEntityMap<TCellBundle> CellBundleMap_;
    TEntityMap<TCellBase> CellMap_;
    TEntityMap<TArea> AreaMap_;
    TEntityMap<TNode> NodeMap_;
    std::vector<TNodeHolder> NodeHolders_;

    THashMap<const TNode*, THashSet<const TArea*>> FeasibilityMap_;

    THashMap<TString, TCellBundle*> NameToBundle_;
    THashMap<TString, const TNode*> NameToNode_;
    THashMap<const TNode*, TString> NodeToName_;
    THashMap<int, TCellBase*> IndexToCell_;
    THashMap<const TCellBase*, int> CellToIndex_;

    TCellSet UnassignedPeers_;

    TString PeersPerCell_;
    TString CellLists_;
    TString InitialDistribution_;

    TCellBundle* GetBundle(const TString& name, bool create = true)
    {
        if (auto it = NameToBundle_.find(name)) {
            return it->second;
        }

        YT_VERIFY(create);

        auto id = GenerateTabletCellBundleId();
        auto bundleHolder = TPoolAllocator::New<TCellBundle>(id);
        bundleHolder->SetName(name);
        auto* bundle = CellBundleMap_.Insert(id, std::move(bundleHolder));
        YT_VERIFY(NameToBundle_.emplace(name, bundle).second);
        bundle->RefObject();

        auto areaId = ReplaceTypeInId(id, EObjectType::Area);
        auto areaHolder = TPoolAllocator::New<TArea>(areaId);
        areaHolder->SetName(DefaultAreaName);
        areaHolder->SetCellBundle(bundle);
        auto* area = AreaMap_.Insert(areaId, std::move(areaHolder));
        bundle->Areas().emplace(DefaultAreaName, area);
        area->RefObject();

        return bundle;
    }

    void CreateCell(TCellBundle* bundle, int index)
    {
        auto id = GenerateTabletCellId();
        auto cellHolder = TPoolAllocator::New<TTabletCell>(id);
        cellHolder->Peers().resize(bundle->GetOptions()->PeerCount);
        cellHolder->CellBundle().Assign(bundle);

        auto* cell = CellMap_.Insert(id, std::move(cellHolder));
        YT_VERIFY(IndexToCell_.emplace(index, cell).second);
        YT_VERIFY(CellToIndex_.emplace(cell, index).second);
        cell->RefObject();
        YT_VERIFY(bundle->Cells().insert(cell).second);

        auto* area = GetBundleArea(bundle);
        cell->SetArea(area);
        YT_VERIFY(area->Cells().insert(cell).second);
    }

    TCellBase* GetCell(int index)
    {
        return GetOrCrash(IndexToCell_, index);
    }

    const TNode* GetNode(const TString& name, bool create = true)
    {
        if (auto it = NameToNode_.find(name)) {
            return it->second;
        }

        YT_VERIFY(create);

        auto id = GenerateClusterNodeId();
        auto nodeHolder = TPoolAllocator::New<TNode>(id);
        auto* node = NodeMap_.Insert(id, std::move(nodeHolder));
        YT_VERIFY(NameToNode_.emplace(name, node).second);
        YT_VERIFY(NodeToName_.emplace(node, name).second);
        node->RefObject();
        node->SetNodeAddresses(TNodeAddressMap{std::pair(
            EAddressType::InternalRpc,
            TAddressMap{std::pair(DefaultNetworkName, name)})});
        return node;
    }


    TArea* GetBundleArea(TCellBundle* bundle)
    {
        return bundle->Areas().begin()->second;
    }

    void RevokePeer(TNodeHolder* holder, const TCellBase* cell, int peerId)
    {
        auto pair = holder->RemoveCell(cell);
        YT_VERIFY(pair.second == peerId);
    }

    void AssignPeer(TNodeHolder* holder, const TCellBase* cell, int peerId)
    {
        holder->InsertCell(std::pair(cell, peerId));
    }

    void ValidatePeerAssignment()
    {
        for (const auto& holder : NodeHolders_) {
            THashSet<const TCellBase*> cellSet;
            for (const auto& slot : holder.GetSlots()) {
                if (cellSet.contains(slot.first)) {
                    THROW_ERROR_EXCEPTION("Cell %v has two peers assigned to node %v",
                        CellToIndex_[slot.first],
                        NodeToName_[holder.GetNode()]);
                }
                YT_VERIFY(cellSet.insert(slot.first).second);
            }
        }

        {
            THashMap<std::pair<const TCellBase*, int>, const TNode*> cellSet;
            for (const auto& holder : NodeHolders_) {
                for (const auto& slot : holder.GetSlots()) {
                    if (cellSet.contains(slot)) {
                        THROW_ERROR_EXCEPTION("Peer %v of cell %v is assigned to nodes %v and %v",
                            slot.second,
                            CellToIndex_[slot.first],
                            NodeToName_[cellSet[slot]],
                            NodeToName_[holder.GetNode()]);
                    }
                    YT_VERIFY(cellSet.emplace(slot, holder.GetNode()).second);
                }
            }

            for (auto [cellId, cell] : CellMap_) {
                for (int peer = 0; peer < cell->CellBundle()->GetOptions()->PeerCount; ++peer) {
                    if (!cellSet.contains(std::pair(cell, peer))) {
                        THROW_ERROR_EXCEPTION("Peer %v of cell %v is not assigned to any node",
                            peer,
                            CellToIndex_[cell]);
                    }
                }
            }
        }
    }

    void ValidateNodeFeasibility()
    {
        for (const auto& holder : NodeHolders_) {
            THashSet<const TCellBase*> cellSet;
            for (const auto& slot : holder.GetSlots()) {
                if (!IsPossibleHost(holder.GetNode(), GetBundleArea(slot.first->CellBundle().Get()))) {
                    THROW_ERROR_EXCEPTION("Cell %v is assigned to infeasible node %v",
                        CellToIndex_[slot.first],
                        NodeToName_[holder.GetNode()]);
                }
            }
        }
    }

    void ValidateSmoothness()
    {
        for (auto [bundleId, bundle] : CellBundleMap_) {
            THashMap<const TNode*, int> cellsPerNode;
            int feasibleNodes = 0;
            int cells = 0;

            for (const auto& holder : NodeHolders_) {
                auto* node = holder.GetNode();
                if (!IsPossibleHost(node, GetBundleArea(bundle))) {
                    continue;
                }
                ++feasibleNodes;
                for (const auto& slot : holder.GetSlots()) {
                    if (slot.first->CellBundle() == bundle) {
                        ++cells;
                        cellsPerNode[node]++;
                    }
                }
            }

            if (feasibleNodes == 0) {
                continue;
            }

            int lower = cells / feasibleNodes;
            int upper = (cells + feasibleNodes - 1) / feasibleNodes;

            for (auto [node, count] : cellsPerNode) {
                if (count < lower || count> upper) {
                    THROW_ERROR_EXCEPTION("Node %v has %v cells of bundle %v which violates smooth interval [%v, %v]",
                        NodeToName_[node],
                        count,
                        bundle->GetName(),
                        lower,
                        upper);
                }
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TCellBaseBalancerTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TSettingParam>
    , public TBootstrapMock
{
public:
    void SetUp() override
    {
        SetupMasterSmartpointers();
    }

    void TearDown() override
    {
        ResetMasterSmartpointers();
    }
};

class TCellBaseBalancerRevokeTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TSettingParam>
    , public TBootstrapMock
{
public:
    void SetUp() override
    {
        SetupMasterSmartpointers();
    }

    void TearDown() override
    {
        ResetMasterSmartpointers();
    }
};

class TCellBaseBalancerStressTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TStressSettingParam>
    , public TBootstrapMock
{
public:
    void SetUp() override
    {
        SetupMasterSmartpointers();

        std::tie(NodesNum_, TabletSlotCount_, PeersNum_, BundlesNum_, CellsNum_) = GetParam();

        YT_VERIFY(NodesNum_ * TabletSlotCount_ == PeersNum_ * BundlesNum_ * CellsNum_);
        YT_VERIFY(NodesNum_ >= PeersNum_);

        Nodes_.resize(NodesNum_);
        std::vector<TString> bundles(BundlesNum_);
        for (int i = 0; i < NodesNum_; ++i) {
            Nodes_[i] = Format("n%v", i);
        }
        for (int i = 0; i < BundlesNum_; ++i) {
            bundles[i] = Format("b%v", i);
        }

        for (int i = 0; i < NodesNum_; ++i) {
            NodeFeasibility_[Nodes_[i]] = bundles;
        }

        Cells_.resize(BundlesNum_, std::vector<int>(CellsNum_));
        CellsFlattened_.resize(BundlesNum_ * CellsNum_);
        int cellIdx = 0;
        for (auto& cell : Cells_) {
            std::iota(cell.begin(), cell.end(), cellIdx);
            cellIdx += cell.size();
        }
        std::iota(CellsFlattened_.begin(), CellsFlattened_.end(), 0);

        for (const auto& bundle : bundles) {
            PeersPerCell_[bundle] = PeersNum_;
        }
        for (int i = 0; i < BundlesNum_; ++i) {
            CellLists_[bundles[i]] = Cells_[i];
        }
    }

    void TearDown() override
    {
        auto setting = New<TSetting>(PeersPerCell_, CellLists_, NodeFeasibility_, TabletSlotCount_, CellDistribution_);
        auto balancer = CreateCellBalancer(setting);
        for (auto& unassigned : setting->GetUnassignedPeers()) {
            balancer->AssignPeer(unassigned.first, unassigned.second);
        }

        setting->ValidateAssignment(balancer->GetCellMoveDescriptors());

        ResetMasterSmartpointers();
    }

protected:
    int NodesNum_;
    int PeersNum_;
    int BundlesNum_;
    int CellsNum_;

    std::vector<TString> Nodes_;
    std::vector<std::vector<int>> Cells_;
    std::vector<int> CellsFlattened_;

    THashMap<TString, int> PeersPerCell_;
    THashMap<TString, std::vector<int>> CellLists_;
    THashMap<TString, std::vector<TString>> NodeFeasibility_;
    int TabletSlotCount_;
    THashMap<TString, std::vector<int>> CellDistribution_;
};

TEST_P(TCellBaseBalancerStressTest, TestBalancerEmptyDistribution)
{
    CellDistribution_.clear();
    for (int i = 0; i < NodesNum_; ++i) {
        CellDistribution_[Nodes_[i]] = {};
    }
}

// Emplace full bundles (first bundles first) while possible.
TEST_P(TCellBaseBalancerStressTest, TestBalancerGeneratedDistribution1)
{
    int initialBundleIdx = 0;
    int initialNodeIdx = 0;
    const int takenBundles = TabletSlotCount_ / CellsNum_;
    while (initialNodeIdx + PeersNum_ < NodesNum_) {
        for (int nodeIdx = initialNodeIdx; nodeIdx < initialNodeIdx + PeersNum_; ++nodeIdx) {
            auto& distribution = CellDistribution_[Nodes_[nodeIdx]];
            for (int bundleIdx = initialBundleIdx; bundleIdx < initialBundleIdx + takenBundles; ++bundleIdx) {
                for (int cellIdx = 0; cellIdx < CellsNum_; ++cellIdx) {
                    distribution.emplace_back(Cells_[bundleIdx][cellIdx]);
                }
            }
            YT_ASSERT(std::ssize(distribution) <= TabletSlotCount_);
            YT_ASSERT(std::ssize(distribution) == takenBundles * CellsNum_);
        }
        initialNodeIdx += PeersNum_;
        initialBundleIdx += takenBundles;
    }
    // State when we have to do some cell exchanges
    YT_ASSERT(initialBundleIdx - takenBundles < BundlesNum_);
}

// Fill all nodes except last 2 with all cells.
TEST_P(TCellBaseBalancerStressTest, TestBalancerGeneratedDistribution2)
{
    int node = 0;
    int cell = 0;
    int replicaCount = 0;
    std::vector<int> allEmplaces(CellsFlattened_.size(), 0);
    while (node < NodesNum_ - 2 && replicaCount < PeersNum_) {
        for (int slotIdx = 0; slotIdx < TabletSlotCount_; ++slotIdx) {
            CellDistribution_[Nodes_[node]].emplace_back(CellsFlattened_[cell]);
            ++allEmplaces[cell];

            ++cell;
            if (cell == std::ssize(CellsFlattened_)) {
                cell = 0;
                ++replicaCount;
                if (replicaCount == PeersNum_) {
                    break;
                }
            }
        }

        ++node;
    }
}

TEST_P(TCellBaseBalancerStressTest, TestBalancerRandomDistribution)
{
    std::vector<THashSet<int>> filledNodes(NodesNum_);
    auto checkEmplace = [&] (int cell, int nodeIdx) -> bool {
        if (std::ssize(CellDistribution_[Nodes_[nodeIdx]]) == TabletSlotCount_) {
            return false;
        }

        return !filledNodes[nodeIdx].contains(cell);
    };

    SetRandomSeed(TInstant::Now().MilliSeconds());
    bool failed = false;
    for (int peer = 0; peer < PeersNum_ / 2; ++peer) {
        for (int bundleIdx = 0; bundleIdx < BundlesNum_ - 1; ++bundleIdx) {
            for (int cellIdx = 0; cellIdx < CellsNum_; ++cellIdx) {
                int startNodeIdx = RandomNumber<ui32>(NodesNum_);
                YT_ASSERT(startNodeIdx < NodesNum_);
                int nodeIdx = startNodeIdx;
                int cell = Cells_[bundleIdx][cellIdx];
                while (!failed && !checkEmplace(cell, nodeIdx)) {
                    ++nodeIdx;
                    if (nodeIdx == NodesNum_) {
                        nodeIdx = 0;
                    }
                    if (nodeIdx == startNodeIdx) {
                        failed = true;
                    }
                }
                if (failed) {
                    break;
                }

                YT_ASSERT(checkEmplace(cell, nodeIdx));
                CellDistribution_[Nodes_[nodeIdx]].emplace_back(Cells_[bundleIdx][cellIdx]);
                filledNodes[nodeIdx].insert(cell);
            }
        }
    }
}

TEST_P(TCellBaseBalancerRevokeTest, TestBalancer)
{
    auto setting = New<TSetting>(GetParam());
    auto balancer = CreateCellBalancer(setting);

    for (auto& unassigned : setting->GetUnassignedPeers()) {
        balancer->AssignPeer(unassigned.first, unassigned.second);
    }

    setting->ValidateAssignment(balancer->GetCellMoveDescriptors());

    for (auto& assigned : setting->GetUnassignedPeers()) {
        balancer->RevokePeer(assigned.first, assigned.second, TError("reason"));
    }

    setting->ApplyMoveDescriptors(balancer->GetCellMoveDescriptors());

    for (auto& unassigned : setting->GetUnassignedPeers()) {
        balancer->AssignPeer(unassigned.first, unassigned.second);
    }

    setting->ValidateAssignment(balancer->GetCellMoveDescriptors());
}

TEST_P(TCellBaseBalancerTest, TestBalancer)
{
    auto setting = New<TSetting>(GetParam());
    auto balancer = CreateCellBalancer(setting);

    for (auto& unassigned : setting->GetUnassignedPeers()) {
        balancer->AssignPeer(unassigned.first, unassigned.second);
    }

    setting->ValidateAssignment(balancer->GetCellMoveDescriptors());
}

/*
 * Tuple of 5 values:
 * number of nodes,
 * number of slots per node,
 * number of peers per cell,
 * number of bundles,
 * number of cells per bundle
 */
INSTANTIATE_TEST_SUITE_P(
    CellBalancer,
    TCellBaseBalancerStressTest,
    ::testing::Values(
        std::tuple(4, 20, 2, 5, 8),
        std::tuple(6, 30, 4, 9, 5),
        std::tuple(10, 50, 4, 5, 25)));

/*
    Test settings description:
        "{bundle_name: peers_per_cell; ...}",
        "{bundle_name: [cell_index; ...]; ...}",
        "{node_name: [feasible_bundle; ...]; ...}",
        tablet_slots_per_node,
        "{node_name: [cell_index; ...]; ...}"
*/
INSTANTIATE_TEST_SUITE_P(
    CellBalancer,
    TCellBaseBalancerRevokeTest,
    ::testing::Values(
        std::tuple(
            "{a=1;}",
            "{a=[1;2;];}",
            "{n1=[a;]; n2=[a;];}",
            1,
            "{n1=[]; n2=[];}")));

INSTANTIATE_TEST_SUITE_P(
    CellBalancer,
    TCellBaseBalancerTest,
    ::testing::Values(
        std::tuple(
            "{a=1;}",
            "{a=[1;2;3;4]; b=[5;6;7;8]}",
            "{n1=[a;b]; n2=[a;b]; n3=[a;b]}",
            10,
            "{n1=[1;2]; n2=[3;4]; n3=[5;6]}"),
        std::tuple(
            "{a=2;}",
            "{a=[1;2;3;4]; b=[5;6;7;8]}",
            "{n1=[a;b]; n2=[a;b]; n3=[a;b]}",
            10,
            "{n1=[1;2]; n2=[3;4]; n3=[5;6]}"),
        std::tuple(
            "{a=2;}",
            "{a=[1;2;3]}",
            "{n1=[a]; n2=[a]; n3=[a]}",
            2,
            "{n1=[]; n2=[]; n3=[]}"),
        std::tuple(
            "{a=2;}",
            "{a=[1;2;3;4;5;6;7;8;9;10]}",
            "{n1=[a]; n2=[a]; n3=[a]}",
            10,
            "{n1=[1;2;3;4;5;6;7;8;9;10]; n2=[1;2;3;4]; n3=[5;6;7;8;9;10]}"),
        std::tuple(
            "{a=2; b=2; c=2}",
            "{a=[1;2;3;]; b=[4;5;6;]; c=[7;8;9;]}",
            "{n1=[a;b;c]; n2=[a;b;c]; n3=[a;b;c]}",
            6,
            "{n1=[]; n2=[]; n3=[]}"),
        std::tuple(
            "{a=2; b=2; c=2}",
            "{a=[1;2;3;]; b=[4;5;6;]; c=[7;8;9;]}",
            "{n1=[a;b;c]; n2=[a;b;c]; n3=[a;b;c]}",
            6,
            "{n1=[1;2;3;4;5;6;]; n2=[]; n3=[1;2;3;4;5;6;]}"),
        std::tuple(
            "{a=2; b=2; c=2}",
            "{a=[1;2;3;]; b=[4;5;6;]; c=[7;8;9;]}",
            "{n1=[a;b;c]; n2=[a;b;c]; n3=[a;b;c]}",
            6,
            "{n1=[1;2;3;4;5;6;]; n2=[1;2;7;8;9;]; n3=[3;4;5;6;8;9]}")));


////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NCellServer

