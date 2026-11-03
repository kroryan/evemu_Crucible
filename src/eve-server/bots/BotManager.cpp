#include "eve-server.h"

#include "bots/BotManager.h"
#include "bots/BotBrain.h"
#include "bots/BotClient.h"
#include "Client.h"
#include "fleet/FleetService.h"
#include "system/SystemManager.h"

namespace PlayerBot {

BotManager::BotManager() : m_services(nullptr), m_initialized(false) {}
BotManager::~BotManager() { Shutdown(); }

void BotManager::Initialize(EVEServiceManager& services)
{
    if (m_initialized)
        return;
    m_initialized = true;
    m_services = &services;
    if (!sConfig.playerbots.enabled) {
        sLog.Warning("      PlayerBots", "Disabled by configuration.");
        return;
    }

    uint32 created = 0;
    if (!BotDB::EnsureProvisioned(sConfig.playerbots.maxBots, services, created))
        sLog.Warning("      PlayerBots", "Automatic provisioning was skipped; apply the PlayerBots migration first.");

    std::vector<Record> records;
    if (!BotDB::Load(records))
        return;
    for (const Record& record : records)
        m_records.emplace(record.characterID, record);

    uint32 loggedIn = 0;
    for (auto& entry : m_records) {
        if (loggedIn >= sConfig.playerbots.maxBots)
            break;
        if (Login(entry.second))
            ++loggedIn;
    }
    sLog.Green("      PlayerBots", "Loaded %u persistent PlayerBots (limit %u).", loggedIn, sConfig.playerbots.maxBots);
}

bool BotManager::Login(Record& record)
{
    if (m_services == nullptr || m_clients.find(record.characterID) != m_clients.end())
        return false;
    std::unique_ptr<BotClient> client(new BotClient(*m_services));
    if (!client->Login(record.characterID)) {
        sLog.Error("      PlayerBots", "Unable to virtual-login bot character %u.", record.characterID);
        return false;
    }
    std::unique_ptr<BotBrain> brain(new BotBrain(*client, record));
    m_brains.emplace(record.characterID, std::move(brain));
    m_clients.emplace(record.characterID, std::move(client));
    return true;
}

void BotManager::Process()
{
    if (!m_initialized || !sConfig.playerbots.enabled)
        return;
    uint16 budget = sConfig.playerbots.cpuBudget;
    for (auto& entry : m_brains) {
        if (budget-- == 0)
            break;
        entry.second->Process();
        Save(m_records[entry.first]);
    }
}

void BotManager::Save(Record& record) { BotDB::SaveState(record); }

void BotManager::Shutdown()
{
    if (!m_initialized)
        return;
    for (auto& entry : m_records)
        Save(entry.second);
    m_brains.clear();
    m_clients.clear();
    m_initialized = false;
}

bool BotManager::Spawn(const std::string& profession, uint32 count, std::string& result)
{
    if (!m_initialized || !sConfig.playerbots.enabled) {
        result = "PlayerBots are disabled.";
        return false;
    }
    std::string requested = profession;
    std::transform(requested.begin(), requested.end(), requested.begin(), ::toupper);
    static const std::set<std::string> valid = {
        "MINER", "HAULER", "MISSION_RUNNER", "RAT_HUNTER", "TRADER", "INDUSTRIALIST"
    };
    if (valid.find(requested) == valid.end()) {
        result = "Unknown profession. Use miner, hauler, mission_runner, rat_hunter, trader or industrialist.";
        return false;
    }
    uint32 added = 0;
    for (auto& entry : m_records) {
        if (added >= count || m_clients.size() >= sConfig.playerbots.maxBots)
            break;
        if (entry.second.profession == requested && m_clients.find(entry.first) == m_clients.end() && Login(entry.second))
            ++added;
    }
    result = "Activated " + std::to_string(added) + " persistent " + requested + " bot(s).";
    return added != 0;
}

bool BotManager::Dismiss(uint32 characterID, std::string& result)
{
    std::map<uint32, std::unique_ptr<BotClient> >::iterator client = m_clients.find(characterID);
    if (client == m_clients.end()) {
        result = "Bot is not online.";
        return false;
    }
    Save(m_records[characterID]);
    m_brains.erase(characterID);
    m_clients.erase(client);
    result = "Bot logged out; its character, ship, inventory and market state were preserved.";
    return true;
}

void BotManager::Follow(Client& leader) { for (auto& entry : m_brains) entry.second->SetManualFollow(leader); }
void BotManager::Stay() { for (auto& entry : m_brains) entry.second->SetManualStay(); }
void BotManager::Mine() { for (auto& entry : m_brains) entry.second->RequestMine(); }
void BotManager::Dock() { for (auto& entry : m_brains) entry.second->RequestDock(); }

bool BotManager::Attack(uint32 targetID, std::string& result)
{
    uint32 ordered = 0;
    for (auto& entry : m_brains) {
        Client* client = m_clients[entry.first].get();
        if (client->IsInSpace() && client->SystemMgr()->FindEntityByID(targetID) != nullptr) {
            entry.second->RequestAttack(targetID);
            ++ordered;
        }
    }
    result = ordered == 0 ? "No online PlayerBot has that target on grid." :
        "Attack order sent to " + std::to_string(ordered) + " PlayerBot(s).";
    return ordered != 0;
}

bool BotManager::Fleet(Client& leader, std::string& result)
{
    uint32 fleetID = leader.GetFleetID();
    if (!IsFleetID(fleetID))
        fleetID = sFltSvc.CreateFleet(&leader);
    if (!IsFleetID(fleetID)) {
        result = "Unable to create a fleet.";
        return false;
    }

    uint32 joined = 0;
    for (auto& entry : m_clients) {
        Client* bot = entry.second.get();
        if (bot->InFleet())
            continue;
        int32 wingID = 0, squadID = 0;
        sFltSvc.GetRandUnitIDs(fleetID, wingID, squadID);
        if (sFltSvc.AddMember(bot, fleetID, wingID, squadID, Fleet::Job::None,
                              Fleet::Role::Member, Fleet::Booster::No))
            ++joined;
    }
    result = "Fleet " + std::to_string(fleetID) + " now has " + std::to_string(joined) + " PlayerBot(s) added.";
    return true;
}

std::string BotManager::Status() const
{
    std::ostringstream stream;
    stream << "PlayerBots online: " << m_clients.size() << "/" << sConfig.playerbots.maxBots << "<br>";
    for (const auto& entry : m_brains)
        stream << entry.first << " " << entry.second->GetStateName() << "<br>";
    return stream.str();
}

Client* BotManager::Find(uint32 characterID) const
{
    std::map<uint32, std::unique_ptr<BotClient> >::const_iterator it = m_clients.find(characterID);
    return it == m_clients.end() ? nullptr : it->second.get();
}

} // namespace PlayerBot
