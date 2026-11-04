#include "eve-server.h"

#include "bots/BotBrain.h"
#include "EntityList.h"
#include "Client.h"
#include "agents/Agent.h"
#include "inventory/Inventory.h"
#include "inventory/ItemFactory.h"
#include "inventory/ItemType.h"
#include "missions/MissionDataMgr.h"
#include "ship/Ship.h"
#include "ship/modules/ModuleManager.h"
#include "npc/NPC.h"
#include "manufacturing/Blueprint.h"
#include "manufacturing/FactoryDB.h"
#include "manufacturing/RamProxyService.h"
#include "packets/Manufacturing.h"
#include "planet/Colony.h"
#include "planet/Planet.h"
#include "planet/PlanetMgr.h"
#include "planet/PlanetDataMgr.h"
#include "market/MarketProxyService.h"
#include "system/Asteroid.h"
#include "system/Container.h"
#include "system/DestinyManager.h"
#include "system/SystemManager.h"
#include "system/TargetManager.h"
#include "system/cosmicMgrs/BeltMgr.h"
#include "market/MarketDB.h"
#include "market/MarketMgr.h"
#include "station/Station.h"
#include "station/ReprocessingService.h"
#include "station/StationDataMgr.h"
#include "../../eve-common/EVE_Standings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace PlayerBot {

namespace {

struct GateRoute {
    uint32 fromSystem = 0;
    uint32 toSystem = 0;
    uint32 fromGate = 0;
    uint32 toGate = 0;
    double destinationSecurity = 0.0;
};

struct GateRouteGraph {
    bool loaded = false;
    bool available = false;
    std::map<uint32, std::vector<GateRoute> > edges;
    std::map<std::pair<uint32, uint32>, GateRoute> firstHopCache;
    std::map<std::pair<uint32, uint32>, GateRoute> highsecFirstHopCache;
    std::map<uint32, std::vector<uint32> > highsecBeltsBySystem;
    std::map<uint32, uint32> highsecSystemByBelt;
};

struct MissionAgentLocation {
    uint32 agentID = 0;
    uint32 stationID = 0;
};

bool GetMissionAgentLocations(uint32 homeStationID, std::vector<MissionAgentLocation>& locations)
{
    static std::map<uint32, std::vector<MissionAgentLocation> > byRegion;
    const uint32 regionID = sDataMgr.GetStationRegion(homeStationID);
    if (regionID == 0)
        return false;

    const auto cached = byRegion.find(regionID);
    if (cached != byRegion.end()) {
        locations = cached->second;
        return !locations.empty();
    }

    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT a.agentID, a.locationID FROM agtAgents a "
        "JOIN staStations st ON st.stationID = a.locationID "
        "JOIN mapSolarSystems sys ON sys.solarSystemID = st.solarSystemID "
        "WHERE a.agentTypeID = 2 AND a.level = 1 AND a.divisionID IN (7,22) "
        "AND sys.regionID = %u AND sys.security >= 0.5 ORDER BY a.agentID", regionID)) {
        sLog.Warning("PlayerBot", "Could not load level-1 distribution agents in region %u: %s.",
                     regionID, result.error.c_str());
        return false;
    }

    DBResultRow row;
    while (result.GetRow(row)) {
        MissionAgentLocation candidate;
        candidate.agentID = row.GetUInt(0);
        candidate.stationID = row.GetUInt(1);
        if (candidate.agentID != 0 && candidate.stationID != 0)
            locations.push_back(candidate);
    }
    byRegion[regionID] = locations;
    if (locations.empty())
        sLog.Warning("PlayerBot", "No level-1 distribution agents are available in region %u.", regionID);
    else
        sLog.Green("PlayerBot", "Loaded %zu highsec level-1 distribution agents in region %u.",
                   locations.size(), regionID);
    return !locations.empty();
}

bool IsSafeMissionStation(uint32 stationID, uint32 expectedRegionID)
{
    StationData station;
    SystemData system;
    return stationID != 0 && stDataMgr.GetStationData(stationID, station) &&
           sDataMgr.GetSystemData(station.systemID, system) &&
           system.regionID == expectedRegionID && system.securityRating >= 0.5f;
}

bool IsTraderMarketStock(const InventoryItemRef& item)
{
    if (item.get() == nullptr)
        return false;
    const ItemType* type = sItemFactory.GetType(item->typeID());
    return type != nullptr && type->published() && !item->isSingleton();
}

struct IndustryProduct {
    uint16 blueprint;
    uint16 product;
};

IndustryProduct IndustryProductForSlot(uint16 slot)
{
    // Crucible T1 mining hulls and a module used by the mining cohorts.
    static const std::array<IndustryProduct, 5> products = {{
        {785, 483}, {683, 582}, {938, 591}, {939, 592}, {946, 599}
    }};
    const uint32 cohort = slot ? (slot - 1) / 6 : 0;
    return products[cohort % products.size()];
}

bool CanFillMinimumVolume(uint32 orderID, uint32 quantity, uint32 remaining)
{
    DBQueryResult result;
    if (!sDatabase.RunQuery(result, "SELECT minVolume FROM mktOrders WHERE orderID=%u AND bid=1", orderID))
        return false;
    DBResultRow row;
    return result.GetRow(row) && quantity >= std::min(row.GetUInt(0), remaining);
}

GateRouteGraph& GetGateRouteGraph()
{
    static GateRouteGraph graph;
    if (graph.loaded)
        return graph;
    graph.loaded = true;

    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT src.solarSystemID, dst.solarSystemID, j.stargateID, j.celestialID, destination.security "
        "FROM mapJumps j "
        "JOIN mapDenormalize src ON src.itemID=j.stargateID "
        "JOIN mapDenormalize dst ON dst.itemID=j.celestialID "
        "JOIN mapSolarSystems destination ON destination.solarSystemID=dst.solarSystemID "
        "WHERE src.solarSystemID BETWEEN 30000000 AND 39999999 "
        "AND dst.solarSystemID BETWEEN 30000000 AND 39999999")) {
        sLog.Warning("PlayerBot", "Could not load the stargate graph for faction-hub travel: %s.",
                     result.error.c_str());
        return graph;
    }

    DBResultRow row;
    while (result.GetRow(row)) {
        GateRoute edge;
        edge.fromSystem = row.GetUInt(0);
        edge.toSystem = row.GetUInt(1);
        edge.fromGate = row.GetUInt(2);
        edge.toGate = row.GetUInt(3);
        edge.destinationSecurity = row.GetDouble(4);
        if (edge.fromSystem != 0 && edge.toSystem != 0 && edge.fromGate != 0 && edge.toGate != 0)
            graph.edges[edge.fromSystem].push_back(edge);
    }

    DBQueryResult beltResult;
    if (sDatabase.RunQuery(beltResult,
        "SELECT d.itemID, d.solarSystemID FROM mapDenormalize d "
        "JOIN mapSolarSystems sys ON sys.solarSystemID=d.solarSystemID "
        "WHERE d.groupID=%u AND d.solarSystemID BETWEEN 30000000 AND 39999999 "
        "AND sys.security >= 0.5 ORDER BY d.solarSystemID, d.itemID",
        EVEDB::invGroups::Asteroid_Belt)) {
        DBResultRow beltRow;
        while (beltResult.GetRow(beltRow)) {
            const uint32 beltID = beltRow.GetUInt(0);
            const uint32 systemID = beltRow.GetUInt(1);
            if (beltID == 0 || systemID == 0)
                continue;
            graph.highsecBeltsBySystem[systemID].push_back(beltID);
            graph.highsecSystemByBelt[beltID] = systemID;
        }
        sLog.Green("PlayerBot", "Loaded highsec asteroid belts in %zu solar systems.",
                   graph.highsecBeltsBySystem.size());
    } else {
        sLog.Warning("PlayerBot", "Could not load highsec asteroid belt locations: %s.",
                     beltResult.error.c_str());
    }

    graph.available = !graph.edges.empty();
    if (graph.available)
        sLog.Green("PlayerBot", "Loaded %zu solar-system stargate route lists.", graph.edges.size());
    return graph;
}

bool GetNextGateRoute(uint32 fromSystem, uint32 toSystem, GateRoute& firstHop, bool highsecOnly = false)
{
    if (fromSystem == toSystem)
        return false;
    GateRouteGraph& graph = GetGateRouteGraph();
    if (!graph.available)
        return false;

    const std::pair<uint32, uint32> key(fromSystem, toSystem);
    std::map<std::pair<uint32, uint32>, GateRoute>& routeCache = highsecOnly ?
        graph.highsecFirstHopCache : graph.firstHopCache;
    const auto cached = routeCache.find(key);
    if (cached != routeCache.end()) {
        firstHop = cached->second;
        return true;
    }

    typedef std::pair<double, uint32> QueueEntry;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry> > frontier;
    std::map<uint32, double> costs;
    std::map<uint32, uint32> previousSystem;
    std::map<uint32, GateRoute> previousEdge;
    costs[fromSystem] = 0.0;
    frontier.push(QueueEntry(0.0, fromSystem));

    while (!frontier.empty()) {
        const QueueEntry next = frontier.top();
        frontier.pop();
        const auto knownCost = costs.find(next.second);
        if (knownCost == costs.end() || next.first > knownCost->second)
            continue;
        if (next.second == toSystem)
            break;

        const auto adjacent = graph.edges.find(next.second);
        if (adjacent == graph.edges.end())
            continue;
        for (const GateRoute& edge : adjacent->second) {
            if (highsecOnly && edge.destinationSecurity < 0.5)
                continue;
            // Prefer high security routes. Low/null systems remain routable
            // when they are the only connection, with a much larger cost.
            const double riskCost = edge.destinationSecurity >= 0.5 ? 1.0 :
                                    (edge.destinationSecurity > 0.0 ? 5.0 : 30.0);
            const double candidateCost = next.first + riskCost;
            const auto oldCost = costs.find(edge.toSystem);
            if (oldCost != costs.end() && oldCost->second <= candidateCost)
                continue;
            costs[edge.toSystem] = candidateCost;
            previousSystem[edge.toSystem] = edge.fromSystem;
            previousEdge[edge.toSystem] = edge;
            frontier.push(QueueEntry(candidateCost, edge.toSystem));
        }
    }

    if (costs.find(toSystem) == costs.end())
        return false;

    uint32 step = toSystem;
    while (previousSystem.find(step) != previousSystem.end() && previousSystem[step] != fromSystem)
        step = previousSystem[step];
    const auto edge = previousEdge.find(step);
    if (edge == previousEdge.end() || edge->second.fromSystem != fromSystem)
        return false;
    firstHop = edge->second;
    routeCache.emplace(key, firstHop);
    return true;
}

uint32 FindPlanetarySaleStation(uint32 systemID)
{
    SystemData source;
    if (!sDataMgr.GetSystemData(systemID, source))
        return 0;
    DBQueryResult stations;
    if (!sDatabase.RunQuery(stations,
        "SELECT st.stationID, st.solarSystemID FROM staStations st "
        "JOIN mapSolarSystems sys ON sys.solarSystemID=st.solarSystemID "
        "WHERE sys.regionID=%u AND sys.security>=0.5 AND sys.solarSystemID<>30000142 "
        "ORDER BY (st.solarSystemID=%u) DESC, st.stationID LIMIT 64", source.regionID, systemID))
        return 0;
    DBResultRow row;
    while (stations.GetRow(row)) {
        const uint32 candidateSystem = row.GetUInt(1);
        GateRoute route;
        if (candidateSystem == systemID || GetNextGateRoute(systemID, candidateSystem, route, true))
            return row.GetUInt(0);
    }
    return 0;
}

bool FindNearestHighsecBelt(uint32 fromSystem, uint16 botSlot, uint32& targetSystem, uint32& targetBelt)
{
    GateRouteGraph& graph = GetGateRouteGraph();
    if (!graph.available || graph.highsecBeltsBySystem.empty())
        return false;

    std::map<uint32, uint32> hops;
    std::queue<uint32> frontier;
    hops[fromSystem] = 0;
    frontier.push(fromSystem);
    while (!frontier.empty()) {
        const uint32 systemID = frontier.front();
        frontier.pop();
        const auto adjacent = graph.edges.find(systemID);
        if (adjacent == graph.edges.end())
            continue;
        for (const GateRoute& edge : adjacent->second) {
            if (edge.destinationSecurity < 0.5 || hops.find(edge.toSystem) != hops.end())
                continue;
            hops[edge.toSystem] = hops[systemID] + 1;
            frontier.push(edge.toSystem);
        }
    }

    uint32 nearestHops = std::numeric_limits<uint32>::max();
    std::vector<uint32> candidates;
    for (const auto& entry : graph.highsecBeltsBySystem) {
        const auto route = hops.find(entry.first);
        if (route == hops.end() || route->second == 0)
            continue;
        if (route->second < nearestHops) {
            nearestHops = route->second;
            candidates.clear();
        }
        if (route->second == nearestHops)
            candidates.push_back(entry.first);
    }
    if (candidates.empty())
        return false;

    const uint32 selectedSystem = candidates[(static_cast<uint32>(botSlot) * 17U) % candidates.size()];
    const std::vector<uint32>& belts = graph.highsecBeltsBySystem[selectedSystem];
    if (belts.empty())
        return false;
    targetSystem = selectedSystem;
    targetBelt = belts[(static_cast<uint32>(botSlot) * 31U) % belts.size()];
    return targetBelt != 0;
}

uint32 HubStationFor(const Client& client, const Record& record)
{
    // New pilots start at the seed hub; established industrial pilots keep
    // their persisted operating base across logins and server restarts.
    (void)client;
    if (record.homeStationID != 0 && record.homeStationID != 60003760)
        return record.homeStationID;
    return sConfig.playerbots.spawnStationID != 0 ? sConfig.playerbots.spawnStationID : 60003760;
}

} // anonymous namespace

BotBrain::BotBrain(Client& client, Record& record)
: m_client(client), m_record(record), m_state(State::Docked), m_stateSince(GetFileTimeNow()),
  m_autoStartAt(GetFileTimeNow() + ((static_cast<uint64_t>(record.characterID) * 37ULL) % 60ULL) * 10000000ULL),
  m_targetID(0), m_requestedAsteroidTypeID(0), m_manualMining(false), m_returnWarpStarted(false),
  m_homeTransitComplete(false), m_missionAgentID(0), m_missionAgentStationID(0),
  m_transitStationID(0), m_missionRetryAt(0), m_lastTransitDiagAt(0),
  m_lastMarketActionAt(0), m_lastJitaOrderCheckAt(0), m_lastPlanetaryCheckAt(0),
  m_lastPlanetarySpaceCheckAt(0), m_planetaryPickupID(0), m_planetarySystemID(0),
  m_planetarySaleStationID(0), m_planetaryCargoTypeID(0), m_planetaryTaskStoreAvailable(false), m_demandOrderID(0),
  m_demandSourceStationID(0)
{
    if (record.state == "MINING") m_state = State::Mining;
    else if (record.state == "WARP_TO_BELT") m_state = State::WarpToBelt;
    else if (record.state == "RETURNING") m_state = State::Returning;
    else if (record.state == "SELLING") m_state = State::Selling;
    else if (record.state == "ATTACKING") m_state = State::Attacking;
    else if (record.state == "RESPAWNING") m_state = State::Respawning;

    // A manual movement/order is not persisted as a resumable command. If the
    // bot restarted docked, resume its profession loop instead of restoring a
    // MANUAL state that has no target and can never make progress.
    if (m_client.IsDocked() && m_state != State::Respawning) {
        if (m_state == State::Returning)
            m_state = State::Selling;
        else if (m_state != State::Selling)
            m_state = State::Docked;
    } else if (m_state == State::Manual) {
        m_state = State::Docked;
    }
    m_record.state = GetStateName();

    const uint32 destinationStationID = HubStationFor(m_client, m_record);
    StationData destination;
    m_homeTransitComplete = stDataMgr.GetStationData(destinationStationID, destination) &&
        m_client.GetSystemID() == destination.systemID && m_client.IsDocked() &&
        static_cast<uint32>(m_client.GetStationID()) == destinationStationID;
    if (record.profession == "HAULER" && sConfig.cosmic.PIEnabled) {
        DBQueryResult task;
        m_planetaryTaskStoreAvailable = sDatabase.RunQuery(task,
            "SELECT pickupID, solarSystemID, saleStationID, cargoTypeID "
            "FROM botPlanetaryTasks WHERE characterID=%u", record.characterID);
        DBResultRow row;
        if (m_planetaryTaskStoreAvailable && task.GetRow(row)) {
            m_planetaryPickupID = row.GetUInt(0);
            m_planetarySystemID = row.GetUInt(1);
            m_planetarySaleStationID = row.GetUInt(2);
            m_planetaryCargoTypeID = row.GetUInt(3);
            if (m_planetarySaleStationID != 0 || m_planetaryPickupID != 0)
                m_homeTransitComplete = true;
        }
        if (!m_planetaryTaskStoreAvailable)
            sLog.Warning("PlayerBot", "Planetary task storage is unavailable; apply the PlayerBot PI migration.");
    }
    if (!m_homeTransitComplete && record.profession == "MISSION_RUNNER") {
        std::vector<MissionOffer> offers;
        sMissionDataMgr.LoadMissionOffers(m_client.GetCharacterID(), offers);
        for (const MissionOffer& offer : offers) {
            if (offer.typeID == Mission::Type::Courier &&
                (offer.stateID == Mission::State::Accepted || offer.stateID == Mission::State::Offered ||
                 offer.stateID == Mission::State::Allocated)) {
                m_homeTransitComplete = true;
                break;
            }
        }
    }
}

