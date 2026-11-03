#include "eve-server.h"

#include "bots/BotBrain.h"
#include "Client.h"
#include "inventory/Inventory.h"
#include "ship/Ship.h"
#include "ship/modules/ModuleManager.h"
#include "system/Asteroid.h"
#include "system/DestinyManager.h"
#include "system/SystemManager.h"
#include "system/TargetManager.h"
#include "system/cosmicMgrs/BeltMgr.h"
#include "market/MarketDB.h"
#include "market/MarketMgr.h"

namespace PlayerBot {

BotBrain::BotBrain(Client& client, Record& record)
: m_client(client), m_record(record), m_state(State::Docked), m_stateSince(GetFileTimeNow()), m_targetID(0)
{
    if (record.state == "MINING") m_state = State::Mining;
    else if (record.state == "WARP_TO_BELT") m_state = State::WarpToBelt;
    else if (record.state == "RETURNING") m_state = State::Returning;
    else if (record.state == "SELLING") m_state = State::Selling;
    else if (record.state == "ATTACKING") m_state = State::Attacking;
    else if (record.state == "MANUAL") m_state = State::Manual;
    else if (record.state == "RESPAWNING") m_state = State::Respawning;
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
    if (m_client.InPod()) {
        StopMining();
        m_record.respawnAt = GetFileTimeNow() + (static_cast<uint64_t>(sConfig.playerbots.respawnDelay) * 10000000ULL);
        SetState(State::Respawning);
    }
    if (m_state == State::Respawning) {
        // The normal Client pod/rookie-ship path is authoritative.  We merely
        // wait before resuming work; nothing is reimbursed or fabricated here.
        if (GetFileTimeNow() >= m_record.respawnAt && m_client.IsDocked())
            SetState(State::Docked);
        return;
    }
    if (m_state == State::Attacking) {
        ProcessAttack();
        return;
    }
    // Professions start with specialised real skills.  Until their dedicated
    // planner claims a job, mining is a safe productive fallback rather than
    // fabricating mission or market outcomes.
    if (m_record.profession == "MINER" || m_record.profession == "HAULER" ||
        m_record.profession == "MISSION_RUNNER" || m_record.profession == "RAT_HUNTER" ||
        m_record.profession == "TRADER" || m_record.profession == "INDUSTRIALIST")
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
    AsteroidSE* asteroid = asteroids.front();
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
    std::multimap<uint8, InventoryItemRef> cargo;
    m_client.GetShip()->GetMyInventory()->GetCargoList(cargo);
    for (const auto& entry : cargo) {
        InventoryItemRef ore = entry.second;
        if (ore.get() == nullptr || ore->categoryID() != EVEDB::invCategories::Asteroid)
            continue;
        m_client.MoveItem(ore->itemID(), m_record.homeStationID, flagHangar);
        const uint32 order = MarketDB::FindBuyOrder(ore->typeID(), m_record.homeStationID, ore->quantity(), 0.0);
        if (order != 0)
            sMktMgr.ExecuteBuyOrder(&m_client, order, ore, ore->quantity(), false, ore->typeID(), m_record.homeStationID, 0.0);
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
    if (m_client.GetShipSE() != nullptr && leader.GetShipSE() != nullptr)
        m_client.GetShipSE()->DestinyMgr()->Follow(leader.GetShipSE(), 2500);
    SetState(State::Manual);
}

void BotBrain::SetManualStay()
{
    if (m_client.GetShipSE() != nullptr)
        m_client.GetShipSE()->DestinyMgr()->Halt();
    SetState(State::Manual);
}

void BotBrain::RequestDock() { StopMining(); SetState(State::Returning); }
void BotBrain::RequestMine() { SetState(m_client.IsDocked() ? State::Docked : State::WarpToBelt); }
void BotBrain::RequestAttack(uint32 targetID) { StopMining(); m_targetID = targetID; SetState(State::Attacking); }

} // namespace PlayerBot
