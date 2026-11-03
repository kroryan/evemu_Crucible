#include "eve-server.h"

#include "bots/BotBrain.h"
#include "Client.h"
#include "inventory/Inventory.h"
#include "inventory/ItemFactory.h"
#include "inventory/ItemType.h"
#include "ship/Ship.h"
#include "ship/modules/ModuleManager.h"
#include "npc/NPC.h"
#include "market/MarketProxyService.h"
#include "system/Asteroid.h"
#include "system/DestinyManager.h"
#include "system/SystemManager.h"
#include "system/TargetManager.h"
#include "system/cosmicMgrs/BeltMgr.h"
#include "market/MarketDB.h"
#include "market/MarketMgr.h"
#include "station/Station.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace PlayerBot {

BotBrain::BotBrain(Client& client, Record& record)
: m_client(client), m_record(record), m_state(State::Docked), m_stateSince(GetFileTimeNow()),
  m_autoStartAt(GetFileTimeNow() + ((static_cast<uint64_t>(record.characterID) * 37ULL) % 60ULL) * 10000000ULL),
  m_targetID(0), m_requestedAsteroidTypeID(0), m_manualMining(false), m_returnWarpStarted(false)
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
        case State::Manual: return "MANUAL";
        case State::Respawning: return "RESPAWNING";
    }
    return "DOCKED";
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
    if (m_state == State::Docked && m_client.IsDocked() && EnsureProfessionShip())
        return;
    if (!m_manualMining && m_state != State::Attacking && m_state != State::Returning && now < m_autoStartAt)
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
    // MINER is the complete production loop.  HAULER, MISSION_RUNNER and
    // INDUSTRIALIST retain that real fallback until their service-specific
    // planners have a job, so they never fabricate a mission or manufacture
    // result.
    ProcessMiner();
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
    if (currentShip.get() == nullptr || currentShip->typeID() != 601 ||
        m_client.GetStationID() == 0 || m_client.SystemMgr() == nullptr)
        return false;

    uint16 hullTypeID = 0;
    std::vector<std::pair<uint16, EVEItemFlags> > loadout;
    if (m_record.profession == "MINER") {
        hullTypeID = 582; // Bantam
        loadout.push_back({3651, flagHiSlot0}); // Civilian Miner
        loadout.push_back({21857, flagMidSlot0}); // Civilian Afterburner
    } else if (m_record.profession == "HAULER") {
        hullTypeID = 648; // Badger
        loadout.push_back({3651, flagHiSlot0}); // Civilian Miner for the real mining fallback
        loadout.push_back({21855, flagLowSlot0}); // Civilian Expanded Cargohold
    } else if (m_record.profession == "MISSION_RUNNER") {
        hullTypeID = 602; // Kestrel
        loadout.push_back({3651, flagHiSlot0}); // Civilian Miner while mission service work remains
        loadout.push_back({32461, flagHiSlot1}); // Civilian Standard Missile Launcher
    } else if (m_record.profession == "RAT_HUNTER") {
        hullTypeID = 603; // Merlin
        loadout.push_back({3638, flagHiSlot0}); // Civilian Gatling Railgun
        loadout.push_back({21853, flagLowSlot0}); // Civilian Armor Repairer
    } else if (m_record.profession == "TRADER") {
        hullTypeID = 583; // Condor
        loadout.push_back({21857, flagMidSlot0}); // Civilian Afterburner
        loadout.push_back({21855, flagLowSlot0}); // Civilian Expanded Cargohold
    } else if (m_record.profession == "INDUSTRIALIST") {
        hullTypeID = 620; // Osprey
        loadout.push_back({3651, flagHiSlot0}); // Civilian Miner
        loadout.push_back({21855, flagLowSlot0}); // Civilian Expanded Cargohold
    }
    if (hullTypeID == 0)
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

        ShipItemRef roleShip(nullptr);
        for (const InventoryItemRef& item : hangarItems) {
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

        for (const std::pair<uint16, EVEItemFlags>& fit : loadout) {
            std::vector<InventoryItemRef> slotItems;
            roleShip->GetMyInventory()->GetItemsByFlag(fit.second, slotItems);
            bool fitted = false;
            for (const InventoryItemRef& item : slotItems)
                if (item.get() != nullptr && item->ownerID() == characterID && item->typeID() == fit.first) {
                    fitted = true;
                    break;
                }
            if (fitted)
                continue;

            if (sItemFactory.GetType(fit.first) == nullptr) {
                restoreFactoryClient();
                return false;
            }
            ItemData moduleData(fit.first, characterID, locTemp, flagNone);
            InventoryItemRef module = sItemFactory.SpawnItem(moduleData);
            if (module.get() == nullptr) {
                restoreFactoryClient();
                return false;
            }
            module->ChangeSingleton(true);
            module->Move(roleShip->itemID(), fit.second);
            module->SetAttribute(AttrOnline, EvilOne, false);
            module->SaveItem();
        }

        roleShip->SaveShip();
        // These seeded starter hulls are already in the hangar; SetShip uses
        // the normal character active-ship path without sending a client RPC.
        m_client.SetShip(roleShip);
        restoreFactoryClient();
        sLog.Green("PlayerBot", "%s boarded %s and fitted its starter modules.",
                   m_client.GetName(), hullType->name());
        return true;
    } catch (...) {
        restoreFactoryClient();
        sLog.Warning("PlayerBot", "Could not install the %s starter hull for %s; will retry when docked.",
                     hullType->name(), m_client.GetName());
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
    if (m_client.GetShipSE()->DestinyMgr()->IsWarping())
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
        case State::WarpToBelt:
            if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr)
                return;
            if (!m_client.GetShipSE()->DestinyMgr()->IsWarping())
                WarpToBelt();
            break;
        case State::Mining:
            if (m_client.GetShipSE() == nullptr || m_client.GetShipSE()->DestinyMgr()->IsWarping())
                return;
            if (m_targetID == 0 && SelectNpcTarget())
                SetState(State::Attacking);
            else if (m_targetID == 0 && GetFileTimeNow() - m_stateSince >= 60ULL * 10000000ULL)
                SetState(State::Returning);
            break;
        case State::Attacking:
            ProcessAttack();
            if (m_targetID == 0)
                SetState(State::Returning);
            break;
        case State::Returning:
            if (m_client.GetShipSE() == nullptr || m_record.homeStationID == 0 ||
                m_client.GetShipSE()->DestinyMgr()->IsWarping())
                return;
            if (!m_returnWarpStarted) {
                SystemEntity* station = m_client.SystemMgr()->FindEntityByID(m_record.homeStationID);
                if (station == nullptr)
                    return;
                m_client.GetShipSE()->DestinyMgr()->WarpTo(station->GetPosition(), 0);
                m_client.SetDockStationID(m_record.homeStationID);
                m_returnWarpStarted = true;
                return;
            }
            m_returnWarpStarted = false;
            SetState(State::Selling);
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