void BotBrain::SetState(State state)
{
    m_state = state;
    m_stateSince = GetFileTimeNow();
    m_record.state = GetStateName();
}

const char* BotBrain::GetStateName() const
{
    switch (m_state) {
        case State::Docked: return "DOCKED";
        case State::Undocking: return "UNDOCKING";
        case State::WarpToBelt: return "WARP_TO_BELT";
        case State::Mining: return "MINING";
        case State::Returning: return "RETURNING";
        case State::Selling: return "SELLING";
        case State::Attacking: return "ATTACKING";
        case State::Traveling: return "TRAVELING";
        case State::Manual: return "MANUAL";
        case State::Respawning: return "RESPAWNING";
    }
    return "DOCKED";
}

bool BotBrain::ProcessHomeTransit()
{
    return ProcessStationTransit(HubStationFor(m_client, m_record));
}

bool BotBrain::ProcessStationTransit(uint32 destinationStationID)
{
    m_transitStationID = destinationStationID;
    const uint64_t now = GetFileTimeNow();
    const bool homeTarget = destinationStationID == HubStationFor(m_client, m_record);
    StationData destination;
    if (!stDataMgr.GetStationData(destinationStationID, destination)) {
        sLog.Warning("PlayerBot", "%s cannot resolve destination station %u for transit.",
                     m_client.GetName(), destinationStationID);
        return false;
    }
    if (homeTarget)
        m_record.homeStationID = destinationStationID;

    // Each roster founder gets a rate-limited trace while automatic transit is
    // blocked. This exposes socketless login, warp, bubble and station issues
    // without flooding logs for all 150 pilots.
    const bool monitorTransit = m_record.slot != 0 && ((m_record.slot - 1) % 15) == 0;
    const auto reportTransitWait = [this, now, monitorTransit](const char* reason, bool destinyWarping) {
        if (!monitorTransit ||
            (m_lastTransitDiagAt != 0 && now - m_lastTransitDiagAt < 600000000ULL))
            return;
        m_lastTransitDiagAt = now;
        sLog.Warning("PlayerBot",
            "Transit wait %s (%u), slot %u: %s; system=%u station=%u loginWarp=%u jump=%u dock=%u undock=%u session=%u bubble=%u idle=%u invul=%u destinyWarp=%u sessionMs=%u.",
            m_client.GetName(), m_record.characterID, m_record.slot, reason,
            m_client.GetSystemID(), m_client.GetStationID(), m_client.IsLoginWarping(),
            m_client.IsJump(), m_client.IsDock(), m_client.IsUndock(), m_client.IsSessionChange(),
            m_client.IsBubbleWait(), m_client.IsIdle(), m_client.IsInvul(), destinyWarping,
            m_client.GetSessionChangeTime());
    };

    // Let the normal client state machine finish session changes, gate jumps,
    // docking animations and undocking before issuing the next command.
    if (m_client.IsLoginWarping() || m_client.IsJump() || m_client.IsDock() || m_client.IsUndock() ||
        m_client.IsSessionChange() || m_client.IsBubbleWait()) {
        reportTransitWait("client-transition", false);
        return true;
    }

    if (m_client.IsDocked()) {
        // Fit the profession hull at home before the initial transit completes.
        if (homeTarget && EnsureProfessionShip())
            return true;

        if (static_cast<uint32>(m_client.GetStationID()) == destinationStationID &&
            m_client.GetSystemID() == destination.systemID) {
            if (homeTarget)
                m_homeTransitComplete = true;
            m_transitStationID = 0;
            SetState(State::Docked);
            return true;
        }

        // Virtual clients still use the regular login/session/invulnerability
        // timers. Do not issue another undock while those are active.
        if (!m_client.IsIdle() || m_client.IsInvul()) {
            reportTransitWait("station-not-idle", false);
            return true;
        }

        m_client.SetDockStationID(destinationStationID);
        m_client.UndockFromStation();
        SetState(State::Traveling);
        return true;
    }

    if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr || m_client.SystemMgr() == nullptr) {
        reportTransitWait("client-has-no-space-ship", false);
        return false;
    }

    DestinyManager* destiny = m_client.GetShipSE()->DestinyMgr();
    if (destiny == nullptr || destiny->IsWarpingOrAligning()) {
        reportTransitWait(destiny == nullptr ? "no-destiny-manager" : "destiny-warping",
                          destiny != nullptr && destiny->IsWarpingOrAligning());
        return true;
    }

    if (m_client.GetSystemID() == destination.systemID) {
        SystemEntity* station = m_client.SystemMgr()->FindEntityByID(destinationStationID);
        if (station == nullptr) {
            reportTransitWait("home-station-not-in-system", false);
            return true;
        }

        m_client.SetDockStationID(destinationStationID);
        if (m_client.GetShipSE()->GetPosition().distance(station->GetPosition()) > 2500.0) {
            destiny->WarpTo(station->GetPosition(), 0);
            SetState(State::Traveling);
            return true;
        }

        m_client.DockToStation();
        if (m_client.IsDocked()) {
            if (homeTarget)
                m_homeTransitComplete = true;
            m_transitStationID = 0;
            SetState(State::Docked);
        } else {
            reportTransitWait("dock-command-did-not-dock", false);
        }
        return true;
    }

    GateRoute route;
    if (!GetNextGateRoute(m_client.GetSystemID(), destination.systemID, route)) {
        // Keep this deterministic and retryable if the static map cache was not
        // ready when the bot first logged in.
        reportTransitWait("no-stargate-route", false);
        return true;
    }

    SystemEntity* gate = m_client.SystemMgr()->FindEntityByID(route.fromGate);
    if (gate == nullptr || gate->GetGateSE() == nullptr) {
        reportTransitWait("next-stargate-not-in-system", false);
        return true;
    }

    if (m_client.GetShipSE()->GetPosition().distance(gate->GetPosition()) > 2500.0) {
        destiny->WarpTo(gate->GetPosition(), 0);
        SetState(State::Traveling);
        return true;
    }

    if (m_client.IsIdle())
        m_client.StargateJump(route.fromGate, route.toGate);
    return true;
}

void BotBrain::Process()
{
    if (m_client.GetShip().get() == nullptr)
        return;

    const uint64_t now = GetFileTimeNow();
    if (m_state == State::Respawning) {
        // Do not move the deadline forward on each tick. A destroyed bot must
        // be allowed to finish the normal clone/rookie-ship path before its
        // profession brain resumes.
        if (m_record.respawnAt == 0)
            m_record.respawnAt = now + static_cast<uint64_t>(sConfig.playerbots.respawnDelay) * 10000000ULL;
        if (now < m_record.respawnAt || m_client.InPod() || !m_client.IsDocked())
            return;

        m_record.respawnAt = 0;
        SetState(State::Docked);
    } else if (m_client.InPod()) {
        StopMining();
        m_record.respawnAt = now + static_cast<uint64_t>(sConfig.playerbots.respawnDelay) * 10000000ULL;
        SetState(State::Respawning);
        return;
    }
    if (m_state == State::Manual)
        return;
    if (!m_homeTransitComplete && m_state != State::Attacking) {
        ProcessHomeTransit();
        return;
    }
    // A character can be between systems or in a Destiny warp while its
    // periodic brain tick runs. Module activation, cargo moves and station
    // actions during those phases raise game UserErrors (for example
    // DeniedActivateInJump); let the regular Client/Destiny lifecycle finish
    // before any profession or manual order executes.
    if (m_client.IsLoginWarping() || m_client.IsJump() || m_client.IsDock() ||
        m_client.IsUndock() || m_client.IsSessionChange() || m_client.IsBubbleWait())
        return;
    ShipSE* activeShip = m_client.GetShipSE();
    if (activeShip != nullptr && activeShip->DestinyMgr() != nullptr &&
        activeShip->DestinyMgr()->IsWarpingOrAligning())
        return;
    if (m_lastJitaOrderCheckAt == 0 || now - m_lastJitaOrderCheckAt >= 60ULL * 10000000ULL) {
        m_lastJitaOrderCheckAt = now;
        DBQueryResult orders;
        if (sDatabase.RunQuery(orders,
            "SELECT orderID FROM mktOrders WHERE ownerID=%u AND solarSystemID=30000142 "
            "AND bid=0 AND isCorp=0 ORDER BY orderID LIMIT 1", m_client.GetCharacterID())) {
            DBResultRow row;
            if (orders.GetRow(row)) {
                MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
                if (market != nullptr && market->CancelBotSellOrder(m_client, row.GetUInt(0)))
                    return;
            }
        }
    }
    if (!m_manualMining && m_state != State::Attacking && m_state != State::Returning && now < m_autoStartAt)
        return;
    if (!m_manualMining && m_state != State::Attacking && m_planetaryPickupID != 0 && ProcessPlanetaryPickup())
        return;
    if (!m_manualMining && m_state != State::Attacking && m_planetarySaleStationID != 0 && ProcessPlanetarySale())
        return;
    if (PreparePlanetary())
        return;
    if (RelocateIndustrialBase())
        return;
    if (DeployPlanetary())
        return;
    if (m_state == State::Attacking && m_record.profession != "RAT_HUNTER") {
        ProcessAttack();
        return;
    }
    if (m_manualMining) {
        ProcessMiner();
        return;
    }
    if (m_record.profession == "RAT_HUNTER") {
        ProcessRatHunter();
        return;
    }
    if (m_record.profession == "TRADER") {
        ProcessTrader();
        return;
    }
    if (m_record.profession == "MISSION_RUNNER") {
        ProcessMissionRunner();
        return;
    }
    if (m_record.profession == "INDUSTRIALIST") {
        ProcessIndustrialist();
        return;
    }

    // HAULER uses live mining until its contract-route planner is enabled.
    ProcessMiner();
}

bool BotBrain::RelocateIndustrialBase()
{
    if (m_manualMining || m_state == State::Attacking || !m_client.IsDocked() ||
        m_client.GetSystemID() != 30000142 || m_client.GetStationID() != 60003760 ||
        m_record.homeStationID != 60003760 || m_record.slot == 0 ||
        m_client.SystemMgr() == nullptr || m_client.GetShip().get() == nullptr ||
        (m_record.profession != "MINER" && m_record.profession != "HAULER" &&
         m_record.profession != "TRADER" && m_record.profession != "INDUSTRIALIST"))
        return false;
    if (EnsureProfessionShip())
        return true;
    if ((m_record.profession == "MINER" || m_record.profession == "HAULER") && !IsMiningShipReady())
        return false;

    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(m_client.GetStationID());
    ShipItemRef ship = m_client.GetShip();
    if (station.get() == nullptr || ship->GetMyInventory() == nullptr)
        return false;
    if (m_record.profession == "HAULER" && sConfig.cosmic.PIEnabled && m_planetaryTaskStoreAvailable) {
        bool haveCenter = false;
        std::vector<InventoryItemRef> cargo;
        ship->GetMyInventory()->GetItemsByFlag(flagCargoHold, cargo);
        for (const InventoryItemRef& item : cargo)
            if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() && item->typeID() == 2524)
                haveCenter = true;
        if (!haveCenter) {
            DBQueryResult colonies;
            DBResultRow row;
            if (!sDatabase.RunQuery(colonies, "SELECT 1 FROM piPlanets WHERE charID=%u LIMIT 1",
                                    m_client.GetCharacterID()) || !colonies.GetRow(row))
                return false;
        }
    }
    if (m_record.profession == "INDUSTRIALIST") {
        // A blueprint in an unfinished station job must stay with that job
        // until it is delivered through the normal manufacturing service.
        if (ProcessActiveIndustryJob())
            return true;
        uint32 blueprintID = 0;
        const IndustryProduct product = IndustryProductForSlot(m_record.slot);
        std::vector<InventoryItemRef> cargo;
        ship->GetMyInventory()->GetItemsByFlag(flagCargoHold, cargo);
        for (const InventoryItemRef& item : cargo)
            if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() && item->typeID() == product.blueprint)
                blueprintID = item->itemID();
        if (!blueprintID) {
            if (!EnsureIndustryBlueprint(m_client.GetStationID(), product.blueprint, blueprintID))
                return false;
            InventoryItemRef blueprint = sItemFactory.GetItemRef(blueprintID);
            if (blueprint.get() == nullptr || !ship->GetMyInventory()->HasAvailableSpace(flagCargoHold, blueprint))
                return false;
            m_client.MoveItem(blueprintID, ship->itemID(), flagCargoHold);
            if (blueprint->locationID() != ship->itemID() || blueprint->flag() != flagCargoHold)
                return false;
        }
    }
    // Carry existing owned stock when it fits. Items that do not fit stay in
    // their original hangar rather than being replaced or destroyed.
    std::vector<InventoryItemRef> items;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, items);
    for (const InventoryItemRef& item : items) {
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->locationID() != m_client.GetStationID() || !IsTraderMarketStock(item) ||
            !ship->GetMyInventory()->HasAvailableSpace(flagCargoHold, item))
            continue;
        m_client.MoveItem(item->itemID(), ship->itemID(), flagCargoHold);
    }
    // Each six-pilot profession cohort shares a real, one-jump highsec
    // station with manufacturing lines, refinery and nearby asteroid belts.
    static const std::array<uint32, 7> bases = {{
        60006427, 60003763, 60002950, 60002956, 60014935, 60002419, 60001306
    }};
    uint32 base = bases[((m_record.slot - 1) / 6) % bases.size()];
    if (m_record.profession == "HAULER") {
        DBQueryResult colonies;
        DBResultRow row;
        if (!sDatabase.RunQuery(colonies,
            "SELECT solarSystemID FROM piPlanets WHERE charID=%u ORDER BY planetID LIMIT 1",
            m_client.GetCharacterID()))
            return false;
        if (colonies.GetRow(row)) {
            const uint32 colonySystem = row.GetUInt(0);
            for (uint32 candidate : bases) {
                StationData candidateData;
                if (stDataMgr.GetStationData(candidate, candidateData) && candidateData.systemID == colonySystem) {
                    base = candidate;
                    break;
                }
            }
        }
    }
    StationData destination;
    GateRoute route;
    if (!stDataMgr.GetStationData(base, destination) ||
        !GetNextGateRoute(m_client.GetSystemID(), destination.systemID, route, true))
        return false;
    const uint32 oldHome = m_record.homeStationID;
    const uint32 oldBelt = m_record.currentBeltID;
    m_record.homeStationID = base;
    m_record.currentBeltID = 0;
    if (!BotDB::SaveState(m_record)) {
        m_record.homeStationID = oldHome;
        m_record.currentBeltID = oldBelt;
        return false;
    }
    m_homeTransitComplete = false;
    m_transitStationID = 0;
    SetState(State::Docked);
    sLog.Green("PlayerBot", "%s is moving its industrial base to station %u.", m_client.GetName(), base);
    return true;
}

bool BotBrain::PreparePlanetary()
{
    // Start with the hauler cohort: its real industrial hold can carry a
    // command center while it continues its ordinary work. Provisioning is a
    // market purchase, never a direct item or colony database insertion.
    if (!sConfig.cosmic.PIEnabled || !m_planetaryTaskStoreAvailable || m_record.profession != "HAULER" ||
        m_manualMining || m_state == State::Attacking || m_state == State::Returning ||
        !m_client.IsDocked() || m_client.GetSystemID() != 30000142 ||
        m_client.GetShip().get() == nullptr || m_client.SystemMgr() == nullptr)
        return false;
    const uint64_t now = GetFileTimeNow();
    if (m_lastPlanetaryCheckAt && now - m_lastPlanetaryCheckAt < 600ULL * 10000000ULL)
        return false;
    m_lastPlanetaryCheckAt = now;

    DBQueryResult colonies;
    if (!sDatabase.RunQuery(colonies, "SELECT 1 FROM piPlanets WHERE charID=%u LIMIT 1",
                            m_client.GetCharacterID()))
        return false;
    DBResultRow row;
    if (colonies.GetRow(row))
        return false;

    static const uint16 commandCenterType = 2524; // Crucible barren command center
    ShipItemRef ship = m_client.GetShip();
    std::multimap<uint8, InventoryItemRef> cargo;
    ship->GetMyInventory()->GetCargoList(cargo);
    for (const auto& entry : cargo) {
        const InventoryItemRef& item = entry.second;
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            item->typeID() == commandCenterType)
            return false;
    }
    const uint32 stationID = m_client.GetStationID();
    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(stationID);
    const ItemType* type = sItemFactory.GetType(commandCenterType);
    if (station.get() == nullptr || type == nullptr ||
        ship->GetMyInventory()->GetRemainingCapacity(flagCargoHold) < type->volume())
        return false;

    std::vector<InventoryItemRef> items;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, items);
    for (const InventoryItemRef& item : items) {
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->locationID() != stationID || item->typeID() != commandCenterType ||
            item->isSingleton())
            continue;
        InventoryItemRef center = item;
        if (item->quantity() > 1)
            center = item->Split(1);
        if (center.get() == nullptr)
            return false;
        m_client.MoveItem(center->itemID(), ship->itemID(), flagCargoHold);
        return center->locationID() == ship->itemID() && center->flag() == flagCargoHold;
    }

    const uint32 orderID = MarketDB::FindSellOrder(commandCenterType, stationID, 1, 1.0e12);
    Market::OrderInfo order;
    if (!orderID || !MarketDB::GetOrderInfo(orderID, order) ||
        order.ownerID == m_client.GetCharacterID() || order.price <= 0 ||
        m_client.GetBalance() < order.price + 1000000.0)
        return false;
    sMktMgr.ExecuteSellOrder(&m_client, orderID, 1, order.price, stationID,
                             commandCenterType, false);
    // Load the settled item immediately; the normal mining loop may undock on
    // its next tick, so leaving it in the hangar would strand the purchase.
    items.clear();
    station->GetMyInventory()->GetItemsByFlag(flagHangar, items);
    for (const InventoryItemRef& item : items) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            item->locationID() == stationID && item->typeID() == commandCenterType &&
            !item->isSingleton() && item->quantity() == 1) {
            m_client.MoveItem(item->itemID(), ship->itemID(), flagCargoHold);
            break;
        }
    }
    return true;
}

