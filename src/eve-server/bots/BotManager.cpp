#include "eve-server.h"

#include "bots/BotManager.h"
#include "bots/BotBrain.h"
#include "bots/BotClient.h"
#include "Client.h"
#include "corporation/CorporationDB.h"
#include "fleet/FleetService.h"
#include "inventory/ItemFactory.h"
#include "system/SystemManager.h"

#include <algorithm>
#include <cctype>

namespace PlayerBot {

BotManager::BotManager()
: m_services(nullptr), m_initialized(false), m_processCursor(0), m_nextLoginAt(0)
{}
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
    m_chatter.Initialize();

    uint32 created = 0;
    if (!BotDB::EnsureProvisioned(sConfig.playerbots.maxBots, services, created))
        sLog.Warning("      PlayerBots", "Automatic provisioning was skipped; apply the PlayerBots migration first.");

    std::vector<Record> records;
    if (!BotDB::Load(records))
        return;
    for (const Record& record : records)
        m_records.emplace(record.characterID, record);

    for (const auto& entry : m_records) {
        if (m_pendingLogin.size() >= sConfig.playerbots.maxBots)
            break;
        m_loginQueue.push_back(entry.first);
        m_pendingLogin.insert(entry.first);
    }
    m_nextLoginAt = GetFileTimeNow();
    sLog.Green("      PlayerBots", "Queued %zu persistent PlayerBots for staggered login (limit %u).",
                m_pendingLogin.size(), sConfig.playerbots.maxBots);
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

    // Avoid creating a 60-client/entity/warp burst during world startup.
    // Login one virtual pilot per second; brains are separately time-sliced.
    const uint64_t now = GetFileTimeNow();
    if (!m_loginQueue.empty() && m_clients.size() < sConfig.playerbots.maxBots && now >= m_nextLoginAt) {
        const uint32 characterID = m_loginQueue.front();
        m_loginQueue.pop_front();
        m_pendingLogin.erase(characterID);

        std::map<uint32, Record>::iterator record = m_records.find(characterID);
        if (record != m_records.end() && m_clients.find(characterID) == m_clients.end() && !Login(record->second)) {
            // Retry transient failures after the rest of the queue has had a
            // chance to start; a bad record must not block the entire fleet.
            m_loginQueue.push_back(characterID);
            m_pendingLogin.insert(characterID);
            m_nextLoginAt = now + 30ULL * 10000000ULL;
        } else {
            m_nextLoginAt = now + 10000000ULL;
        }
    }

    // Network/LLM work is completed off-thread.  Only this small drain and
    // the real Client notification run on the world thread.
    for (const ChatterReply& reply : m_chatter.Drain()) {
        BotClient* bot = nullptr;
        std::map<uint32, std::unique_ptr<BotClient> >::iterator it = m_clients.find(reply.characterID);
        if (it != m_clients.end())
            bot = it->second.get();
        if (bot != nullptr && !reply.text.empty())
            bot->SelfChatMessage("%s", reply.text.c_str());
    }

    if (m_brains.empty() || sConfig.playerbots.cpuBudget == 0)
        return;

    // A fixed range-for always starts at the lowest characterID, starving all
    // later bots whenever the configured budget is smaller than the fleet.
    // Rotate the starting point so every brain gets its share over time.
    std::map<uint32, std::unique_ptr<BotBrain> >::iterator brain =
        m_brains.upper_bound(m_processCursor);
    if (brain == m_brains.end())
        brain = m_brains.begin();

    const size_t count = std::min<size_t>(sConfig.playerbots.cpuBudget, m_brains.size());
    for (size_t processed = 0; processed < count; ++processed) {
        const uint32 characterID = brain->first;
        BotBrain* current = brain->second.get();
        ++brain;
        if (brain == m_brains.end())
            brain = m_brains.begin();

        m_processCursor = characterID;
        current->Process();
        std::map<uint32, Record>::iterator record = m_records.find(characterID);
        if (record != m_records.end())
            Save(record->second);
    }
}

void BotManager::Save(Record& record) { BotDB::SaveState(record); }