void BotBrain::ProcessTrader()
{
    if (m_state == State::Returning) {
        if (m_client.IsDocked()) {
            m_returnWarpStarted = false;
            SetState(State::Docked);
            return;
        }
        if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr)
            return;

        DestinyManager* destiny = m_client.GetShipSE()->DestinyMgr();
        if (destiny->IsWarping())
            return;
        if (!m_returnWarpStarted) {
            SystemEntity* station = m_client.SystemMgr()->FindEntityByID(m_record.homeStationID);
            if (station == nullptr)
                return;
            destiny->WarpTo(station->GetPosition(), 0);
            m_client.SetDockStationID(m_record.homeStationID);
            m_returnWarpStarted = true;
            return;
        }

        m_client.DockToStation();
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
    if (m_record.homeStationID == 0)
        m_record.homeStationID = m_client.GetStationID();
    const uint64_t now = GetFileTimeNow();
    // Each attempt is deliberately spread out and stays within the one-Hz bot
    // budget. Open orders are backed by this character's wallet or inventory.
    if (now - m_stateSince < 60ULL * 10000000ULL)
        return;
    m_stateSince = now;

    StationItemRef station = m_client.SystemMgr()->GetStationFromInventory(m_record.homeStationID);
    MarketProxyService* market = m_client.services().Lookup<MarketProxyService>("marketProxy");
    if (station.get() == nullptr || market == nullptr)
        return;

    // First offer ore that this character already owns. Posting goes through
    // MarketProxyService::PlaceCharOrder so fees and item removal use the
    // same path as a connected pilot.
    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);
    for (const InventoryItemRef& item : hangarItems) {
        if (item.get() == nullptr || item->ownerID() != m_client.GetCharacterID() ||
            item->locationID() != m_record.homeStationID ||
            item->categoryID() != EVEDB::invCategories::Asteroid ||
            MarketDB::FindOwnerOrder(m_client.GetCharacterID(), m_record.homeStationID,
                                     item->typeID(), false) != 0)
            continue;

        const ItemType* itemType = sItemFactory.GetType(item->typeID());
        if (itemType == nullptr)
            continue;

        const uint32 buyID = MarketDB::FindBuyOrder(item->typeID(), m_record.homeStationID, 1, 0.0);
        const uint32 sellID = MarketDB::FindSellOrder(item->typeID(), m_record.homeStationID, 1, 1.0e12);
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
        if (quantity != 0 && market->PlaceBotLimitOrder(m_client, m_record.homeStationID,
                item->typeID(), askPrice, quantity, false, item->itemID(), 7))
            _log(MARKET__MESSAGE, "PlayerBot %s placed a real sell order for %u %s at %.2f ISK.",
                 m_client.GetName(), quantity, item->name(), askPrice);
        return;
    }

    // Rotate the ten Trader pilots among common highsec ores. Their bids are
    // real character orders with limited escrow; miners can sell into them via
    // ExecuteBuyOrder, after which the Trader lists the acquired stack above
    // the current bid/ask spread.
    static const std::array<uint32, 4> oreTypes = {{1230, 1228, 1224, 18}};
    const uint32 ordinal = (m_client.GetCharacterID() >= 90000001U)
        ? (m_client.GetCharacterID() - 90000001U) / 6U : m_client.GetCharacterID();
    const uint32 typeID = oreTypes[ordinal % oreTypes.size()];
    if (MarketDB::FindOwnerOrder(m_client.GetCharacterID(), m_record.homeStationID, typeID, false) != 0 ||
        MarketDB::FindOwnerOrder(m_client.GetCharacterID(), m_record.homeStationID, typeID, true) != 0)
        return;

    const ItemType* type = sItemFactory.GetType(typeID);
    if (type == nullptr || m_client.GetBalance() >= sConfig.playerbots.maxISK)
        return;

    const uint32 buyID = MarketDB::FindBuyOrder(typeID, m_record.homeStationID, 1, 0.0);
    const uint32 sellID = MarketDB::FindSellOrder(typeID, m_record.homeStationID, 1, 1.0e12);
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

    if (market->PlaceBotLimitOrder(m_client, m_record.homeStationID, typeID,
                                   bidPrice, quantity, true, 0, 7))
        _log(MARKET__MESSAGE, "PlayerBot %s placed a real buy order for %u %s at %.2f ISK.",
             m_client.GetName(), quantity, type->name(), bidPrice);
}