bool BotBrain::DeployPlanetary()
{
    if (!sConfig.cosmic.PIEnabled || !m_planetaryTaskStoreAvailable || m_record.profession != "HAULER" ||
        m_manualMining || m_state == State::Attacking || !m_client.IsInSpace() ||
        m_client.GetSystemID() == 30000142 || m_client.SystemMgr() == nullptr ||
        m_client.GetShip().get() == nullptr)
        return false;
    const uint64_t now = GetFileTimeNow();
    if (m_lastPlanetarySpaceCheckAt && now - m_lastPlanetarySpaceCheckAt < 60ULL * 10000000ULL)
        return false;
    m_lastPlanetarySpaceCheckAt = now;

    DBQueryResult launches;
    if (!sDatabase.RunQuery(launches,
        "SELECT itemID FROM piLaunches WHERE charID=%u AND solarSystemID=%u AND status=0 "
        "ORDER BY launchID LIMIT 1", m_client.GetCharacterID(), m_client.GetSystemID()))
        return false;
    DBResultRow launchRow;
    if (launches.GetRow(launchRow)) {
        const uint32 itemID = launchRow.GetUInt(0);
        SystemEntity* entity = m_client.SystemMgr()->FindEntityByID(itemID);
        if (entity != nullptr && entity->GetSelf()->ownerID() == m_client.GetCharacterID()) {
            m_planetaryPickupID = itemID;
            m_planetarySystemID = m_client.GetSystemID();
            if (!SavePlanetaryTask()) {
                m_planetaryPickupID = 0;
                return false;
            }
            StopMining();
            SetState(State::WarpToBelt);
            return true;
        }
    }

    DBQueryResult existing;
    if (!sDatabase.RunQuery(existing,
        "SELECT planetID FROM piPlanets WHERE charID=%u AND solarSystemID=%u ORDER BY planetID LIMIT 1",
        m_client.GetCharacterID(), m_client.GetSystemID()))
        return false;
    DBResultRow existingRow;
    if (existing.GetRow(existingRow)) {
        SystemEntity* entity = m_client.SystemMgr()->GetPlanet(existingRow.GetUInt(0));
        if (entity == nullptr || !entity->IsPlanetSE())
            return false;
        PlanetSE* planet = entity->GetPlanetSE();
        Colony* colony = planet->GetColony(&m_client);
        colony->Init();
        if (!colony->HasColony())
            return false;
        colony->Update();
        const PI_CCPin& network = colony->GetNetwork();
        const auto center = network.pins.find(network.ccPinID);
        if (center != network.pins.end() && FindPlanetarySaleStation(m_client.GetSystemID()) != 0) {
            const double freeVolume = m_client.GetShip()->GetMyInventory()->GetRemainingCapacity(flagCargoHold);
            for (const auto& stored : center->second.contents) {
                const ItemType* type = sItemFactory.GetType(stored.first);
                if (type == nullptr || type->volume() <= 0.0 || stored.second < 1000 || freeVolume < type->volume())
                    continue;
                const uint32 amount = static_cast<uint32>(std::min<double>(stored.second,
                    std::min<double>(50000.0, std::floor(freeVolume / type->volume()))));
                if (amount == 0)
                    continue;
                std::map<uint16, uint32> commodities;
                commodities.emplace(stored.first, amount);
                StopMining();
                PyRep* result = colony->LaunchCommodities(network.ccPinID, commodities);
                if (result == nullptr)
                    return false;
                PySafeDecRef(result);
                SetState(State::WarpToBelt);
                // The launch itself records its container in piLaunches. Read
                // that durable record before issuing the physical pickup.
                m_lastPlanetarySpaceCheckAt = 0;
                return true;
            }
        }
        return ConfigurePlanetary(*planet, *colony);
    }

    std::multimap<uint8, InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetCargoList(cargo);
    InventoryItemRef center(nullptr);
    for (const auto& entry : cargo) {
        if (entry.second.get() != nullptr && entry.second->ownerID() == m_client.GetCharacterID() &&
            entry.second->typeID() == 2524 && entry.second->locationID() == m_client.GetShipID() &&
            entry.second->flag() == flagCargoHold && !entry.second->isSingleton()) {
            center = entry.second;
            break;
        }
    }
    if (center.get() == nullptr)
        return false;
    DBQueryResult planets;
    if (!sDatabase.RunQuery(planets,
        "SELECT itemID FROM mapDenormalize WHERE solarSystemID=%u AND typeID=2016 "
        "ORDER BY itemID LIMIT 16", m_client.GetSystemID()))
        return false;
    std::vector<uint32> candidates;
    DBResultRow row;
    while (planets.GetRow(row))
        candidates.push_back(row.GetUInt(0));
    if (candidates.empty())
        return false;

    const uint32 planetID = candidates[m_record.slot % candidates.size()];
    SystemEntity* entity = m_client.SystemMgr()->GetPlanet(planetID);
    if (entity == nullptr || !entity->IsPlanetSE())
        return false;
    PlanetSE* planet = entity->GetPlanetSE();
    Colony* colony = planet->GetColony(&m_client);
    colony->Init();
    if (colony->HasColony())
        return false;

    PyTuple* data = new PyTuple(4);
    data->SetItem(0, new PyInt(center->itemID()));
    data->SetItem(1, new PyInt(center->typeID()));
    data->SetItem(2, new PyFloat(0.05 + (m_record.slot % 20) * 0.01));
    data->SetItem(3, new PyFloat(0.05 + (m_record.slot % 15) * 0.01));
    PyTuple* command = new PyTuple(2);
    command->SetItem(0, new PyInt(PI::Command::CreatePin));
    command->SetItem(1, data);
    PyList* commands = new PyList();
    commands->AddItem(command);
    PlanetMgr manager(&m_client, planet, colony);
    PyRep* result = manager.UpdateNetwork(commands);
    PySafeDecRef(result);
    PySafeDecRef(commands);
    if (!colony->HasColony())
        return false;
    sLog.Green("PlayerBot", "%s deployed a purchased command center on planet %u.",
               m_client.GetName(), planetID);
    return true;
}

bool BotBrain::ProcessPlanetaryPickup()
{
    if (!m_planetaryPickupID || m_client.SystemMgr() == nullptr || m_client.GetShip().get() == nullptr)
        return false;
    if (m_client.GetSystemID() != m_planetarySystemID) {
        DBQueryResult stations;
        DBResultRow row;
        if (sDatabase.RunQuery(stations, "SELECT stationID FROM staStations WHERE solarSystemID=%u LIMIT 1",
                               m_planetarySystemID) && stations.GetRow(row))
            ProcessStationTransit(row.GetUInt(0));
        return true;
    }
    if (m_client.IsDocked()) {
        if (m_client.IsIdle() && !m_client.IsInvul())
            m_client.UndockFromStation();
        return true;
    }
    if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr)
        return true;
    SystemEntity* entity = m_client.SystemMgr()->FindEntityByID(m_planetaryPickupID);
    CargoContainerRef container = sItemFactory.GetCargoRef(m_planetaryPickupID);
    if (entity == nullptr || container.get() == nullptr ||
        container->ownerID() != m_client.GetCharacterID() ||
        container->locationID() != m_client.GetSystemID()) {
        m_planetaryPickupID = 0;
        if (!m_planetaryCargoTypeID)
            m_planetarySaleStationID = 0;
        SavePlanetaryTask();
        return false;
    }
    DestinyManager* destiny = m_client.GetShipSE()->DestinyMgr();
    if (destiny == nullptr)
        return true;
    const double distance = m_client.GetShipSE()->GetPosition().distance(entity->GetPosition());
    if (distance > 150000.0) {
        destiny->WarpTo(entity->GetPosition(), 0);
        return true;
    }
    if (distance > 2000.0) {
        destiny->Follow(entity, 1500);
        return true;
    }

    Inventory* contents = container->GetMyInventory();
    Inventory* cargo = m_client.GetShip()->GetMyInventory();
    if (contents == nullptr || cargo == nullptr || !contents->LoadContents())
        return true;
    if (!m_planetarySaleStationID)
        m_planetarySaleStationID = FindPlanetarySaleStation(m_client.GetSystemID());
    if (!m_planetarySaleStationID)
        return true;
    std::map<uint32, InventoryItemRef> items;
    contents->GetInventoryMap(items);
    for (const auto& entry : items) {
        const InventoryItemRef& item = entry.second;
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->locationID() != container->itemID() || !cargo->HasAvailableSpace(flagCargoHold, item))
            continue;
        const uint16 typeID = item->typeID();
        const uint16 previousType = m_planetaryCargoTypeID;
        m_planetaryCargoTypeID = typeID;
        if (!SavePlanetaryTask())
            return true;
        m_client.MoveItem(item->itemID(), m_client.GetShipID(), flagCargoHold);
        if (item->locationID() != m_client.GetShipID() || item->flag() != flagCargoHold) {
            m_planetaryCargoTypeID = previousType;
            SavePlanetaryTask();
        }
    }
    const bool empty = contents->IsEmpty();
    m_planetaryPickupID = 0;
    if (m_planetaryCargoTypeID == 0) {
        m_planetarySaleStationID = 0;
        SavePlanetaryTask();
        SetState(State::WarpToBelt);
        return false;
    }
    if (!SavePlanetaryTask())
        return true;
    if (empty)
        container->Delete(); // only the bot's now-empty launch container
    return true;
}

bool BotBrain::SavePlanetaryTask()
{
    if (!m_planetaryTaskStoreAvailable)
        return false;
    DBerror error;
    if (!sDatabase.RunQuery(error,
        "INSERT INTO botPlanetaryTasks(characterID,pickupID,solarSystemID,saleStationID,cargoTypeID) "
        "VALUES(%u,%u,%u,%u,%u) ON DUPLICATE KEY UPDATE pickupID=VALUES(pickupID), "
        "solarSystemID=VALUES(solarSystemID), saleStationID=VALUES(saleStationID), cargoTypeID=VALUES(cargoTypeID)",
        m_client.GetCharacterID(), m_planetaryPickupID, m_planetarySystemID,
        m_planetarySaleStationID, m_planetaryCargoTypeID)) {
        sLog.Warning("PlayerBot", "Could not persist planetary task for %s: %s.", m_client.GetName(), error.GetError());
        return false;
    }
    return true;
}

bool BotBrain::ProcessPlanetarySale()
{
    if (!m_planetarySaleStationID || !m_planetaryCargoTypeID)
        return false;
    if (!m_client.IsDocked() || m_client.GetStationID() != m_planetarySaleStationID) {
        ProcessStationTransit(m_planetarySaleStationID);
        return true;
    }
    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(m_planetarySaleStationID);
    MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
    const ItemType* type = sItemFactory.GetType(m_planetaryCargoTypeID);
    if (station.get() == nullptr || market == nullptr || type == nullptr)
        return true;

    std::vector<InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlag(flagCargoHold, cargo);
    for (const InventoryItemRef& item : cargo) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            item->typeID() == m_planetaryCargoTypeID)
            m_client.MoveItem(item->itemID(), m_planetarySaleStationID, flagHangar);
    }
    if (m_client.GetSystemID() != 30000142 &&
        MarketDB::FindOwnerOrder(m_client.GetCharacterID(), m_planetarySaleStationID,
                                 m_planetaryCargoTypeID, false) == 0) {
        std::vector<InventoryItemRef> hangar;
        station->GetMyInventory()->GetItemsByFlag(flagHangar, hangar);
        for (const InventoryItemRef& item : hangar) {
            if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
                item->locationID() != m_planetarySaleStationID || item->typeID() != m_planetaryCargoTypeID)
                continue;
            const double price = std::round(type->basePrice() * 110.0) / 100.0;
            const double headroom = sConfig.playerbots.maxISK - m_client.GetBalance();
            if (price > 0.0 && headroom > 0.0) {
                const uint32 quantity = std::min<uint32>(item->quantity(),
                    static_cast<uint32>(std::min<double>(10000.0, std::floor(headroom / price))));
                if (quantity && !market->PlaceBotLimitOrder(m_client, m_planetarySaleStationID,
                        item->typeID(), price, quantity, false, item->itemID(), 7))
                    return true;
            }
            break;
        }
    }
    m_planetarySaleStationID = 0;
    m_planetaryCargoTypeID = 0;
    m_planetarySystemID = 0;
    SavePlanetaryTask();
    m_transitStationID = 0;
    SetState(State::Returning);
    return true;
}

bool BotBrain::ConfigurePlanetary(PlanetSE& planet, Colony& colony)
{
    // Use the same command decoder and colony machinery as UserUpdateNetwork.
    // Work in small steps so normal piloting can proceed between PI actions.
    const auto runCommand = [&](uint32 code, PyTuple* data) {
        PyTuple* command = new PyTuple(2);
        command->SetItem(0, new PyInt(code));
        command->SetItem(1, data);
        PyList* commands = new PyList();
        commands->AddItem(command);
        PlanetMgr manager(&m_client, &planet, &colony);
        PyRep* result = manager.UpdateNetwork(commands);
        PySafeDecRef(result);
        PySafeDecRef(commands);
    };
    const PI_CCPin& network = colony.GetNetwork();
    const auto center = network.pins.find(network.ccPinID);
    if (center == network.pins.end() || !center->second.isCommandCenter)
        return false;
    uint32 ecuID = 0;
    for (const auto& pin : network.pins)
        if (pin.second.isECU) {
            ecuID = pin.first;
            break;
        }
    if (!ecuID) {
        if (m_client.GetBalance() < 100000)
            return false;
        PyTuple* ids = new PyTuple(2);
        ids->SetItem(0, new PyInt(network.ccPinID));
        ids->SetItem(1, new PyInt(1));
        PyTuple* data = new PyTuple(4);
        data->SetItem(0, ids);
        data->SetItem(1, new PyInt(2848)); // barren extractor control unit
        data->SetItem(2, new PyFloat(center->second.latitude + 0.001));
        data->SetItem(3, new PyFloat(center->second.longitude + 0.001));
        runCommand(PI::Command::CreatePin, data);
        return true;
    }
    const auto ecu = network.pins.find(ecuID);
    if (ecu == network.pins.end())
        return false;
    if (ecu->second.heads.empty()) {
        PyTuple* data = new PyTuple(4);
        data->SetItem(0, new PyInt(ecuID));
        data->SetItem(1, new PyInt(0));
        data->SetItem(2, new PyFloat(ecu->second.latitude + 0.001));
        data->SetItem(3, new PyFloat(ecu->second.longitude + 0.001));
        runCommand(PI::Command::AddExtractorHead, data);
        return true;
    }
    bool linked = false;
    for (const auto& entry : network.links) {
        const PI_Link& link = entry.second;
        if ((link.endpoint1 == ecuID && link.endpoint2 == network.ccPinID) ||
            (link.endpoint2 == ecuID && link.endpoint1 == network.ccPinID)) {
            linked = true;
            break;
        }
    }
    if (!linked) {
        PyTuple* data = new PyTuple(3);
        data->SetItem(0, new PyInt(ecuID));
        data->SetItem(1, new PyInt(network.ccPinID));
        data->SetItem(2, new PyInt(0));
        runCommand(PI::Command::CreateLink, data);
        return true;
    }
    if (ecu->second.state == PI::Pin::State::Active)
        return false;

    std::vector<uint16> resources;
    sPlanetDataMgr.GetPlanetData(planet.GetTypeID(), resources);
    if (resources.empty())
        return false;
    std::sort(resources.begin(), resources.end());
    const uint16 resource = resources[m_record.slot % resources.size()];
    const float radius = 0.015f;
    PyList* heads = new PyList();
    for (const auto& head : ecu->second.heads) {
        PyTuple* position = new PyTuple(3);
        position->SetItem(0, new PyInt(head.first));
        position->SetItem(1, new PyFloat(head.second.latitude));
        position->SetItem(2, new PyFloat(head.second.longitude));
        heads->AddItem(position);
    }
    PyRep* survey = sPIDataMgr.GetProgramResultInfo(&colony, ecuID, resource, heads, radius);
    PySafeDecRef(heads);
    if (survey == nullptr || !survey->IsTuple() || survey->AsTuple()->size() != 3) {
        PySafeDecRef(survey);
        return false;
    }
    const uint32 quantity = PyRep::IntegerValueU32(survey->AsTuple()->GetItem(0));
    PySafeDecRef(survey);
    if (!quantity)
        return false;
    bool routed = false;
    for (const auto& entry : network.routes) {
        const PI_Route& route = entry.second;
        if (route.srcPinID == ecuID && route.destPinID == network.ccPinID &&
            route.commodityTypeID == resource) {
            routed = true;
            break;
        }
    }
    if (!routed) {
        PyTuple* routeID = new PyTuple(2);
        routeID->SetItem(0, new PyInt(ecuID));
        routeID->SetItem(1, new PyInt(1));
        PyList* path = new PyList();
        path->AddItem(new PyInt(ecuID));
        path->AddItem(new PyInt(network.ccPinID));
        PyTuple* data = new PyTuple(4);
        data->SetItem(0, routeID);
        data->SetItem(1, path);
        data->SetItem(2, new PyInt(resource));
        data->SetItem(3, new PyInt(quantity));
        runCommand(PI::Command::CreateRoute, data);
    }
    PyTuple* install = new PyTuple(3);
    install->SetItem(0, new PyInt(ecuID));
    install->SetItem(1, new PyInt(resource));
    install->SetItem(2, new PyFloat(radius));
    runCommand(PI::Command::InstallProgram, install);
    return true;
}