void BotManager::Shutdown()
{
    if (!m_initialized)
        return;
    for (auto& entry : m_records)
        Save(entry.second);
    m_chatter.Shutdown();
    m_brains.clear();
    m_clients.clear();
    m_loginQueue.clear();
    m_pendingLogin.clear();
    m_records.clear();
    m_processCursor = 0;
    m_nextLoginAt = 0;
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
    std::vector<uint32> selected;
    for (auto& entry : m_records) {
        if (selected.size() >= count)
            break;
        if (entry.second.profession != requested || m_clients.find(entry.first) != m_clients.end())
            continue;
        if (m_pendingLogin.find(entry.first) == m_pendingLogin.end() &&
            m_clients.size() + m_pendingLogin.size() >= sConfig.playerbots.maxBots)
            continue;
        selected.push_back(entry.first);
    }

    // All persistent records may already be in the startup queue. Promote the
    // requested profession so /bot spawn has an observable effect without
    // violating the one-login-per-second startup budget.
    for (uint32 characterID : selected) {
        std::deque<uint32>::iterator queued = std::find(m_loginQueue.begin(), m_loginQueue.end(), characterID);
        if (queued != m_loginQueue.end())
            m_loginQueue.erase(queued);
        else
            m_pendingLogin.insert(characterID);
    }
    for (std::vector<uint32>::reverse_iterator it = selected.rbegin(); it != selected.rend(); ++it)
        m_loginQueue.push_front(*it);
    if (!selected.empty())
        m_nextLoginAt = GetFileTimeNow();

    result = "Prioritized " + std::to_string(selected.size()) + " persistent " + requested +
        " bot(s) for staggered login.";
    return !selected.empty();
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

void BotManager::Follow(Client& leader, uint32 botID)
{
    for (auto& entry : m_brains)
        if (botID == 0 || entry.first == botID)
            entry.second->SetManualFollow(leader);
}

void BotManager::Stay(uint32 botID)
{
    for (auto& entry : m_brains)
        if (botID == 0 || entry.first == botID)
            entry.second->SetManualStay();
}

void BotManager::Mine(uint32 botID, uint32 asteroidTypeID)
{
    for (auto& entry : m_brains)
        if (botID == 0 || entry.first == botID)
            entry.second->RequestMine(asteroidTypeID);
}

void BotManager::Dock(uint32 botID)
{
    for (auto& entry : m_brains)
        if (botID == 0 || entry.first == botID)
            entry.second->RequestDock();
}

bool BotManager::Attack(uint32 targetID, std::string& result, uint32 botID)
{
    uint32 ordered = 0;
    for (auto& entry : m_brains) {
        if (botID != 0 && entry.first != botID)
            continue;
        std::map<uint32, std::unique_ptr<BotClient> >::iterator clientIt = m_clients.find(entry.first);
        if (clientIt == m_clients.end())
            continue;
        Client* client = clientIt->second.get();
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

bool BotManager::CorpInvite(uint32 corporationID, uint32 botID, std::string& result)
{
    if (!IsPlayerCorp(corporationID)) {
        result = "The target corporation does not accept player membership.";
        return false;
    }

    BotClient* bot = nullptr;
    if (botID != 0) {
        std::map<uint32, std::unique_ptr<BotClient> >::iterator it = m_clients.find(botID);
        if (it != m_clients.end())
            bot = it->second.get();
    } else if (!m_clients.empty()) {
        bot = m_clients.begin()->second.get();
    }
    if (bot == nullptr || bot->GetChar().get() == nullptr) {
        result = "No online PlayerBot matches that corporation invite.";
        return false;
    }

    CorpData data = bot->GetChar()->GetCorpData();
    const uint32 oldCorp = data.corporationID;
    data.corporationID = corporationID;
    CorporationDB::GetCorpData(data);
    data.baseID = data.corpHQ != 0 ? data.corpHQ : bot->GetStationID();
    data.corpRole = Corp::Role::Member;
    data.rolesAtAll = Corp::Role::Member;
    data.rolesAtBase = Corp::Role::Member;
    data.rolesAtHQ = Corp::Role::Member;
    data.rolesAtOther = Corp::Role::Member;
    data.grantableRoles = Corp::Role::Member;
    data.grantableRolesAtBase = Corp::Role::Member;
    data.grantableRolesAtHQ = Corp::Role::Member;
    data.grantableRolesAtOther = Corp::Role::Member;
    bot->GetChar()->JoinCorporation(data);
    result = bot->GetName();
    result += " joined corporation " + std::to_string(corporationID) +
              " (previous corporation " + std::to_string(oldCorp) + ").";
    return true;
}

bool BotManager::Give(uint32 typeID, uint32 quantity, uint32 botID, std::string& result)
{
    if (quantity == 0 || quantity > 1000000000U) {
        result = "Quantity must be between 1 and 1000000000.";
        return false;
    }
    if (typeID > UINT16_MAX) {
        result = "Item typeID is outside the supported range.";
        return false;
    }
    BotClient* bot = nullptr;
    if (botID != 0) {
        std::map<uint32, std::unique_ptr<BotClient> >::iterator it = m_clients.find(botID);
        if (it != m_clients.end())
            bot = it->second.get();
    } else if (!m_clients.empty()) {
        bot = m_clients.begin()->second.get();
    }
    if (bot == nullptr || bot->GetChar().get() == nullptr) {
        result = "No online PlayerBot matches that item transfer.";
        return false;
    }
    if (sItemFactory.GetType(static_cast<uint16>(typeID)) == nullptr) {
        result = "Unknown item typeID.";
        return false;
    }

    const bool inSpace = bot->IsInSpace() && bot->GetShip().get() != nullptr;
    const uint32 destination = inSpace ? bot->GetShipID() : bot->GetStationID();
    if (destination == 0) {
        result = "The bot has no valid hangar or cargo destination.";
        return false;
    }
    ItemData item(static_cast<uint16>(typeID), bot->GetCharacterID(), locTemp, flagNone, quantity);
    sItemFactory.SetUsingClient(bot);
    InventoryItemRef itemRef = sItemFactory.SpawnItem(item);
    sItemFactory.UnsetUsingClient();
    if (itemRef.get() == nullptr) {
        result = "The server rejected that item creation.";
        return false;
    }
    if (inSpace)
        bot->GetShip()->AddItemByFlag(flagCargoHold, itemRef);
    else
        itemRef->Move(destination, flagHangar, true);
    itemRef->SaveItem();
    result = "Transferred " + std::to_string(quantity) + " x " +
             std::to_string(typeID) + " to " + bot->GetName() + ".";
    return true;
}

bool BotManager::CreateCourierContract(uint32 botID, uint32 assigneeID, uint32 reward, std::string& result)
{
    std::map<uint32, std::unique_ptr<BotClient> >::iterator it = m_clients.find(botID);
    if (it == m_clients.end() || it->second->GetChar().get() == nullptr) {
        result = "The contract issuer bot is not online.";
        return false;
    }
    BotClient& bot = *it->second;
    if (!bot.IsDocked() || bot.GetStationID() == 0) {
        result = "The bot must be docked at a station to issue a contract.";
        return false;
    }
    if (reward > 0 && bot.GetBalance() < reward) {
        result = "The bot cannot escrow that reward.";
        return false;
    }

    const uint32 stationID = bot.GetStationID();
    const uint32 systemID = sDataMgr.GetStationSystem(stationID);
    const uint32 regionID = sDataMgr.GetStationRegion(stationID);
    std::string title = "EVEMU PlayerBot courier contract";
    std::string description = "Issued by a persistent PlayerBot using the normal contract table.";
    std::string escapedTitle, escapedDescription;
    sDatabase.DoEscapeString(escapedTitle, title);
    sDatabase.DoEscapeString(escapedDescription, description);

    if (reward > 0)
        bot.AddBalance(-static_cast<double>(reward), Account::CreditType::ISK);

    uint32 contractID = 0;
    DBerror error;
    const uint32 expireMinutes = 10080; // one week
    if (!sDatabase.RunQueryLID(error, contractID,
        "INSERT INTO ctrContracts "
        "(contractType, issuerID, issuerCorpID, forCorp, isPrivate, assigneeID, "
        "dateIssued, dateExpired, expireTimeInMinutes, duration, numDays, "
        "startStationID, startSolarSystemID, startRegionID, endStationID, endSolarSystemID, endRegionID, "
        "price, reward, collateral, title, description, issuerAllianceID, startStationDivision) "
        "VALUES "
        "(3, %u, %u, 0, %u, %u, %" PRIi64 ", %" PRIi64 ", %u, %u, %u, "
        "%u, %u, %u, 0, %u, %u, 0, %u, 0, '%s', '%s', %u, 0)",
        bot.GetCharacterID(), bot.GetCorporationID(), assigneeID != 0 ? 1 : 0, assigneeID,
        static_cast<int64>(GetFileTimeNow()),
        static_cast<int64>(GetRelativeFileTime(0, 0, expireMinutes)), expireMinutes,
        expireMinutes, expireMinutes / 1440, stationID, systemID, regionID, systemID,
        reward, escapedTitle.c_str(), escapedDescription.c_str(), bot.GetAllianceID())) {
        if (reward > 0)
            bot.AddBalance(static_cast<double>(reward), Account::CreditType::ISK);
        result = "Contract creation failed: " + std::string(error.c_str());
        return false;
    }

    result = "Created courier contract " + std::to_string(contractID) +
             " from " + bot.GetName() + ".";
    return true;
}

std::vector<ChatReply> BotManager::HandleChat(Client& speaker, const std::string& message)
{
    std::vector<ChatReply> replies;
    if (!m_initialized || message.empty() || Find(speaker.GetCharacterID()) != nullptr)
        return replies;

    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const std::string prefix = sConfig.playerbots.chatPrefix;
    const bool prefixed = !prefix.empty() && message.compare(0, prefix.size(), prefix) == 0;

    uint32 addressed = 0;
    for (const auto& entry : m_clients) {
        std::string name = entry.second->GetName();
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lower.find(name) != std::string::npos) {
            addressed = entry.first;
            break;
        }
    }
    if (!prefixed && addressed == 0)
        return replies;

    std::string command = prefixed ? message.substr(prefix.size()) : "";
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front())))
        command.erase(command.begin());
    std::transform(command.begin(), command.end(), command.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::string result;
    if (!prefixed && addressed != 0) {
        const std::map<uint32, Record>::const_iterator record = m_records.find(addressed);
        const std::map<uint32, std::unique_ptr<BotClient> >::const_iterator bot = m_clients.find(addressed);
        if (record != m_records.end() && bot != m_clients.end() &&
            m_chatter.Queue(addressed, bot->second->GetName(), record->second.personality, message))
            return replies;
    }
    if (command == "follow" || command == "come") {
        Follow(speaker);
        result = "Following your ship.";
    } else if (command == "stay") {
        Stay();
        result = "Holding position.";
    } else if (command == "dock") {
        Dock();
        result = "Returning to station.";
    } else if (command == "mine") {
        Mine();
        result = "Mining operation acknowledged.";
    } else if (command == "fleet") {
        Fleet(speaker, result);
    } else if (command == "status") {
        result = "Online PlayerBots: " + std::to_string(m_clients.size()) + ".";
    } else if (prefixed && command.compare(0, 7, "attack ") == 0) {
        const std::string target = command.substr(7);
        bool numeric = !target.empty();
        for (char ch : target)
            numeric = numeric && std::isdigit(static_cast<unsigned char>(ch));
        if (numeric)
            Attack(static_cast<uint32>(strtoul(target.c_str(), nullptr, 10)), result);
        else
            result = "Use " + prefix + "attack <entityID>.";
    } else if (prefixed && !command.empty()) {
        result = "Commands: " + prefix + "follow, " + prefix + "stay, " + prefix +
            "dock, " + prefix + "mine, " + prefix + "fleet, " + prefix + "status.";
    } else {
        static const char* acknowledgements[] = {
            "Acknowledged. I am flying my assigned route.",
            "Copy that. The operation is running on live orders.",
            "o7. Let me know if you need a fleet or mining support."
        };
        result = acknowledgements[addressed % (sizeof(acknowledgements) / sizeof(acknowledgements[0]))];
    }

    if (addressed == 0 && !m_clients.empty())
        addressed = m_clients.begin()->first;
    if (addressed != 0 && !result.empty())
        replies.push_back({addressed, result});
    return replies;
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