void BotBrain::ProcessMiner()
{
    switch (m_state) {
        case State::Docked:
            if (!IsMiningShipReady())
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
        case State::WarpToBelt:
            if (!m_client.IsInSpace() || m_client.GetShipSE() == nullptr)
                return;
            if (!m_client.GetShipSE()->DestinyMgr()->IsWarping())
                WarpToBelt();
            break;
        case State::Mining:
            if (m_client.GetShipSE() == nullptr)
                return;
            if (m_client.GetShipSE()->DestinyMgr()->IsWarping())
                return;
            if (m_targetID == 0)
                StartMining();
            if (GetFileTimeNow() - m_stateSince >= static_cast<uint64_t>(sConfig.playerbots.minerCycleSeconds) * 10000000ULL) {
                StopMining();
                SetState(State::Returning);
            }
            break;
        case State::Returning:
            if (m_client.GetShipSE() == nullptr || m_record.homeStationID == 0)
                return;
            if (!m_client.GetShipSE()->DestinyMgr()->IsWarping()) {
                SystemEntity* station = m_client.SystemMgr()->FindEntityByID(m_record.homeStationID);
                if (station != nullptr) {
                    m_client.GetShipSE()->DestinyMgr()->WarpTo(station->GetPosition(), 0);
                    m_client.SetDockStationID(m_record.homeStationID);
                    // Client::DockToStation is the same final docking path used
                    // after a client docking request; the preceding warp is real.
                    SetState(State::Selling);
                }
            }
            break;
        case State::Selling:
            if (m_client.GetShipSE() != nullptr && m_client.GetShipSE()->DestinyMgr()->IsWarping())
                return;
            if (m_client.IsInSpace())
                m_client.DockToStation();
            if (m_client.IsDocked()) {
                SellOre();
                SetState(State::Docked);
            }
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
    const double wealthHeadroom = sConfig.playerbots.maxISK - m_client.GetBalance();
    if (wealthHeadroom <= 0.0)
        return;

    std::multimap<uint8, InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetCargoList(cargo);
    for (const auto& entry : cargo) {
        InventoryItemRef ore = entry.second;
        if (ore.get() == nullptr || ore->categoryID() != EVEDB::invCategories::Asteroid)
            continue;

        m_client.MoveItem(ore->itemID(), m_record.homeStationID, flagHangar);
        const uint32 orderID = MarketDB::FindBuyOrder(ore->typeID(), m_record.homeStationID, 1, 0.0);
        Market::OrderInfo order;
        if (orderID == 0 || !MarketDB::GetOrderInfo(orderID, order) ||
            order.ownerID == m_client.GetCharacterID() || order.price <= 0.0)
            continue;

        // Market fills are bounded by both the public order and the bot's
        // configured wallet ceiling. Split the stack before settlement because
        // ExecuteBuyOrder transfers the full item reference it receives.
        const uint32 maxByOrder = order.quantity;
        const double headroom = sConfig.playerbots.maxISK - m_client.GetBalance();
        const uint32 maxByWealth = headroom > 0.0
            ? static_cast<uint32>(std::min<double>(std::floor(headroom / order.price), 10000000.0)) : 0;
        const uint32 quantity = std::min<uint32>(ore->quantity(), std::min(maxByOrder, maxByWealth));
        if (quantity == 0)
            continue;

        InventoryItemRef saleItem = ore;
        if (quantity < ore->quantity()) {
            saleItem = ore->Split(quantity);
            if (saleItem.get() == nullptr)
                continue;
        }
        sMktMgr.ExecuteBuyOrder(&m_client, orderID, saleItem, quantity, false,
                                ore->typeID(), m_record.homeStationID, order.price);
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