void BotBrain::ProcessMissionRunner()
{
    const uint64_t now = GetFileTimeNow();
    const uint32 characterID = m_client.GetCharacterID();
    std::vector<MissionOffer> offers;
    sMissionDataMgr.LoadMissionOffers(characterID, offers);

    MissionOffer activeOffer = MissionOffer();
    bool haveAcceptedOffer = false;
    bool haveOfferedMission = false;
    for (MissionOffer& offer : offers) {
        if (offer.typeID != Mission::Type::Courier)
            continue;
        if (offer.stateID == Mission::State::Accepted) {
            activeOffer = offer;
            haveAcceptedOffer = true;
            break;
        }
        if (!haveOfferedMission &&
            (offer.stateID == Mission::State::Offered || offer.stateID == Mission::State::Allocated)) {
            activeOffer = offer;
            haveOfferedMission = true;
        }
    }

    if (haveAcceptedOffer || haveOfferedMission) {
        Agent* agent = sEntityList.GetAgent(activeOffer.agentID);
        if (agent == nullptr) {
            sLog.Warning("PlayerBot", "%s cannot load mission agent %u for offer %u.",
                         m_client.GetName(), activeOffer.agentID, activeOffer.offerID);
            m_missionRetryAt = now + 60ULL * 10000000ULL;
            return;
        }

        const uint32 homeRegionID = sDataMgr.GetStationRegion(HubStationFor(m_client, m_record));
        const bool validCourier = activeOffer.offerID != 0 && activeOffer.originID != 0 && activeOffer.destinationID != 0 &&
            activeOffer.courierTypeID != 0 && activeOffer.courierAmount != 0 &&
            IsSafeMissionStation(activeOffer.originID, homeRegionID) &&
            IsSafeMissionStation(activeOffer.destinationID, homeRegionID);
        if (!validCourier) {
            if (haveAcceptedOffer && activeOffer.courierTypeID != 0)
                m_client.RemoveMissionItem(activeOffer.courierTypeID, activeOffer.courierAmount);
            agent->DeleteOffer(characterID);
            if (haveAcceptedOffer)
                agent->UpdateStandings(&m_client, Standings::MissionFailure, activeOffer.important);
            m_missionRetryAt = now + 45ULL * 10000000ULL;
            sLog.Warning("PlayerBot", "%s rejected unsupported or unsafe courier offer %u.",
                         m_client.GetName(), activeOffer.offerID);
            return;
        }

        if (!haveAcceptedOffer) {
            if (activeOffer.expiryTime < now) {
                agent->DeleteOffer(characterID);
                m_missionRetryAt = now + 30ULL * 10000000ULL;
                return;
            }
            const uint32 offerAgentStationID = agent->GetStationID();
            if (!m_client.IsDocked() || static_cast<uint32>(m_client.GetStationID()) != offerAgentStationID) {
                ProcessStationTransit(offerAgentStationID);
                return;
            }
            if (!agent->CanUseAgent(&m_client)) {
                agent->DeleteOffer(characterID);
                m_missionRetryAt = now + 5ULL * 60ULL * 10000000ULL;
                return;
            }

            const Inventory* cargo = m_client.GetShip()->GetMyInventory();
            if (cargo == nullptr) {
                m_missionRetryAt = now + 60ULL * 10000000ULL;
                return;
            }
            const float missionVolume = activeOffer.courierItemVolume * activeOffer.courierAmount;
            if (missionVolume <= 0.0f || missionVolume > cargo->GetRemainingCapacity(flagCargoHold)) {
                agent->DeleteOffer(characterID);
                m_missionRetryAt = now + 60ULL * 10000000ULL;
                sLog.Warning("PlayerBot", "%s declined courier offer %u: package volume %.2f m3 exceeds cargo space.",
                             m_client.GetName(), activeOffer.offerID, missionVolume);
                return;
            }

            if (!agent->AcceptMission(&m_client, activeOffer)) {
                agent->DeleteOffer(characterID);
                m_missionRetryAt = now + 60ULL * 10000000ULL;
                return;
            }
            sLog.Green("PlayerBot", "%s accepted real level-1 courier mission %u: %u units to station %u.",
                       m_client.GetName(), activeOffer.offerID, activeOffer.courierAmount,
                       activeOffer.destinationID);
            return;
        }

        if (activeOffer.expiryTime < now) {
            if (activeOffer.courierTypeID != 0)
                m_client.RemoveMissionItem(activeOffer.courierTypeID, activeOffer.courierAmount);
            agent->DeleteOffer(characterID);
            agent->UpdateStandings(&m_client, Standings::MissionFailure, activeOffer.important);
            m_missionRetryAt = now + 45ULL * 10000000ULL;
            _log(AGENT__MESSAGE, "PlayerBot %s failed expired courier mission %u.",
                 m_client.GetName(), activeOffer.offerID);
            return;
        }

        const bool delivered = m_client.IsDocked() &&
            static_cast<uint32>(m_client.GetStationID()) == activeOffer.destinationID &&
            m_client.IsMissionComplete(activeOffer);
        if (delivered) {
            if (agent->CompleteMission(&m_client, activeOffer)) {
                agent->RemoveOffer(characterID);
                m_missionRetryAt = now + (30ULL + (characterID * 13ULL) % 90ULL) * 10000000ULL;
                SetState(State::Docked);
                sLog.Green("PlayerBot", "%s completed courier mission %u through the agent service.",
                           m_client.GetName(), activeOffer.offerID);
            }
            return;
        }

        const ShipItemRef ship = m_client.GetShip();
        if (ship.get() == nullptr || ship->GetMyInventory() == nullptr) {
            m_missionRetryAt = now + 60ULL * 10000000ULL;
            return;
        }
        bool packageLoaded = ship->GetMyInventory()->ContainsTypeQtyByFlag(
            activeOffer.courierTypeID, flagCargoHold, activeOffer.courierAmount);
        if (!packageLoaded && m_client.IsDocked() &&
            static_cast<uint32>(m_client.GetStationID()) == activeOffer.originID) {
            StationItemRef origin = m_client.SystemMgr() == nullptr ? StationItemRef(nullptr) :
                m_client.SystemMgr()->GetStationFromInventory(activeOffer.originID);
            if (origin.get() != nullptr) {
                InventoryItemRef package = origin->GetMyInventory()->GetByTypeFlag(
                    activeOffer.courierTypeID, flagHangar);
                if (package.get() != nullptr && package->ownerID() == characterID &&
                    package->quantity() >= activeOffer.courierAmount &&
                    ship->GetMyInventory()->HasAvailableSpace(flagCargoHold, package)) {
                    m_client.MoveItem(package->itemID(), ship->itemID(), flagCargoHold);
                    packageLoaded = ship->GetMyInventory()->ContainsTypeQtyByFlag(
                        activeOffer.courierTypeID, flagCargoHold, activeOffer.courierAmount);
                }
            }
        }

        if (!packageLoaded) {
            if (m_client.IsDocked() && static_cast<uint32>(m_client.GetStationID()) == activeOffer.destinationID) {
                agent->DeleteOffer(characterID);
                agent->UpdateStandings(&m_client, Standings::MissionFailure, activeOffer.important);
                m_missionRetryAt = now + 60ULL * 10000000ULL;
                return;
            }
            ProcessStationTransit(activeOffer.originID);
            return;
        }

        if (m_client.IsDocked() && static_cast<uint32>(m_client.GetStationID()) == activeOffer.destinationID) {
            // The station hangar package path is also accepted by the normal
            // courier completion check if origin and destination are equal.
            if (agent->CompleteMission(&m_client, activeOffer)) {
                agent->RemoveOffer(characterID);
                m_missionRetryAt = now + (30ULL + (characterID * 13ULL) % 90ULL) * 10000000ULL;
                SetState(State::Docked);
                sLog.Green("PlayerBot", "%s completed courier mission %u through the agent service.",
                           m_client.GetName(), activeOffer.offerID);
            }
            return;
        }

        ProcessStationTransit(activeOffer.destinationID);
        return;
    }

    if (now < m_missionRetryAt) {
        if (m_client.IsDocked())
            ProcessTrader(false);
        return;
    }

    if (m_missionAgentID == 0 || m_missionAgentStationID == 0) {
        std::vector<MissionAgentLocation> candidates;
        if (!GetMissionAgentLocations(HubStationFor(m_client, m_record), candidates)) {
            m_missionRetryAt = now + 5ULL * 60ULL * 10000000ULL;
            ProcessTrader(false);
            return;
        }

        const size_t start = (m_record.slot != 0 ? m_record.slot - 1 : characterID) % candidates.size();
        for (size_t offset = 0; offset < candidates.size(); ++offset) {
            const MissionAgentLocation& candidate = candidates[(start + offset) % candidates.size()];
            Agent* agent = sEntityList.GetAgent(candidate.agentID);
            if (agent == nullptr || !agent->CanUseAgent(&m_client))
                continue;
            m_missionAgentID = candidate.agentID;
            m_missionAgentStationID = candidate.stationID;
            break;
        }
        if (m_missionAgentID == 0) {
            m_missionRetryAt = now + 5ULL * 60ULL * 10000000ULL;
            ProcessTrader(false);
            return;
        }
        _log(AGENT__MESSAGE, "PlayerBot %s selected distribution agent %u at station %u.",
             m_client.GetName(), m_missionAgentID, m_missionAgentStationID);
    }

    if (!IsSafeMissionStation(m_missionAgentStationID,
                              sDataMgr.GetStationRegion(HubStationFor(m_client, m_record)))) {
        m_missionAgentID = 0;
        m_missionAgentStationID = 0;
        m_missionRetryAt = now + 5ULL * 60ULL * 10000000ULL;
        return;
    }

    if (!m_client.IsDocked() || static_cast<uint32>(m_client.GetStationID()) != m_missionAgentStationID) {
        ProcessStationTransit(m_missionAgentStationID);
        return;
    }

    Agent* agent = sEntityList.GetAgent(m_missionAgentID);
    if (agent == nullptr || !agent->CanUseAgent(&m_client)) {
        m_missionAgentID = 0;
        m_missionAgentStationID = 0;
        m_missionRetryAt = now + 5ULL * 60ULL * 10000000ULL;
        return;
    }

    MissionOffer offer = MissionOffer();
    agent->MakeOffer(characterID, offer);
    if (offer.offerID == 0 || offer.typeID != Mission::Type::Courier ||
        !IsSafeMissionStation(offer.originID, sDataMgr.GetStationRegion(HubStationFor(m_client, m_record))) ||
        !IsSafeMissionStation(offer.destinationID, sDataMgr.GetStationRegion(HubStationFor(m_client, m_record)))) {
        if (offer.offerID != 0)
            agent->DeleteOffer(characterID);
        m_missionRetryAt = now + 60ULL * 10000000ULL;
        return;
    }

    // The real courier offer is already persisted in agtOffers. Acceptance is
    // handled on the next tick through the same Agent path as a player.
    sLog.Green("PlayerBot", "%s received real courier offer %u from agent %u.",
               m_client.GetName(), offer.offerID, m_missionAgentID);
}

bool BotBrain::IsMiningShipReady() const
{
    if (m_client.GetShip().get() == nullptr || m_client.GetShip()->GetModuleManager() == nullptr)
        return false;
    std::vector<InventoryItemRef> modules;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlagRange(flagHiSlot0, flagHiSlot7, modules);
    for (const InventoryItemRef& module : modules)
        if (module->groupID() == EVEDB::invGroups::Mining_Laser || module->groupID() == EVEDB::invGroups::Strip_Miner)
            return true;
    return false;
}

bool BotBrain::IsCombatShipReady() const
{
    if (m_client.GetShip().get() == nullptr || m_client.GetShip()->GetModuleManager() == nullptr)
        return false;
    std::vector<InventoryItemRef> modules;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlagRange(flagHiSlot0, flagHiSlot7, modules);
    for (const InventoryItemRef& module : modules) {
        if (module->type().HasEffect(EVEEffectID::targetAttack) ||
            module->type().HasEffect(EVEEffectID::projectileFired) ||
            module->type().HasEffect(EVEEffectID::missileLaunching))
            return true;
    }
    return false;
}

bool BotBrain::EnsureProfessionShip()
{
    const ShipItemRef currentShip = m_client.GetShip();
    if (currentShip.get() == nullptr || m_client.GetStationID() == 0 ||
        m_client.SystemMgr() == nullptr)
        return false;

    uint16 hullTypeID = 0;
    struct StarterModule {
        uint16 typeID;
        EVEItemFlags preferredSlot;
        uint16 chargeTypeID;
    };
    std::vector<StarterModule> loadout;
    const CharacterRef character = m_client.GetChar();
    const uint8 raceID = character.get() == nullptr ? 1 : character->race();
    const uint16 miningFrigate = raceID == 2 ? 599 : // Minmatar Burst
                                 raceID == 4 ? 591 : // Amarr Tormentor
                                 raceID == 8 ? 592 : // Gallente Navitas
                                              582;  // Caldari Bantam
    const uint16 industrial = raceID == 2 ? 653 : // Minmatar Wreathe
                              raceID == 4 ? 1944 : // Amarr Bestower
                              raceID == 8 ? 655 : // Gallente Iteron Mark III (requires Gallente Industrial III)
                                           648;  // Caldari Badger
    const uint16 combatFrigate = raceID == 2 ? 587 : // Minmatar Rifter
                                 raceID == 4 ? 597 : // Amarr Punisher
                                 raceID == 8 ? 594 : // Gallente Incursus
                                              603;  // Caldari Merlin
    const uint16 racialTurret = raceID == 2 ? 485 : // 150mm Light AutoCannon I
                                raceID == 4 ? 450 : // Gatling Pulse Laser I
                                raceID == 8 ? 561 : // 75mm Gatling Rail I
                                             562;  // Light Electron Blaster I
    const uint16 racialAmmo = raceID == 2 ? 185 : // EMP S
                              raceID == 4 ? 246 : // Multifrequency S
                                           222;  // Antimatter Charge S
    if (m_record.profession == "MINER") {
        hullTypeID = miningFrigate;
        loadout.push_back({483, flagHiSlot0, 0}); // Miner I
        loadout.push_back({439, flagMidSlot0, 0}); // 1MN Afterburner I
    } else if (m_record.profession == "HAULER") {
        hullTypeID = industrial;
        loadout.push_back({483, flagHiSlot0, 0}); // Miner I for the real mining fallback
        loadout.push_back({1317, flagLowSlot0, 0}); // Expanded Cargohold I
    } else if (m_record.profession == "MISSION_RUNNER") {
        // Level-1 courier jobs carry real packages; use the correct racial
        // industrial hull so cargo volume, travel and fittings are real.
        hullTypeID = industrial;
        loadout.push_back({439, flagMidSlot0, 0}); // 1MN Afterburner I
        loadout.push_back({1317, flagLowSlot0, 0}); // Expanded Cargohold I
    } else if (m_record.profession == "RAT_HUNTER") {
        hullTypeID = combatFrigate;
        loadout.push_back({racialTurret, flagHiSlot0, racialAmmo});
        loadout.push_back({523, flagLowSlot0, 0}); // Small Armor Repairer I
    } else if (m_record.profession == "TRADER") {
        hullTypeID = industrial;
        loadout.push_back({439, flagMidSlot0, 0}); // 1MN Afterburner I
        loadout.push_back({1317, flagLowSlot0, 0}); // Expanded Cargohold I
    } else if (m_record.profession == "INDUSTRIALIST") {
        hullTypeID = industrial;
        loadout.push_back({1317, flagLowSlot0, 0}); // Expanded Cargohold I
    }
    if (hullTypeID == 0)
        return false;

    // Bootstrap ships issued by character creation in this database include
    // Ibis, Condor, Bantam, Osprey, Badger, Kestrel and Merlin. Replace only
    // those starter hulls with the role/race hull once; leave a later ship the
    // bot bought for itself alone.
    static const std::array<uint16, 16> bootstrapHulls = {{
        601, 582, 583, 587, 591, 592, 594, 597,
        599, 602, 603, 620, 648, 653, 657, 1944
    }};
    if (currentShip->typeID() != hullTypeID &&
        std::find(bootstrapHulls.begin(), bootstrapHulls.end(), currentShip->typeID()) == bootstrapHulls.end())
        return false;

    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(m_client.GetStationID());
    const ItemType* hullType = sItemFactory.GetType(hullTypeID);
    if (station.get() == nullptr || hullType == nullptr)
        return false;

    Client* previousClient = sItemFactory.GetUsingClient();
    sItemFactory.SetUsingClient(&m_client);
    const auto restoreFactoryClient = [&]() {
        if (previousClient != nullptr)
            sItemFactory.SetUsingClient(previousClient);
        else
            sItemFactory.UnsetUsingClient();
    };

    try {
        const uint32 characterID = m_client.GetCharacterID();
        const uint32 stationID = m_client.GetStationID();
        std::vector<InventoryItemRef> hangarItems;
        station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);

        ShipItemRef roleShip = currentShip->typeID() == hullTypeID ? currentShip : ShipItemRef(nullptr);
        for (const InventoryItemRef& item : hangarItems) {
            if (roleShip.get() != nullptr)
                break;
            if (item.get() == nullptr || item->ownerID() != characterID || item->typeID() != hullTypeID)
                continue;
            roleShip = sItemFactory.GetShipRef(item->itemID());
            if (roleShip.get() != nullptr)
                break;
        }

        if (roleShip.get() == nullptr) {
            const std::string shipName = m_client.GetName() + std::string("'s ") + hullType->name();
            ItemData shipData(hullTypeID, characterID, locTemp, flagNone, shipName.c_str());
            roleShip = sItemFactory.SpawnShip(shipData);
            if (roleShip.get() == nullptr) {
                restoreFactoryClient();
                return false;
            }
            roleShip->ChangeSingleton(true);
            roleShip->Move(stationID, flagHangar);
        }

        bool changed = currentShip->itemID() != roleShip->itemID();
        if (changed)
            m_client.SetShip(roleShip);

        ModuleManager* modules = roleShip->GetModuleManager();
        if (modules == nullptr || !modules->Initialize()) {
            restoreFactoryClient();
            return false;
        }

        for (const StarterModule& fit : loadout) {
            const ItemType* moduleType = sItemFactory.GetType(fit.typeID);
            if (moduleType == nullptr || moduleType->categoryID() != EVEDB::invCategories::Module) {
                restoreFactoryClient();
                return false;
            }

            EVEItemFlags fittedSlot = fit.preferredSlot;
            bool fitted = false;
            std::vector<InventoryItemRef> fittedItems;
            roleShip->GetMyInventory()->GetItemsByFlagRange(flagLowSlot0, flagHiSlot7, fittedItems);
            for (const InventoryItemRef& item : fittedItems) {
                if (item.get() != nullptr && item->ownerID() == characterID && item->typeID() == fit.typeID) {
                    fittedSlot = static_cast<EVEItemFlags>(item->flag());
                    fitted = true;
                    break;
                }
            }

            if (!fitted) {
                EVEEffectID rack = fit.preferredSlot >= flagHiSlot0 ? EVEEffectID::hiPower :
                                   fit.preferredSlot >= flagMidSlot0 ? EVEEffectID::medPower :
                                                                        EVEEffectID::loPower;
                if (modules->GetModule(fittedSlot) != nullptr)
                    fittedSlot = static_cast<EVEItemFlags>(modules->GetAvailableSlotInBank(rack));
                if (fittedSlot == flagIllegal) {
                    restoreFactoryClient();
                    return false;
                }

                ItemData moduleData(fit.typeID, characterID, locTemp, flagNone);
                InventoryItemRef module = sItemFactory.SpawnItem(moduleData);
                if (module.get() == nullptr) {
                    restoreFactoryClient();
                    return false;
                }
                module->ChangeSingleton(true);
                if (roleShip->AddItemByFlag(fittedSlot, module, &m_client) == 0) {
                    module->Move(stationID, flagHangar, true);
                    restoreFactoryClient();
                    return false;
                }
                module->SetAttribute(AttrOnline, EvilOne, false);
                module->SaveItem();
                changed = true;
            }

            if (fit.chargeTypeID != 0) {
                const ItemType* chargeType = sItemFactory.GetType(fit.chargeTypeID);
                if (chargeType == nullptr || chargeType->categoryID() != EVEDB::invCategories::Charge) {
                    restoreFactoryClient();
                    return false;
                }
                InventoryItemRef loaded = modules->GetLoadedChargeOnModule(fittedSlot);
                if (loaded.get() == nullptr || loaded->quantity() == 0) {
                    if (loaded.get() != nullptr)
                        modules->UnloadModule(fittedSlot);

                    const float moduleCapacity = moduleType->capacity();
                    const float chargeVolume = chargeType->volume();
                    if (moduleCapacity <= 0.0f || chargeVolume <= 0.0f) {
                        restoreFactoryClient();
                        return false;
                    }
                    const uint32 magazineSize = static_cast<uint32>(moduleCapacity / chargeVolume);
                    const uint32 chargeQuantity = std::max<uint32>(1, magazineSize * 10);
                    ItemData chargeData(fit.chargeTypeID, characterID, locTemp, flagNone, chargeQuantity);
                    InventoryItemRef charge = sItemFactory.SpawnItem(chargeData);
                    if (charge.get() == nullptr || roleShip->AddItemByFlag(fittedSlot, charge, &m_client) == 0) {
                        restoreFactoryClient();
                        return false;
                    }
                    if (charge->locationID() == locTemp && charge->quantity() > 0)
                        charge->Move(roleShip->itemID(), flagCargoHold, true);
                    changed = true;
                }
            }
        }

        roleShip->SaveShip();
        // SetShip above uses the same active-ship persistence path as a pilot
        // boarding a hull at a station; keep an already-correct active hull in
        // place so normal purchased ships are not needlessly re-boarded.
        restoreFactoryClient();
        if (changed)
            sLog.Green("PlayerBot", "%s boarded %s and fitted its role loadout.",
                       m_client.GetName(), hullType->name().c_str());
        return changed;
    } catch (...) {
        restoreFactoryClient();
        sLog.Warning("PlayerBot", "Could not install the %s starter hull for %s; will retry when docked.",
                     hullType->name().c_str(), m_client.GetName());
        return false;
    }
}

bool BotBrain::SelectNpcTarget()
{
    if (m_client.SystemMgr() == nullptr)
        return false;
    const std::map<uint32, SystemEntity*> entities = m_client.SystemMgr()->GetEntities();
    for (const auto& entry : entities) {
        NPC* npc = dynamic_cast<NPC*>(entry.second);
        if (npc == nullptr || npc->GetID() == m_client.GetShipID())
            continue;
        m_targetID = npc->GetID();
        return true;
    }
    return false;
}

void BotBrain::ReturnToHome()
{
    if (m_record.homeStationID == 0 || m_client.GetShipSE() == nullptr)
        return;
    if (m_client.GetShipSE()->DestinyMgr()->IsWarpingOrAligning())
        return;
    SystemEntity* station = m_client.SystemMgr()->FindEntityByID(m_record.homeStationID);
    if (station != nullptr) {
        m_client.GetShipSE()->DestinyMgr()->WarpTo(station->GetPosition(), 0);
        m_client.SetDockStationID(m_record.homeStationID);
    }
}

void BotBrain::ProcessRatHunter()
{
    switch (m_state) {
        case State::Docked:
            if (EnsureProfessionShip())
                return;
            if (!IsCombatShipReady())
                return;
            if (m_record.homeStationID == 0)
                m_record.homeStationID = m_client.GetStationID();
            m_client.UndockFromStation();
            SetState(State::Undocking);
            break;
        case State::Undocking:
            if (m_client.IsInSpace() && m_client.GetShipSE() != nullptr)
                SetState(State::WarpToBelt);
            break;
        case State::Traveling:
        case State::WarpToBelt:
            if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr)
                return;
            if (m_transitStationID != 0) {
                const uint32 destinationStationID = m_transitStationID;
                ProcessStationTransit(destinationStationID);
                if (m_client.IsDocked() && destinationStationID == m_record.homeStationID)
                    SetState(State::Selling);
                return;
            }
            if (ProcessBeltSystemTransit())
                return;
            if (!m_client.GetShipSE()->DestinyMgr()->IsWarpingOrAligning())
                WarpToBelt();
            break;
        case State::Mining:
            if (m_client.GetShipSE() == nullptr || m_client.GetShipSE()->DestinyMgr()->IsWarpingOrAligning())
                return;
            if (m_targetID == 0 && SelectNpcTarget())
                SetState(State::Attacking);
            else if (m_targetID == 0 && GetFileTimeNow() - m_stateSince >= 30ULL * 10000000ULL &&
                     !SelectAlternateBelt())
                SetState(State::Returning);
            break;
        case State::Attacking:
            ProcessAttack();
            if (m_targetID == 0)
                SetState(State::Returning);
            break;
        case State::Returning:
            if (m_record.homeStationID == 0)
                return;
            ProcessStationTransit(m_record.homeStationID);
            if (m_client.IsDocked())
                SetState(State::Docked);
            break;
        case State::Selling:
            if (m_client.IsInSpace())
                m_client.DockToStation();
            if (m_client.IsDocked())
                SetState(State::Docked);
            break;
        default:
            break;
    }
}

bool BotBrain::SellTraderStockAtStation(uint32 stationID)
{
    if (stationID == 0 || !m_client.IsDocked() || m_client.GetShip().get() == nullptr ||
        m_client.SystemMgr() == nullptr)
        return false;

    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(stationID);
    if (station.get() == nullptr)
        return false;

    // On arrival, move only trade goods from the ship into the station hangar.
    // Cargo is real inventory and can survive a restart while the delivery
    // destination is recalculated from current open orders.
    std::vector<InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlag(flagCargoHold, cargo);
    for (const InventoryItemRef& item : cargo) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            item->locationID() == m_client.GetShipID() && IsTraderMarketStock(item))
            m_client.MoveItem(item->itemID(), stationID, flagHangar);
    }

    if (m_client.GetSystemID() == 30000142)
        return false;

    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    for (const InventoryItemRef& item : hangarItems) {
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->locationID() != stationID || item->quantity() == 0 ||
            !IsTraderMarketStock(item))
            continue;

        const uint32 orderID = MarketDB::FindBuyOrder(item->typeID(), stationID, 1, 0.0,
                                                     m_client.GetCharacterID());
        Market::OrderInfo order;
        if (orderID == 0 || !MarketDB::GetOrderInfo(orderID, order) ||
            order.ownerID == m_client.GetCharacterID() || order.price <= 0.0)
            continue;

        const double headroom = sConfig.playerbots.maxISK - m_client.GetBalance();
        if (headroom <= 0.0)
            continue;
        const uint32 maxByWealth = static_cast<uint32>(std::min<double>(
            std::floor(headroom / order.price), 10000000.0));
        const uint32 quantity = std::min<uint32>(item->quantity(),
            std::min<uint32>(order.quantity, maxByWealth));
        if (quantity == 0 || !CanFillMinimumVolume(orderID, quantity, order.quantity))
            continue;

        InventoryItemRef saleItem = item;
        if (quantity < item->quantity()) {
            saleItem = item->Split(static_cast<int32>(quantity));
            if (saleItem.get() == nullptr)
                continue;
        }
        const std::string itemName = item->name();
        const uint32 itemTypeID = item->typeID();
        if (sMktMgr.ExecuteBuyOrder(&m_client, orderID, saleItem, quantity, false,
                                    itemTypeID, stationID, order.price)) {
            _log(MARKET__MESSAGE,
                 "PlayerBot %s filled real buy order %u with %u %s at station %u for %.2f ISK each.",
                 m_client.GetName(), orderID, quantity, itemName.c_str(), stationID, order.price);
            return true;
        }
    }
    return false;
}

bool BotBrain::SourcePlayerDemand(uint32 stationID)
{
    if (!m_client.IsDocked() || stationID == 0 || m_client.SystemMgr() == nullptr ||
        m_client.GetShip().get() == nullptr || m_client.GetShip()->GetMyInventory() == nullptr)
        return false;

    // Prioritize human bids, then supply industrial bots' backed material
    // orders. The market and physical cargo remain authoritative throughout.
    if (m_demandOrderID == 0) {
        DBQueryResult bids;
        if (!sDatabase.RunQuery(bids,
            "SELECT o.orderID,o.typeID,o.stationID,o.solarSystemID,o.price,o.volRemaining,o.minVolume "
            "FROM mktOrders o "
            "LEFT JOIN chrCharacters person ON person.characterID=o.ownerID "
            "LEFT JOIN botCharacters personBot ON personBot.characterID=person.characterID "
            "LEFT JOIN crpCorporation corp ON corp.corporationID=o.ownerID "
            "LEFT JOIN botCharacters corpCEO ON corpCEO.characterID=corp.ceoID "
            "WHERE o.bid=1 AND o.volRemaining>0 AND o.solarSystemID<>30000142 AND o.ownerID<>%u "
            "AND ((person.characterID IS NOT NULL AND (personBot.characterID IS NULL OR personBot.profession='INDUSTRIALIST')) "
            "OR (corp.corporationID>=98000000 AND corpCEO.characterID IS NULL)) "
            "ORDER BY (personBot.characterID IS NOT NULL),o.issued,o.orderID LIMIT 64", m_client.GetCharacterID()))
            return false;
        DBResultRow bid;
        while (bids.GetRow(bid)) {
            const uint32 orderID = bid.GetUInt(0);
            const uint32 typeID = bid.GetUInt(1);
            const uint32 destination = bid.GetUInt(2);
            const uint32 destinationSystem = bid.GetUInt(3);
            const double bidPrice = bid.GetDouble(4);
            const uint32 remaining = bid.GetUInt(5);
            const uint32 minimum = std::min(bid.GetUInt(6), remaining);
            const ItemType* type = typeID <= UINT16_MAX ? sItemFactory.GetType(typeID) : nullptr;
            const double capacity = m_client.GetShip()->GetMyInventory()->GetRemainingCapacity(flagCargoHold);
            if (type == nullptr || !type->published() || type->volume() <= 0.0 ||
                capacity < type->volume() * minimum || bidPrice <= 0.0 || destination == 0)
                continue;
            GateRoute route;
            if (destinationSystem != m_client.GetSystemID() &&
                !GetNextGateRoute(m_client.GetSystemID(), destinationSystem, route))
                continue;

            // Prefer existing supply outside Jita. Only fall back to its seeded
            // market when no affordable, profitable outside offer is reachable.
            DBQueryResult asks;
            if (!sDatabase.RunQuery(asks,
                "SELECT orderID,stationID,solarSystemID,price,volRemaining FROM mktOrders "
                "WHERE bid=0 AND typeID=%u AND ownerID<>%u AND volRemaining>0 "
                "AND price>0 AND price<%.2f ORDER BY (solarSystemID=30000142),price,orderID LIMIT 64",
                typeID, m_client.GetCharacterID(), bidPrice / 1.02))
                continue;
            DBResultRow ask;
            while (asks.GetRow(ask)) {
                const uint32 source = ask.GetUInt(1);
                const uint32 sourceSystem = ask.GetUInt(2);
                const double askPrice = ask.GetDouble(3);
                const uint32 affordable = askPrice > 0.0 ? static_cast<uint32>(std::min<double>(
                    std::floor(m_client.GetBalance() / askPrice), 10000000.0)) : 0;
                if (source == 0 || ask.GetUInt(4) < minimum || affordable < minimum ||
                    (sourceSystem != m_client.GetSystemID() &&
                     !GetNextGateRoute(m_client.GetSystemID(), sourceSystem, route)) ||
                    (sourceSystem != destinationSystem &&
                     !GetNextGateRoute(sourceSystem, destinationSystem, route)))
                    continue;
                m_demandOrderID = orderID;
                m_demandSourceStationID = source;
                break;
            }
            if (m_demandOrderID != 0)
                break;
        }
        if (m_demandOrderID == 0)
            return false;
    }

    if (stationID != m_demandSourceStationID) {
        m_transitStationID = m_demandSourceStationID;
        SetState(State::Traveling);
        return true;
    }

    DBQueryResult demand;
    if (!sDatabase.RunQuery(demand,
        "SELECT typeID,stationID,price,volRemaining,minVolume FROM mktOrders "
        "WHERE orderID=%u AND bid=1 AND volRemaining>0 AND solarSystemID<>30000142",
        m_demandOrderID))
        return true;
    DBResultRow bid;
    if (!demand.GetRow(bid)) {
        m_demandOrderID = m_demandSourceStationID = 0;
        return false;
    }
    const uint32 typeID = bid.GetUInt(0);
    const uint32 destination = bid.GetUInt(1);
    const double bidPrice = bid.GetDouble(2);
    const uint32 remaining = bid.GetUInt(3);
    const uint32 minimum = std::min(bid.GetUInt(4), remaining);
    const ItemType* type = typeID <= UINT16_MAX ? sItemFactory.GetType(typeID) : nullptr;
    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(stationID);
    if (type == nullptr || type->volume() <= 0.0 || station.get() == nullptr) {
        m_demandOrderID = m_demandSourceStationID = 0;
        return false;
    }
    const double capacity = m_client.GetShip()->GetMyInventory()->GetRemainingCapacity(flagCargoHold);
    const uint32 capacityQty = static_cast<uint32>(std::max<double>(0.0, std::min<double>(
        std::floor(capacity / type->volume()), 10000.0)));
    if (capacityQty < minimum) {
        m_demandOrderID = m_demandSourceStationID = 0;
        return false;
    }

    std::vector<InventoryItemRef> before;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, before);
    std::set<uint32> previousIDs;
    for (const InventoryItemRef& item : before)
        if (item.get() != nullptr)
            previousIDs.insert(item->itemID());

    DBQueryResult asks;
    if (!sDatabase.RunQuery(asks,
        "SELECT orderID,price,volRemaining FROM mktOrders WHERE stationID=%u AND typeID=%u "
        "AND bid=0 AND ownerID<>%u AND volRemaining>0 AND price>0 AND price<%.2f "
        "ORDER BY price,orderID LIMIT 1", stationID, typeID, m_client.GetCharacterID(), bidPrice / 1.02))
        return true;
    DBResultRow ask;
    if (!asks.GetRow(ask)) {
        m_demandOrderID = m_demandSourceStationID = 0;
        return false;
    }
    const double askPrice = ask.GetDouble(1);
    const uint32 affordable = static_cast<uint32>(std::min<double>(
        std::floor(m_client.GetBalance() / askPrice), 10000000.0));
    const uint32 quantity = std::min({remaining, capacityQty, ask.GetUInt(2), affordable});
    if (quantity == 0 || quantity < minimum) {
        m_demandOrderID = m_demandSourceStationID = 0;
        return false;
    }
    sMktMgr.ExecuteSellOrder(&m_client, ask.GetUInt(0), quantity, askPrice, stationID, typeID, false);

    std::vector<InventoryItemRef> after;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, after);
    for (const InventoryItemRef& item : after) {
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->typeID() != typeID || previousIDs.count(item->itemID()) != 0)
            continue;
        m_client.MoveItem(item->itemID(), m_client.GetShipID(), flagCargoHold);
        if (item->locationID() == m_client.GetShipID() && item->flag() == flagCargoHold) {
            sLog.Green("PlayerBot", "%s purchased %u %s for player buy order %u; travelling to station %u.",
                       m_client.GetName(), quantity, type->name().c_str(), m_demandOrderID, destination);
            m_demandOrderID = m_demandSourceStationID = 0;
            m_transitStationID = destination;
            SetState(State::Traveling);
            return true;
        }
        break;
    }
    sLog.Warning("PlayerBot", "%s purchased market stock but could not load it; it remains in station %u.",
                 m_client.GetName(), stationID);
    m_demandOrderID = m_demandSourceStationID = 0;
    return true;
}

bool BotBrain::StartRemoteMarketDelivery(uint32 stationID, uint32 itemID)
{
    if (stationID == 0 || itemID == 0 || !m_client.IsDocked() ||
        m_client.GetShip().get() == nullptr || m_client.SystemMgr() == nullptr)
        return false;

    InventoryItemRef item = sItemFactory.GetItemRef(itemID);
    if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
        item->locationID() != stationID || item->quantity() == 0 ||
        !IsTraderMarketStock(item))
        return false;

    const ItemType* itemType = sItemFactory.GetType(item->typeID());
    if (itemType == nullptr)
        return false;
    const double minimumPrice = std::max(0.01, itemType->basePrice() * 0.75);
    DBQueryResult orders;
    if (!sDatabase.RunQuery(orders,
        "SELECT orders.orderID, orders.stationID, destination.solarSystemID, "
        "orders.volRemaining, orders.ownerID, orders.price, orders.minVolume "
        "FROM mktOrders orders "
        "JOIN staStations destination ON destination.stationID=orders.stationID "
        "WHERE orders.bid=1 AND orders.typeID=%u AND orders.stationID<>%u "
        "AND orders.ownerID<>%u AND orders.volRemaining>0 AND orders.price>=%.2f "
        "ORDER BY orders.price DESC LIMIT 64",
        item->typeID(), stationID, m_client.GetCharacterID(), minimumPrice))
        return false;

    ShipItemRef ship = m_client.GetShip();
    Inventory* shipInventory = ship->GetMyInventory();
    if (shipInventory == nullptr)
        return false;
    const double cargoCapacity = shipInventory->GetCapacity(flagCargoHold);
    double cargoUsed = 0.0;
    std::vector<InventoryItemRef> cargo;
    shipInventory->GetItemsByFlag(flagCargoHold, cargo);
    for (const InventoryItemRef& carried : cargo) {
        if (carried.get() != nullptr)
            cargoUsed += carried->GetAttribute(AttrVolume).get_float() * carried->quantity();
    }
    const double unitVolume = item->GetAttribute(AttrVolume).get_float();
    if (cargoCapacity <= cargoUsed || unitVolume < 0.0)
        return false;
    const double freeVolume = cargoCapacity - cargoUsed;
    const uint32 maxByCargo = unitVolume > 0.0
        ? static_cast<uint32>(std::min<double>(std::floor(freeVolume / unitVolume), 10000000.0))
        : item->quantity();
    if (maxByCargo == 0)
        return false;

    DBResultRow row;
    while (orders.GetRow(row)) {
        const uint32 orderID = row.GetUInt(0);
        const uint32 destinationStationID = row.GetUInt(1);
        const uint32 destinationSystemID = row.GetUInt(2);
        const uint32 remaining = row.GetUInt(3);
        const uint32 ownerID = row.GetUInt(4);
        const double price = row.GetDouble(5);
        const uint32 minimumVolume = row.GetUInt(6);
        if (orderID == 0 || destinationStationID == 0 || remaining == 0 ||
            ownerID == m_client.GetCharacterID() || price < minimumPrice)
            continue;

        StationData destination;
        if (!stDataMgr.GetStationData(destinationStationID, destination) ||
            destination.systemID != destinationSystemID)
            continue;
        if (destinationSystemID != m_client.GetSystemID()) {
            GateRoute route;
            if (!GetNextGateRoute(m_client.GetSystemID(), destinationSystemID, route))
                continue;
        }

        const uint32 quantity = std::min<uint32>(item->quantity(),
            std::min<uint32>(remaining, maxByCargo));
        if (quantity == 0 || quantity < std::min(minimumVolume, remaining))
            continue;
        InventoryItemRef cargoItem = item;
        if (quantity < item->quantity()) {
            cargoItem = item->Split(static_cast<int32>(quantity));
            if (cargoItem.get() == nullptr)
                continue;
        }
        m_client.MoveItem(cargoItem->itemID(), ship->itemID(), flagCargoHold);
        if (cargoItem->locationID() != ship->itemID() || cargoItem->flag() != flagCargoHold)
            return false;

        m_transitStationID = destinationStationID;
        SetState(State::Traveling);
        _log(MARKET__MESSAGE,
             "PlayerBot %s loaded %u %s and is travelling to station %u to fill buy order %u at %.2f ISK each.",
             m_client.GetName(), quantity, item->name(), destinationStationID, orderID, price);
        return true;
    }
    return false;
}

void BotBrain::ProcessTrader(bool allowRemoteDelivery)
{
    if (m_state == State::Returning || m_transitStationID != 0) {
        const uint32 destinationStationID = m_transitStationID != 0 ?
            m_transitStationID : m_record.homeStationID;
        if (destinationStationID == 0)
            return;
        ProcessStationTransit(destinationStationID);
        if (m_client.IsDocked()) {
            m_returnWarpStarted = false;
            SetState(State::Docked);
        }
        return;
    }

    if (!m_client.IsDocked()) {
        RequestDock();
        return;
    }
    const uint32 stationID = static_cast<uint32>(m_client.GetStationID());
    if (stationID == 0)
        return;
    if (m_record.homeStationID == 0)
        m_record.homeStationID = stationID;
    const uint64_t now = GetFileTimeNow();
    // Each attempt is deliberately spread out and stays within the one-Hz bot
    // budget. Open orders are backed by this character's wallet or inventory.
    if (m_lastMarketActionAt != 0 && now - m_lastMarketActionAt < 60ULL * 10000000ULL)
        return;
    m_lastMarketActionAt = now;

    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(stationID);
    MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
    if (station.get() == nullptr || market == nullptr)
        return;

    // Stock carried through gates remains in the ship until the pilot docks.
    // Unload it to the real hangar before matching or placing station orders.
    std::vector<InventoryItemRef> deliveredCargo;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlag(flagCargoHold, deliveredCargo);
    for (const InventoryItemRef& item : deliveredCargo) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() && IsTraderMarketStock(item))
            m_client.MoveItem(item->itemID(), stationID, flagHangar);
    }

    if (allowRemoteDelivery && m_demandOrderID != 0 && SourcePlayerDemand(stationID))
        return;

    // First settle stock against any real local, system, constellation or
    // regional buy order. When none can reach this station, physically haul a
    // small load to the best reachable station-specific buy order.
    if (SellTraderStockAtStation(stationID))
        return;

    if (allowRemoteDelivery && SourcePlayerDemand(stationID))
        return;

    // Offer owned ore, minerals and completed Miner I modules. Posting goes
    // through MarketProxyService::PlaceCharOrder so fees and item removal use
    // the same path as a connected pilot.
    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    for (const InventoryItemRef& item : hangarItems) {
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->locationID() != stationID || !IsTraderMarketStock(item) ||
            MarketDB::FindOwnerOrder(m_client.GetCharacterID(), stationID,
                                     item->typeID(), false) != 0)
            continue;

        if (allowRemoteDelivery && StartRemoteMarketDelivery(stationID, item->itemID()))
            return;

        if (m_client.GetSystemID() == 30000142)
            continue;

        const ItemType* itemType = sItemFactory.GetType(item->typeID());
        if (itemType == nullptr)
            continue;

        const uint32 buyID = MarketDB::FindBuyOrder(item->typeID(), stationID, 1, 0.0,
                                                    m_client.GetCharacterID());
        const uint32 sellID = MarketDB::FindSellOrder(item->typeID(), stationID, 1, 1.0e12);
        Market::OrderInfo order;
        double bestBid = buyID != 0 && MarketDB::GetOrderInfo(buyID, order) ? order.price : 0.0;
        double bestAsk = sellID != 0 && MarketDB::GetOrderInfo(sellID, order) ? order.price : 0.0;
        const double basePrice = itemType->basePrice();

        double askPrice = basePrice > 0.0 ? basePrice * 1.10 : 0.0;
        if (bestBid > 0.0)
            askPrice = std::max(askPrice, bestBid * 1.01);
        if (bestAsk > 0.0 && (bestBid <= 0.0 || bestAsk * 0.99 > bestBid))
            askPrice = askPrice > 0.0 ? std::min(askPrice, bestAsk * 0.99) : bestAsk * 0.99;
        if (bestBid > 0.0)
            askPrice = std::max(askPrice, bestBid * 1.01);
        askPrice = std::round(askPrice * 100.0) / 100.0;
        if (askPrice <= 0.0)
            return;

        const double headroom = sConfig.playerbots.maxISK - m_client.GetBalance();
        const uint32 maxByWealth = headroom > 0.0
            ? static_cast<uint32>(std::min<double>(std::floor(headroom / askPrice), 10000.0)) : 0;
        const uint32 quantity = std::min<uint32>(item->quantity(), maxByWealth);
        if (quantity != 0 && market->PlaceBotLimitOrder(m_client, stationID,
                item->typeID(), askPrice, quantity, false, item->itemID(), 7))
            _log(MARKET__MESSAGE, "PlayerBot %s placed a real sell order for %u %s at %.2f ISK.",
                 m_client.GetName(), quantity, item->name(), askPrice);
        return;
    }

    // Rotate Trader pilots among common highsec ores and the T1 Miner made by
    // industrialists. These are genuine character orders with limited escrow;
    // miners and industrialists can settle into them, then traders relist the
    // acquired inventory through the normal market service.
    static const std::array<uint32, 8> marketTypes = {{1230, 1228, 1224, 34, 35, 36, 18, 483}};
    const uint32 ordinal = (m_client.GetCharacterID() >= 90000001U)
        ? (m_client.GetCharacterID() - 90000001U) / 6U : m_client.GetCharacterID();
    const uint32 typeID = marketTypes[ordinal % marketTypes.size()];
    if (MarketDB::FindOwnerOrder(m_client.GetCharacterID(), stationID, typeID, false) != 0 ||
        MarketDB::FindOwnerOrder(m_client.GetCharacterID(), stationID, typeID, true) != 0)
        return;

    const ItemType* type = sItemFactory.GetType(typeID);
    if (type == nullptr || m_client.GetBalance() >= sConfig.playerbots.maxISK)
        return;

    const uint32 buyID = MarketDB::FindBuyOrder(typeID, stationID, 1, 0.0,
                                                m_client.GetCharacterID());
    const uint32 sellID = MarketDB::FindSellOrder(typeID, stationID, 1, 1.0e12);
    Market::OrderInfo order;
    const double bestBid = buyID != 0 && MarketDB::GetOrderInfo(buyID, order) ? order.price : 0.0;
    const double bestAsk = sellID != 0 && MarketDB::GetOrderInfo(sellID, order) ? order.price : 0.0;
    const double basePrice = type->basePrice();

    double bidPrice = basePrice > 0.0 ? basePrice * 0.80 : 0.0;
    if (bestBid > 0.0)
        bidPrice = std::max(bidPrice, bestBid * 1.01);
    if (bestAsk > 0.0 && (bidPrice <= 0.0 || bestAsk * 0.90 < bidPrice))
        bidPrice = bestAsk * 0.90;
    if (basePrice > 0.0)
        bidPrice = std::min(bidPrice, basePrice * 1.25);
    // Product bids pay enough to let a real one-run Miner I job recover its
    // local mineral cost and assembly-line fee while staying below the seeded
    // Forge NPC sell order. Ore bids retain the more conservative spread.
    if (typeID == 483 && bestAsk > 0.0 && basePrice > 0.0)
        bidPrice = std::min(std::max(bidPrice, bestAsk * 0.95), basePrice * 1.25);
    bidPrice = std::round(bidPrice * 100.0) / 100.0;
    if (bidPrice <= 0.0)
        return;

    // Reserve at most 5% of the wallet in a single order and leave room for
    // the broker fee charged by the regular market service.
    const double orderBudget = std::min(m_client.GetBalance() * 0.05, 1000000.0);
    uint32 quantity = static_cast<uint32>(std::floor(orderBudget / (bidPrice * 1.10)));
    quantity = std::min<uint32>(quantity, 10000);
    if (quantity == 0)
        return;

    if (market->PlaceBotLimitOrder(m_client, stationID, typeID,
                                   bidPrice, quantity, true, 0, 7))
        _log(MARKET__MESSAGE, "PlayerBot %s placed a real buy order for %u %s at %.2f ISK.",
             m_client.GetName(), quantity, type->name(), bidPrice);
}

bool BotBrain::ProcessActiveIndustryJob()
{
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT job.jobID, job.endProductionTime "
        "FROM ramJobs job "
        "JOIN ramAssemblyLines line USING (assemblyLineID) "
        "WHERE job.installerID=%u AND job.completedStatusID=0 AND line.activityID=%u "
        "ORDER BY job.installTime LIMIT 1",
        m_client.GetCharacterID(), EvERam::Activity::Manufacturing)) {
        sLog.Warning("PlayerBot", "%s could not inspect its real manufacturing jobs: %s.",
                     m_client.GetName(), result.error.c_str());
        return true;
    }

    DBResultRow row;
    if (!result.GetRow(row))
        return false;

    const uint32 jobID = row.GetUInt(0);
    const int64 endTime = row.GetInt64(1);
    if (GetFileTimeNow() < endTime)
        return true;

    RamProxyService* ram = m_client.services().Lookup<RamProxyService>("ramProxy");
    if (ram == nullptr || m_client.GetStationID() == 0)
        return true;

    Call_CompleteJob args;
    args.invLocationID = m_client.GetStationID();
    args.invLocationGroupID = EVEDB::invGroups::Station;
    args.path = new PyList();
    args.containerID = m_client.GetStationID();
    args.jobID = jobID;
    args.cancel = false;
    ram->ExecutePlayerBotCompleteJob(m_client, args);
    sLog.Green("PlayerBot", "%s delivered completed manufacturing job %u through ramProxy.",
               m_client.GetName(), jobID);
    return true;
}

bool BotBrain::EnsureIndustryBlueprint(uint32 stationID, uint16 blueprintTypeID, uint32& blueprintItemID)
{
    blueprintItemID = 0;
    StationItemRef station = m_client.SystemMgr() == nullptr
        ? StationItemRef(nullptr) : m_client.SystemMgr()->GetStationFromInventory(stationID);
    if (station.get() == nullptr)
        return false;

    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    for (const InventoryItemRef& item : hangarItems) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            item->locationID() == stationID && item->typeID() == blueprintTypeID &&
            item->categoryID() == EVEDB::invCategories::Blueprint) {
            blueprintItemID = item->itemID();
            return true;
        }
    }

    // NPC blueprint sales are real market orders. Buying through the normal
    // settlement path creates the persistent BPO and charges its true price.
    const uint32 orderID = MarketDB::FindSellOrder(blueprintTypeID, stationID, 1, 1.0e12);
    Market::OrderInfo order;
    if (orderID == 0 || !MarketDB::GetOrderInfo(orderID, order) ||
        order.ownerID == m_client.GetCharacterID() || order.price <= 0.0 ||
        m_client.GetBalance() < order.price)
        return false;

    sMktMgr.ExecuteSellOrder(&m_client, orderID, 1, order.price, stationID,
                             blueprintTypeID, false);
    const ItemType* blueprintType = sItemFactory.GetType(blueprintTypeID);
    if (blueprintType != nullptr)
        _log(MARKET__MESSAGE, "PlayerBot %s bought %s from market order %u for %.2f ISK.",
             m_client.GetName(), blueprintType->name(), orderID, order.price);
    return false;
}

bool BotBrain::BuyIndustryMaterial(uint32 stationID, uint16 typeID, uint32 quantity)
{
    if (quantity == 0)
        return true;

    StationItemRef station = m_client.SystemMgr() == nullptr
        ? StationItemRef(nullptr) : m_client.SystemMgr()->GetStationFromInventory(stationID);
    if (station.get() == nullptr)
        return false;
    // Buy the best available public stack even when it is smaller than the
    // exact shortage returned by the real manufacturing quote. Repeated real
    // settlements let production use multiple existing orders instead of
    // waiting forever on a single large lot.
    const uint32 orderID = MarketDB::FindSellOrder(typeID, stationID, 1, 1.0e12);
    Market::OrderInfo order;
    if (orderID == 0 || !MarketDB::GetOrderInfo(orderID, order) ||
        order.ownerID == m_client.GetCharacterID() || order.price <= 0.0) {
        if (m_client.GetSystemID() == 30000142 ||
            MarketDB::FindOwnerOrder(m_client.GetCharacterID(), stationID, typeID, true) != 0)
            return false;
        const ItemType* material = sItemFactory.GetType(typeID);
        MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
        if (material == nullptr || market == nullptr)
            return false;
        // Pay enough for an independent trader to buy and haul the material
        // from the seed hub, while retaining wallet-backed escrow and fees.
        DBQueryResult jita;
        double reference = material->basePrice();
        if (sDatabase.RunQuery(jita,
            "SELECT MIN(price) FROM mktOrders WHERE stationID=60003760 AND typeID=%u "
            "AND bid=0 AND volRemaining>0 AND price>0", typeID)) {
            DBResultRow row;
            if (jita.GetRow(row) && !row.IsNull(0) && row.GetDouble(0) > 0.0)
                reference = std::max(reference, row.GetDouble(0));
        }
        const double bidPrice = std::round(reference * 115.0) / 100.0;
        if (bidPrice <= 0.0)
            return false;
        const uint32 affordable = static_cast<uint32>(std::min<double>(10000.0,
            std::floor((m_client.GetBalance() * 0.15) / (bidPrice * 1.10))));
        const uint32 bidQuantity = std::min<uint32>(quantity, affordable);
        if (bidQuantity && market->PlaceBotLimitOrder(m_client, stationID, typeID,
                                                       bidPrice, bidQuantity, true, 0, 7))
            _log(MARKET__MESSAGE, "PlayerBot %s placed a backed buy order for %u %s at station %u.",
                 m_client.GetName(), bidQuantity, material->name(), stationID);
        return false;
    }

    uint32 purchaseQuantity = std::min<uint32>(quantity, order.quantity);
    const uint32 affordable = static_cast<uint32>(std::min<double>(
        std::floor(m_client.GetBalance() / order.price), 10000000.0));
    purchaseQuantity = std::min<uint32>(purchaseQuantity, affordable);
    if (purchaseQuantity == 0)
        return false;

    sMktMgr.ExecuteSellOrder(&m_client, orderID, purchaseQuantity, order.price,
                             stationID, typeID, false);
    const ItemType* material = sItemFactory.GetType(typeID);
    if (material != nullptr)
        _log(MARKET__MESSAGE, "PlayerBot %s bought %u %s for manufacturing from order %u at %.2f ISK each.",
             m_client.GetName(), purchaseQuantity, material->name(), orderID, order.price);
    return false;
}

bool BotBrain::PlaceIndustrySellOrder(uint32 stationID, uint16 productTypeID)
{
    if (m_client.GetSystemID() == 30000142)
        return false;
    if (MarketDB::FindOwnerOrder(m_client.GetCharacterID(), stationID, productTypeID, false) != 0)
        return true;

    StationItemRef station = m_client.SystemMgr() == nullptr
        ? StationItemRef(nullptr) : m_client.SystemMgr()->GetStationFromInventory(stationID);
    MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
    if (station.get() == nullptr || market == nullptr)
        return false;

    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    InventoryItemRef product(nullptr);
    for (const InventoryItemRef& item : hangarItems) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            item->locationID() == stationID && item->typeID() == productTypeID && item->quantity() != 0) {
            product = item;
            break;
        }
    }
    if (product.get() == nullptr)
        return false;

    const uint32 buyOrderID = MarketDB::FindBuyOrder(productTypeID, stationID, 1, 0.0,
                                                      m_client.GetCharacterID());
    Market::OrderInfo buyOrder;
    if (buyOrderID != 0 && MarketDB::GetOrderInfo(buyOrderID, buyOrder) &&
        buyOrder.ownerID != m_client.GetCharacterID() && buyOrder.price > 0.0) {
        const double headroom = sConfig.playerbots.maxISK - m_client.GetBalance();
        const uint32 maxByWealth = headroom > 0.0
            ? static_cast<uint32>(std::min<double>(std::floor(headroom / buyOrder.price), 10000000.0)) : 0;
        const uint32 quantity = std::min<uint32>(product->quantity(),
            std::min<uint32>(buyOrder.quantity, maxByWealth));
        if (quantity != 0 && CanFillMinimumVolume(buyOrderID, quantity, buyOrder.quantity)) {
            InventoryItemRef saleItem = product;
            if (quantity < product->quantity())
                saleItem = product->Split(static_cast<int32>(quantity));
            if (saleItem.get() != nullptr) {
                const std::string productName = product->name();
                if (sMktMgr.ExecuteBuyOrder(&m_client, buyOrderID, saleItem, quantity, false,
                                            productTypeID, stationID, buyOrder.price)) {
                    _log(MARKET__MESSAGE, "PlayerBot %s sold %u manufactured %s to buy order %u for %.2f ISK each.",
                         m_client.GetName(), quantity, productName.c_str(), buyOrderID, buyOrder.price);
                    return true;
                }
            }
        }
    }

    const ItemType* type = sItemFactory.GetType(productTypeID);
    if (type == nullptr)
        return true;
    const uint32 sellID = MarketDB::FindSellOrder(productTypeID, stationID, 1, 1.0e12);
    Market::OrderInfo sellOrder;
    const double bestAsk = sellID != 0 && MarketDB::GetOrderInfo(sellID, sellOrder)
        ? sellOrder.price : 0.0;
    const double basePrice = type->basePrice();
    double askPrice = basePrice > 0.0 ? basePrice * 1.10 : 0.0;
    if (bestAsk > 0.0)
        askPrice = std::min(askPrice, bestAsk * 0.99);
    askPrice = std::max(askPrice, basePrice * 0.75);
    askPrice = std::round(askPrice * 100.0) / 100.0;
    if (askPrice <= 0.0)
        return true;

    if (market->PlaceBotLimitOrder(m_client, stationID, productTypeID, askPrice,
                                   product->quantity(), false, product->itemID(), 7))
        _log(MARKET__MESSAGE, "PlayerBot %s listed manufactured %s x%u for %.2f ISK each.",
             m_client.GetName(), product->name(), product->quantity(), askPrice);
    return true;
}

void BotBrain::ProcessIndustrialist()
{
    if (!m_client.IsDocked()) {
        RequestDock();
        return;
    }
    const uint32 stationID = static_cast<uint32>(m_client.GetStationID());
    if (stationID == 0)
        return;
    if (m_record.homeStationID == 0)
        m_record.homeStationID = stationID;

    std::vector<InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlag(flagCargoHold, cargo);
    for (const InventoryItemRef& item : cargo) {
        if (item.get() != nullptr && item->ownerID() == m_client.GetCharacterID() &&
            (item->categoryID() == EVEDB::invCategories::Blueprint || IsTraderMarketStock(item)))
            m_client.MoveItem(item->itemID(), stationID, flagHangar);
    }

    const uint64_t now = GetFileTimeNow();
    if (now - m_stateSince < 60ULL * 10000000ULL)
        return;
    m_stateSince = now;

    // One real job or market action per interval keeps the bot tick bounded.
    if (ProcessActiveIndustryJob())
        return;
    IndustryProduct product = IndustryProductForSlot(m_record.slot);
    if (product.blueprint != 785 && m_client.SystemMgr() != nullptr) {
        StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(stationID);
        bool ownsPreferred = false, ownsMinerBlueprint = false;
        if (station.get() != nullptr) {
            std::vector<InventoryItemRef> hangar;
            station->GetMyInventory()->GetItemsByFlag(flagHangar, hangar);
            for (const InventoryItemRef& item : hangar) {
                if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
                    item->locationID() != stationID || item->categoryID() != EVEDB::invCategories::Blueprint)
                    continue;
                ownsPreferred = ownsPreferred || item->typeID() == product.blueprint;
                ownsMinerBlueprint = ownsMinerBlueprint || item->typeID() == 785;
            }
        }
        if (!ownsPreferred && ownsMinerBlueprint)
            product = {785, 483};
    }
    uint32 blueprintItemID = 0;
    if (!EnsureIndustryBlueprint(stationID, product.blueprint, blueprintItemID)) {
        ProcessTrader(false);
        return;
    }
    if (PlaceIndustrySellOrder(stationID, product.product))
        return;

    BlueprintRef blueprint = sItemFactory.GetBlueprintRef(blueprintItemID);
    if (blueprint.get() == nullptr || blueprint->ownerID() != m_client.GetCharacterID() ||
        blueprint->locationID() != stationID || blueprint->typeID() != product.blueprint)
        return;

    DBQueryResult lines;
    if (!sDatabase.RunQuery(lines,
        "SELECT assemblyLineID FROM ramAssemblyLines "
        "WHERE containerID=%u AND activityID=%u "
        "ORDER BY nextFreeTime, assemblyLineID LIMIT 16",
        stationID, EvERam::Activity::Manufacturing))
        return;
    uint32 assemblyLineID = 0;
    DBResultRow lineRow;
    while (lines.GetRow(lineRow)) {
        const uint32 candidate = lineRow.GetUInt(0);
        if (FactoryDB::IsProducableBy(candidate, &blueprint->productType())) {
            assemblyLineID = candidate;
            break;
        }
    }
    if (assemblyLineID == 0) {
        ProcessTrader(false);
        return;
    }

    PathElement itemPath;
    itemPath.locationID = stationID;
    itemPath.ownerID = m_client.GetCharacterID();
    itemPath.flagID = flagHangar;
    Call_InstallJob args;
    args.lineLocationID = stationID;
    args.lineLocationGroupID = EVEDB::invGroups::Station;
    args.lineContainerID = stationID;
    args.AssemblyLineID = assemblyLineID;
    args.bpLocationID = stationID;
    args.bpLocationGroupID = EVEDB::invGroups::Station;
    args.bpLocPath = new PyList(1);
    args.bpLocPath->items[0] = itemPath.Encode();
    args.bpItemID = blueprintItemID;
    args.bomLocationID = stationID;
    args.bomLocationGroupID = EVEDB::invGroups::Station;
    args.bomLocPath = new PyList(1);
    args.bomLocPath->items[0] = itemPath.Encode();
    args.outputFlag = flagHangar;
    args.runs = 1;
    args.activityID = EvERam::Activity::Manufacturing;
    args.copyRuns = 0;
    args.isCorpJob = false;
    args.description = "PlayerBot T1 production";

    RamProxyService* ram = m_client.services().Lookup<RamProxyService>("ramProxy");
    if (ram == nullptr)
        return;
    PyResult quoteResult = ram->ExecutePlayerBotInstallJob(m_client, args, true);
    Rsp_InstallJob quote;
    if (quoteResult.ssResult == nullptr || !quote.Decode(quoteResult.ssResult))
        return;

    bool materialsReady = true;
    // The quote's missingMaterials map includes skill-adjusted waste and
    // subtracts every owned stack. Buying the rawMaterials line alone can
    // leave a character short by its manufacturing waste multiplier.
    for (const auto& missing : quote.missingMaterials) {
        if (missing.first <= 0 || sDataMgr.IsSkillTypeID(static_cast<uint16>(missing.first)))
            continue;
        const uint32 quantity = PyRep::IntegerValueU32(missing.second);
        if (quantity == 0)
            continue;
        if (!BuyIndustryMaterial(stationID, static_cast<uint16>(missing.first), quantity)) {
            materialsReady = false;
            break;
        }
    }
    if (!materialsReady) {
        ProcessTrader(false);
        return;
    }

    if (m_client.GetBalance() < quote.cost) {
        ProcessTrader(false);
        return;
    }
    ram->ExecutePlayerBotInstallJob(m_client, args, false, quote.cost);
    sLog.Green("PlayerBot", "%s installed real manufacturing job for %s on assembly line %u.",
               m_client.GetName(), blueprint->productType().name(), assemblyLineID);
}

void BotBrain::ProcessMiner()
{
    switch (m_state) {
        case State::Docked:
        {
            if (m_record.homeStationID == 0)
                m_record.homeStationID = m_client.GetStationID();

            // A restart can leave already-mined ore inside a docked rookie
            // ship. Unload and process that real inventory before boarding a
            // profession hull, otherwise changing ships strands its cargo in
            // the old hull's hangar entry.
            const ShipItemRef dockedShip = m_client.GetShip();
            if (dockedShip.get() == nullptr || dockedShip->GetMyInventory() == nullptr)
                return;
            std::multimap<uint8, InventoryItemRef> cargo;
            dockedShip->GetMyInventory()->GetCargoList(cargo);
            for (const auto& entry : cargo) {
                if (entry.second.get() != nullptr &&
                    entry.second->categoryID() == EVEDB::invCategories::Asteroid) {
                    SetState(State::Selling);
                    return;
                }
            }

            // Some accounts are already docked at their home station when the
            // virtual login finishes, so the initial home-transit path never
            // fits their role hull. Ensure the same real loadout here before
            // sending miners and haulers back into space.
            if (EnsureProfessionShip())
                return;
            if (!IsMiningShipReady())
                return;
            m_client.UndockFromStation();
            SetState(State::Undocking);
            break;
        }
        case State::Undocking:
            if (m_client.IsInSpace() && m_client.GetShipSE() != nullptr)
                SetState(State::WarpToBelt);
            break;
        case State::Traveling:
        case State::WarpToBelt:
            if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr)
                return;
            if (m_transitStationID != 0) {
                ProcessStationTransit(m_transitStationID);
                if (m_client.IsDocked())
                    SetState(State::Docked);
                return;
            }
            if (ProcessBeltSystemTransit())
                return;
            if (!m_client.GetShipSE()->DestinyMgr()->IsWarpingOrAligning())
                WarpToBelt();
            break;
        case State::Mining:
            if (m_client.GetShipSE() == nullptr)
                return;
            if (m_client.GetShipSE()->DestinyMgr()->IsWarpingOrAligning())
                return;
            if (m_targetID == 0 && !StartMining() &&
                GetFileTimeNow() - m_stateSince >= 15ULL * 10000000ULL && SelectAlternateBelt())
                return;
            if (GetFileTimeNow() - m_stateSince >= static_cast<uint64_t>(sConfig.playerbots.minerCycleSeconds) * 10000000ULL) {
                StopMining();
                SetState(State::Returning);
            }
            break;
        case State::Returning:
            if (m_record.homeStationID == 0)
                return;
            ProcessStationTransit(m_record.homeStationID);
            if (m_client.IsDocked())
                SetState(State::Selling);
            break;
        case State::Selling:
            if (!m_client.IsDocked()) {
                if (m_record.homeStationID != 0)
                    ProcessStationTransit(m_record.homeStationID);
                return;
            }
            SellOre();
            SetState(State::Docked);
            break;
        default:
            break;
    }
}

bool BotBrain::WarpToBelt()
{
    if (m_client.SystemMgr()->BeltCount() == 0)
        return false;
    if (m_record.currentBeltID == 0)
        m_record.currentBeltID = m_client.SystemMgr()->GetRandBeltID();
    SystemEntity* belt = m_client.SystemMgr()->FindEntityByID(m_record.currentBeltID);
    if (belt == nullptr) {
        m_record.currentBeltID = 0;
        return false;
    }
    m_client.GetShipSE()->DestinyMgr()->WarpTo(belt->GetPosition(), 15000);
    SetState(State::Mining);
    return true;
}

bool BotBrain::ProcessBeltSystemTransit()
{
    SystemManager* system = m_client.SystemMgr();
    ShipSE* ship = m_client.GetShipSE();
    if (system == nullptr || ship == nullptr)
        return true;

    // Use a belt already present in this grid when possible. Forge's Jita
    // system intentionally has no belts, so pilots have to select a reachable
    // highsec mining system instead of silently idling here.
    if (system->BeltCount() != 0) {
        if (m_record.currentBeltID != 0 && system->FindEntityByID(m_record.currentBeltID) != nullptr)
            return false;
        const GateRouteGraph& knownBelts = GetGateRouteGraph();
        if (m_record.currentBeltID == 0 ||
            knownBelts.highsecSystemByBelt.find(m_record.currentBeltID) == knownBelts.highsecSystemByBelt.end()) {
            m_record.currentBeltID = system->GetRandBeltID();
            if (m_record.currentBeltID != 0)
                return false;
        }
    }

    GateRouteGraph& graph = GetGateRouteGraph();
    uint32 targetSystem = 0;
    if (m_record.currentBeltID != 0) {
        const auto beltSystem = graph.highsecSystemByBelt.find(m_record.currentBeltID);
        if (beltSystem != graph.highsecSystemByBelt.end())
            targetSystem = beltSystem->second;
        else
            m_record.currentBeltID = 0;
    }

    if (targetSystem == 0) {
        uint32 targetBelt = 0;
        if (!FindNearestHighsecBelt(m_client.GetSystemID(), m_record.slot, targetSystem, targetBelt)) {
            const uint64_t now = GetFileTimeNow();
            if (m_record.slot != 0 && ((m_record.slot - 1) % 15) == 0 &&
                (m_lastTransitDiagAt == 0 || now - m_lastTransitDiagAt >= 600000000ULL)) {
                m_lastTransitDiagAt = now;
                sLog.Warning("PlayerBot", "%s has no reachable highsec asteroid belt from system %u.",
                             m_client.GetName(), m_client.GetSystemID());
            }
            return true;
        }
        m_record.currentBeltID = targetBelt;
        sLog.Green("PlayerBot", "%s selected highsec mining system %s (%u), belt %u.",
                   m_client.GetName(), sDataMgr.GetSystemName(targetSystem),
                   targetSystem, targetBelt);
    }

    if (targetSystem == m_client.GetSystemID()) {
        if (system->BeltCount() != 0) {
            m_record.currentBeltID = system->GetRandBeltID();
            return m_record.currentBeltID == 0;
        }
        return true;
    }

    if (m_client.IsDocked()) {
        if (!m_client.IsIdle() || m_client.IsInvul())
            return true;
        m_client.UndockFromStation();
        SetState(State::Undocking);
        return true;
    }
    if (m_client.IsLoginWarping() || m_client.IsJump() || m_client.IsDock() || m_client.IsUndock() ||
        m_client.IsSessionChange() || m_client.IsBubbleWait() || !m_client.IsIdle() || m_client.IsInvul())
        return true;

    DestinyManager* destiny = ship->DestinyMgr();
    if (destiny == nullptr || destiny->IsWarpingOrAligning())
        return true;

    GateRoute route;
    if (!GetNextGateRoute(m_client.GetSystemID(), targetSystem, route, true))
        return true;
    SystemEntity* gate = system->FindEntityByID(route.fromGate);
    if (gate == nullptr || gate->GetGateSE() == nullptr)
        return true;

    if (ship->GetPosition().distance(gate->GetPosition()) > 2500.0) {
        destiny->WarpTo(gate->GetPosition(), 0);
        SetState(State::Traveling);
        return true;
    }

    m_client.StargateJump(route.fromGate, route.toGate);
    return true;
}

bool BotBrain::SelectAlternateBelt()
{
    uint32 targetSystem = 0;
    uint32 targetBelt = 0;
    if (!FindNearestHighsecBelt(m_client.GetSystemID(), m_record.slot,
                                targetSystem, targetBelt))
        return false;
    m_record.currentBeltID = targetBelt;
    m_targetID = 0;
    SetState(State::WarpToBelt);
    sLog.Green("PlayerBot", "%s is moving to another highsec belt in %s (%u).",
               m_client.GetName(), sDataMgr.GetSystemName(targetSystem), targetSystem);
    return true;
}

bool BotBrain::StartMining()
{
    std::vector<AsteroidSE*> asteroids;
    m_client.SystemMgr()->GetBeltMgr()->GetList(m_record.currentBeltID, asteroids);
    if (asteroids.empty())
        return false;

    AsteroidSE* asteroid = nullptr;
    for (AsteroidSE* candidate : asteroids) {
        if (candidate != nullptr &&
            (m_requestedAsteroidTypeID == 0 || candidate->GetTypeID() == m_requestedAsteroidTypeID)) {
            asteroid = candidate;
            break;
        }
    }
    if (asteroid == nullptr)
        return false;

    m_targetID = asteroid->GetID();
    m_client.GetShipSE()->DestinyMgr()->Orbit(asteroid, 3000);

    std::vector<InventoryItemRef> modules;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlagRange(flagHiSlot0, flagHiSlot7, modules);
    for (const InventoryItemRef& module : modules) {
        if (module->groupID() == EVEDB::invGroups::Mining_Laser || module->groupID() == EVEDB::invGroups::Strip_Miner)
            m_client.GetShip()->GetModuleManager()->Activate(module->itemID(), EVEEffectID::miningLaser, m_targetID, 1000);
    }
    return true;
}

void BotBrain::StopMining()
{
    if (m_client.GetShip().get() != nullptr)
        m_client.GetShip()->AbortCycle();
    m_targetID = 0;
}

void BotBrain::SellOre()
{
    const uint32 stationID = m_record.homeStationID;
    if (stationID == 0 || !m_client.IsDocked())
        return;

    // Keep half of the mining cohorts in the raw-ore trade and send the other
    // half through the actual station refinery. That gives industry bots a
    // renewable mineral supply after the bounded NPC market seed is consumed.
    const bool refineOre = m_record.slot != 0 && (((m_record.slot - 1) / 6) % 2 == 0);
    std::multimap<uint8, InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetCargoList(cargo);
    for (const auto& entry : cargo) {
        InventoryItemRef ore = entry.second;
        if (ore.get() == nullptr || ore->categoryID() != EVEDB::invCategories::Asteroid)
            continue;
        m_client.MoveItem(ore->itemID(), stationID, flagHangar);
    }

    StationItemRef station = m_client.SystemMgr() == nullptr
        ? StationItemRef(nullptr) : m_client.SystemMgr()->GetStationFromInventory(stationID);
    if (station.get() == nullptr)
        return;

    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    std::vector<uint32> oreToRefine;
    for (const InventoryItemRef& ore : hangarItems) {
        if (ore.get() == nullptr || ore->ownerID() != m_client.GetCharacterID() ||
            ore->locationID() != stationID || ore->categoryID() != EVEDB::invCategories::Asteroid)
            continue;
        if (refineOre) {
            if (ore->quantity() < ore->type().portionSize())
                continue;
            oreToRefine.push_back(ore->itemID());
            continue;
        }

        if (m_client.GetSystemID() == 30000142)
            continue;

        const double wealthHeadroom = sConfig.playerbots.maxISK - m_client.GetBalance();
        if (wealthHeadroom <= 0.0)
            continue;
        const uint32 orderID = MarketDB::FindBuyOrder(ore->typeID(), stationID, 1, 0.0,
                                                      m_client.GetCharacterID());
        Market::OrderInfo order;
        if (orderID == 0 || !MarketDB::GetOrderInfo(orderID, order) ||
            order.ownerID == m_client.GetCharacterID() || order.price <= 0.0)
            continue;

        // Market fills are bounded by both the public order and the bot's
        // configured wallet ceiling. Split the stack before settlement because
        // ExecuteBuyOrder transfers the full item reference it receives.
        const uint32 maxByOrder = order.quantity;
        const uint32 maxByWealth = static_cast<uint32>(std::min<double>(
            std::floor(wealthHeadroom / order.price), 10000000.0));
        const uint32 quantity = std::min<uint32>(ore->quantity(), std::min(maxByOrder, maxByWealth));
        if (quantity == 0 || !CanFillMinimumVolume(orderID, quantity, order.quantity))
            continue;

        InventoryItemRef saleItem = ore;
        if (quantity < ore->quantity()) {
            saleItem = ore->Split(quantity);
            if (saleItem.get() == nullptr)
                continue;
        }
        sMktMgr.ExecuteBuyOrder(&m_client, orderID, saleItem, quantity, false,
                                ore->typeID(), stationID, order.price);
    }

    if (oreToRefine.empty())
        return;
    ReprocessingService* reprocessing = m_client.services().Lookup<ReprocessingService>("reprocessingSvc");
    if (reprocessing != nullptr && reprocessing->ExecutePlayerBotReprocess(m_client, stationID, oreToRefine)) {
        sLog.Green("PlayerBot", "%s reprocessed %zu real ore stack(s) at station %u.",
                   m_client.GetName(), oreToRefine.size(), stationID);
        PlaceRefinedMineralOrders(stationID);
    }
}

void BotBrain::PlaceRefinedMineralOrders(uint32 stationID)
{
    if (m_client.GetSystemID() == 30000142)
        return;
    StationItemRef station = m_client.SystemMgr() == nullptr
        ? StationItemRef(nullptr) : m_client.SystemMgr()->GetStationFromInventory(stationID);
    MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
    if (station.get() == nullptr || market == nullptr)
        return;

    static const std::array<uint16, 3> mineralTypes = {{34, 35, 36}};
    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    for (uint16 mineralTypeID : mineralTypes) {
        if (MarketDB::FindOwnerOrder(m_client.GetCharacterID(), stationID, mineralTypeID, false) != 0)
            continue;
        for (const InventoryItemRef& mineral : hangarItems) {
            if (mineral.get() == nullptr || mineral->ownerID() != m_client.GetCharacterID() ||
                mineral->locationID() != stationID || mineral->typeID() != mineralTypeID ||
                mineral->quantity() == 0)
                continue;

            const double wealthHeadroom = sConfig.playerbots.maxISK - m_client.GetBalance();
            if (wealthHeadroom <= 0.0)
                break;
            const ItemType* mineralInfo = sItemFactory.GetType(mineralTypeID);
            if (mineralInfo == nullptr)
                break;
            const uint32 bidID = MarketDB::FindBuyOrder(mineralTypeID, stationID, 1, 0.0,
                                                        m_client.GetCharacterID());
            const uint32 askID = MarketDB::FindSellOrder(mineralTypeID, stationID, 1, 1.0e12);
            Market::OrderInfo buyOrder;
            Market::OrderInfo sellOrder;
            const bool haveBuyOrder = bidID != 0 && MarketDB::GetOrderInfo(bidID, buyOrder);
            const bool haveSellOrder = askID != 0 && MarketDB::GetOrderInfo(askID, sellOrder);
            const double bestBid = haveBuyOrder ? buyOrder.price : 0.0;
            const double bestAsk = haveSellOrder ? sellOrder.price : 0.0;
            const double basePrice = mineralInfo->basePrice();
            double askPrice = basePrice > 0.0 ? basePrice * 1.10 : 0.0;
            if (bestAsk > 0.0)
                askPrice = std::min(askPrice, bestAsk * 0.99);
            if (bestBid > 0.0)
                askPrice = std::max(askPrice, bestBid * 1.01);
            if (basePrice > 0.0)
                askPrice = std::max(askPrice, basePrice * 0.75);
            askPrice = std::round(askPrice * 100.0) / 100.0;
            if (askPrice <= 0.0)
                break;

            // Fill an existing backed bid directly when it offers a sensible
            // price. This lets regional human buy orders consume actual
            // reprocessed minerals instead of leaving those stacks idle
            // behind a new sell order.
            if (haveBuyOrder && buyOrder.ownerID != m_client.GetCharacterID() &&
                bestBid >= basePrice * 0.75) {
                const double saleHeadroom = sConfig.playerbots.maxISK - m_client.GetBalance();
                const uint32 maxByWealth = saleHeadroom > 0.0
                    ? static_cast<uint32>(std::min<double>(std::floor(saleHeadroom / bestBid), 10000000.0))
                    : 0;
                const uint32 saleQuantity = std::min<uint32>(mineral->quantity(),
                    std::min<uint32>(buyOrder.quantity, maxByWealth));
                if (saleQuantity != 0 && CanFillMinimumVolume(bidID, saleQuantity, buyOrder.quantity)) {
                    InventoryItemRef saleItem = mineral;
                    if (saleQuantity < mineral->quantity())
                        saleItem = mineral->Split(static_cast<int32>(saleQuantity));
                    const std::string mineralName = mineral->name();
                    if (saleItem.get() != nullptr &&
                        sMktMgr.ExecuteBuyOrder(&m_client, bidID, saleItem, saleQuantity, false,
                                                mineralTypeID, stationID, bestBid)) {
                        _log(MARKET__MESSAGE,
                             "PlayerBot %s sold %u refined %s into buy order %u for %.2f ISK each.",
                             m_client.GetName(), saleQuantity, mineralName.c_str(), bidID, bestBid);
                        break;
                    }
                }
            }
            const uint32 affordable = static_cast<uint32>(std::min<double>(
                std::floor(wealthHeadroom / askPrice), 10000000.0));
            const uint32 quantity = std::min<uint32>(mineral->quantity(), affordable);
            if (quantity != 0 && market->PlaceBotLimitOrder(
                m_client, stationID, mineralTypeID, askPrice, quantity,
                false, mineral->itemID(), 7)) {
                _log(MARKET__MESSAGE, "PlayerBot %s listed refined %s x%u at %.2f ISK each.",
                     m_client.GetName(), mineral->name(), quantity, askPrice);
            }
            break;
        }
    }
}

void BotBrain::ProcessAttack()
{
    if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr || m_targetID == 0)
        return;

    SystemEntity* target = m_client.SystemMgr()->FindEntityByID(m_targetID);
    if (target == nullptr || target == m_client.GetShipSE()) {
        m_targetID = 0;
        SetState(State::Manual);
        return;
    }

    TargetManager* targets = m_client.GetShipSE()->TargetMgr();
    if (targets == nullptr)
        return;
    if (targets->GetTarget(m_targetID, false) == nullptr) {
        // Same player path: range/lock-time checks, notifications and target
        // state are performed by TargetManager rather than by a bot shortcut.
        targets->StartTargeting(target, m_client.GetShip());
        m_client.GetShipSE()->DestinyMgr()->Orbit(target, 5000);
        return;
    }
    if (targets->GetTarget(m_targetID, true) == nullptr)
        return;

    m_client.GetShipSE()->DestinyMgr()->Orbit(target, 5000);
    std::vector<InventoryItemRef> modules;
    m_client.GetShip()->GetMyInventory()->GetItemsByFlagRange(flagHiSlot0, flagHiSlot7, modules);
    for (const InventoryItemRef& module : modules) {
        if (module->type().HasEffect(EVEEffectID::targetAttack))
            m_client.GetShip()->GetModuleManager()->Activate(module->itemID(), EVEEffectID::targetAttack, m_targetID, 1000);
        else if (module->type().HasEffect(EVEEffectID::projectileFired))
            m_client.GetShip()->GetModuleManager()->Activate(module->itemID(), EVEEffectID::projectileFired, m_targetID, 1000);
        else if (module->type().HasEffect(EVEEffectID::missileLaunching))
            m_client.GetShip()->GetModuleManager()->Activate(module->itemID(), EVEEffectID::missileLaunching, m_targetID, 1000);
    }
}

void BotBrain::SetManualFollow(Client& leader)
{
    m_manualMining = false;
    m_returnWarpStarted = false;
    if (m_client.GetShipSE() != nullptr && leader.GetShipSE() != nullptr)
        m_client.GetShipSE()->DestinyMgr()->Follow(leader.GetShipSE(), 2500);
    SetState(State::Manual);
}

void BotBrain::SetManualStay()
{
    m_manualMining = false;
    m_returnWarpStarted = false;
    if (m_client.GetShipSE() != nullptr)
        m_client.GetShipSE()->DestinyMgr()->Halt();
    SetState(State::Manual);
}

void BotBrain::RequestDock()
{
    m_manualMining = false;
    m_returnWarpStarted = false;
    StopMining();
    if (m_client.IsDocked()) {
        SetState(State::Manual);
        return;
    }
    SetState(State::Returning);
}

void BotBrain::RequestMine(uint32 asteroidTypeID)
{
    m_manualMining = true;
    m_returnWarpStarted = false;
    m_requestedAsteroidTypeID = asteroidTypeID;
    SetState(m_client.IsDocked() ? State::Docked : State::WarpToBelt);
}

void BotBrain::RequestAttack(uint32 targetID)
{
    m_manualMining = false;
    m_returnWarpStarted = false;
    StopMining();
    m_targetID = targetID;
    SetState(State::Attacking);
}

} // namespace PlayerBot
