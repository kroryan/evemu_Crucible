#include "eve-server.h"

#include "bots/BotManager.h"
#include "bots/BotBrain.h"
#include "bots/BotClient.h"
#include "Client.h"
#include "EntityList.h"
#include "account/AccountService.h"
#include "chat/LSCService.h"
#include "contract/ContractProxy.h"
#include "corporation/CorpRegistryBound.h"
#include "corporation/CorpRegistryService.h"
#include "corporation/CorporationDB.h"
#include "fleet/FleetService.h"
#include "inventory/Inventory.h"
#include "inventory/ItemFactory.h"
#include "ship/Ship.h"
#include "station/Station.h"
#include "system/SystemManager.h"
#include "system/SystemEntity.h"
#include "services/ServiceManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace PlayerBot {

BotManager::BotManager()
: m_services(nullptr), m_initialized(false), m_processCursor(0), m_corporationCursor(0),
  m_nextLoginAt(0), m_nextCorporationProcessAt(0), m_nextCorporationPresenceAt(0)
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

    const uint16 onlineLimit = std::min(sConfig.playerbots.maxBots,
                                        sConfig.playerbots.maxOnlineBots);
    for (const auto& entry : m_records) {
        if (m_pendingLogin.size() >= onlineLimit)
            break;
        m_loginQueue.push_back(entry.first);
        m_pendingLogin.insert(entry.first);
    }
    m_nextLoginAt = GetFileTimeNow();
    sLog.Green("      PlayerBots", "Queued %zu persistent PlayerBots for staggered login (limit %u).",
                m_pendingLogin.size(), onlineLimit);
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
    const uint16 onlineLimit = std::min(sConfig.playerbots.maxBots,
                                        sConfig.playerbots.maxOnlineBots);
    if (now >= m_nextCorporationPresenceAt) {
        RefreshCorporationPresence();
        m_nextCorporationPresenceAt = now + 5ULL * 10000000ULL;
    }
    if (!m_loginQueue.empty() &&
        (m_clients.size() < onlineLimit || m_corporationRequiredOnline.count(m_loginQueue.front())) &&
        now >= m_nextLoginAt) {
        const uint32 characterID = m_loginQueue.front();
        m_loginQueue.pop_front();
        m_pendingLogin.erase(characterID);

        std::map<uint32, Record>::iterator record = m_records.find(characterID);
        bool loginSucceeded = false;
        const auto retry = m_loginRetryAt.find(characterID);
        const bool retryDeferred = retry != m_loginRetryAt.end() && now < retry->second;
        if (!retryDeferred) {
            try {
                loginSucceeded = record != m_records.end() &&
                    m_clients.find(characterID) == m_clients.end() && Login(record->second);
            } catch (const PyException& error) {
                sLog.Error("PlayerBot", "Virtual login for bot %u raised a game UserError; retrying in 30 seconds.",
                           characterID);
                if (error.ssException != nullptr)
                    error.ssException->Dump(CLIENT__ERROR, "   ");
            } catch (const std::exception& error) {
                sLog.Error("PlayerBot", "Virtual login for bot %u failed: %s; retrying in 30 seconds.",
                           characterID, error.what());
            } catch (...) {
                sLog.Error("PlayerBot", "Virtual login for bot %u raised an unknown exception; retrying in 30 seconds.",
                           characterID);
            }
        }
        if (!loginSucceeded && record != m_records.end()) {
            // Retry transient failures after the rest of the queue has had a
            // chance to start; a bad record must not block the entire fleet.
            m_loginQueue.push_back(characterID);
            m_pendingLogin.insert(characterID);
            if (!retryDeferred)
                m_loginRetryAt[characterID] = now + 30ULL * 10000000ULL;
        } else {
            m_loginRetryAt.erase(characterID);
        }
        m_nextLoginAt = now + 10000000ULL;
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
        const auto retry = m_processRetryAt.find(characterID);
        if (retry != m_processRetryAt.end() && now < retry->second)
            continue;
        try {
            current->Process();
            std::map<uint32, Record>::iterator record = m_records.find(characterID);
            if (record != m_records.end())
                Save(record->second);
            m_processRetryAt.erase(characterID);
        } catch (const PyException& error) {
            sLog.Error("PlayerBot", "Bot brain for %s (%u) raised a game UserError; pausing this bot for 60 seconds.",
                       m_clients.find(characterID)->second->GetName(), characterID);
            if (error.ssException != nullptr)
                error.ssException->Dump(CLIENT__ERROR, "   ");
            m_processRetryAt[characterID] = now + 600000000ULL;
        } catch (const std::exception& error) {
            sLog.Error("PlayerBot", "Bot brain for %s (%u) failed: %s; pausing this bot for 60 seconds.",
                       m_clients.find(characterID)->second->GetName(), characterID, error.what());
            m_processRetryAt[characterID] = now + 600000000ULL;
        } catch (...) {
            sLog.Error("PlayerBot", "Bot brain for %s (%u) raised an unknown exception; pausing this bot for 60 seconds.",
                       m_clients.find(characterID)->second->GetName(), characterID);
            m_processRetryAt[characterID] = now + 600000000ULL;
        }
    }

    if (now >= m_nextCorporationProcessAt) {
        try {
            ProcessBotCorporations();
            m_nextCorporationProcessAt = now + 10000000ULL;
        } catch (const PyException& error) {
            sLog.Error("PlayerBot", "Bot corporation lifecycle raised a game UserError; retrying in 60 seconds.");
            if (error.ssException != nullptr)
                error.ssException->Dump(CLIENT__ERROR, "   ");
            m_nextCorporationProcessAt = now + 600000000ULL;
        } catch (const std::exception& error) {
            sLog.Error("PlayerBot", "Bot corporation lifecycle failed: %s; retrying in 60 seconds.", error.what());
            m_nextCorporationProcessAt = now + 600000000ULL;
        } catch (...) {
            sLog.Error("PlayerBot", "Bot corporation lifecycle raised an unknown exception; retrying in 60 seconds.");
            m_nextCorporationProcessAt = now + 600000000ULL;
        }
    }
}

void BotManager::RefreshCorporationPresence()
{
    std::vector<Client*> clients;
    sEntityList.GetClients(clients);
    std::set<uint32> humanCorps;
    for (Client* client : clients) {
        if (client != nullptr && client->GetChar().get() != nullptr &&
            m_records.find(client->GetCharacterID()) == m_records.end() &&
            client->GetCorporationID() != 0)
            humanCorps.insert(client->GetCorporationID());
    }
    std::set<uint32> required;
    DBQueryResult members;
    if (!sDatabase.RunQuery(members,
        "SELECT b.characterID,c.corporationID,EXISTS(SELECT 1 FROM chrCharacters human "
        "LEFT JOIN botCharacters hb ON hb.characterID=human.characterID "
        "WHERE human.corporationID=c.corporationID AND hb.characterID IS NULL) "
        "FROM botCharacters b JOIN chrCharacters c ON c.characterID=b.characterID "
        "WHERE b.enabled=1 ORDER BY c.corporationID, "
        "(b.profession IN ('MINER','HAULER','INDUSTRIALIST')) DESC,b.characterID"))
        return; // retain the previous protection on a transient DB failure
    std::map<uint32, uint32> retainedPerCorp;
    DBResultRow row;
    while (members.GetRow(row)) {
        const uint32 characterID = row.GetUInt(0);
        const uint32 corporationID = row.GetUInt(1);
        if (!m_records.count(characterID))
            continue;
        // An online human requires every member; otherwise retain at least
        // three productive-role members while any human belongs to the corp.
        if (humanCorps.count(corporationID) || (row.GetBool(2) && retainedPerCorp[corporationID] < 3)) {
            required.insert(characterID);
            ++retainedPerCorp[corporationID];
        }
    }
    // The founder must be online for the normal corporation registry path to
    // process a human's pending application, even when the online cap is full.
    DBQueryResult applications;
    if (!sDatabase.RunQuery(applications,
        "SELECT DISTINCT corp.creatorID,a.characterID FROM crpApplications a "
        "JOIN crpCorporation corp ON corp.corporationID=a.corporationID "
        "JOIN botCharacters founder ON founder.characterID=corp.creatorID AND founder.enabled=1 "
        "LEFT JOIN botCharacters applicant ON applicant.characterID=a.characterID "
        "WHERE a.status=%u AND a.deleted=0 AND applicant.characterID IS NULL",
        Corp::AppStatus::AppliedByCharacter))
        return;
    DBResultRow application;
    while (applications.GetRow(application))
        if (m_records.count(application.GetUInt(0)) &&
            sEntityList.FindClientByCharID(application.GetUInt(1)) != nullptr)
            required.insert(application.GetUInt(0));

    m_corporationRequiredOnline.swap(required);
    // Rebuild only the front of the existing queue; all other startup and
    // manual spawn requests keep their original order.
    for (auto it = m_corporationRequiredOnline.rbegin(); it != m_corporationRequiredOnline.rend(); ++it) {
        if (m_clients.count(*it))
            continue;
        const auto retry = m_loginRetryAt.find(*it);
        if (retry != m_loginRetryAt.end() && GetFileTimeNow() < retry->second)
            continue;
        const auto queued = std::find(m_loginQueue.begin(), m_loginQueue.end(), *it);
        if (queued != m_loginQueue.end())
            m_loginQueue.erase(queued);
        m_pendingLogin.insert(*it);
        m_loginQueue.push_front(*it);
    }

    const size_t onlineLimit = std::min(sConfig.playerbots.maxBots, sConfig.playerbots.maxOnlineBots);
    if (m_clients.size() > onlineLimit) {
        // Overflow is permitted while players are present. Once nobody needs
        // a particular excess bot, retire at most one per refresh through the
        // existing persistent logout path.
        for (auto it = m_clients.rbegin(); it != m_clients.rend(); ++it) {
            if (m_corporationRequiredOnline.count(it->first))
                continue;
            std::string result;
            Dismiss(it->first, result);
            break;
        }
    }
}

void BotManager::ProcessBotCorporations()
{
    if (m_services == nullptr || m_records.empty())
        return;

    std::map<uint16, std::vector<uint32> > groups;
    for (const auto& entry : m_records) {
        if (entry.second.slot == 0)
            continue;
        const uint16 group = static_cast<uint16>(((entry.second.slot - 1) / 15) + 1);
        groups[group].push_back(entry.first);
    }
    if (groups.empty())
        return;

    std::map<uint16, std::vector<uint32> >::iterator group = groups.upper_bound(m_corporationCursor);
    if (group == groups.end())
        group = groups.begin();
    m_corporationCursor = group->first;
    if (group->second.size() < 10)
        return;

    std::sort(group->second.begin(), group->second.end(), [this](uint32 left, uint32 right) {
        const auto leftRecord = m_records.find(left);
        const auto rightRecord = m_records.find(right);
        if (leftRecord == m_records.end() || rightRecord == m_records.end())
            return left < right;
        return leftRecord->second.slot < rightRecord->second.slot;
    });

    const uint32 founderID = group->second.front();
    std::map<uint32, std::unique_ptr<BotClient> >::iterator founderIt = m_clients.find(founderID);
    if (founderIt == m_clients.end() || founderIt->second->GetChar().get() == nullptr)
        return;
    BotClient* founder = founderIt->second.get();

    uint32 corporationID = 0;
    DBQueryResult existingCorp;
    if (sDatabase.RunQuery(existingCorp,
        "SELECT corporationID FROM crpCorporation WHERE creatorID=%u ORDER BY corporationID DESC LIMIT 1",
        founderID)) {
        DBResultRow row;
        if (existingCorp.GetRow(row))
            corporationID = row.GetUInt(0);
    }

    CorporationDB corporationDB;
    if (corporationID == 0) {
        // A bot already invited to a player's corporation is free to accept the
        // human's order; it does not replace that corporation with a bot one.
        if (IsPlayerCorp(founder->GetCorporationID()) || !founder->IsDocked())
            return;

        const double registrationCost = sConfig.rates.corpCost;
        for (uint32 memberID : group->second) {
            if (founder->GetBalance(Account::CreditType::ISK) >= registrationCost)
                break;
            if (memberID == founderID)
                continue;
            std::map<uint32, std::unique_ptr<BotClient> >::iterator donorIt = m_clients.find(memberID);
            if (donorIt == m_clients.end())
                continue;
            BotClient* donor = donorIt->second.get();
            const double available = donor->GetBalance(Account::CreditType::ISK) - 1000000.0;
            if (available <= 0.0)
                continue;
            const double contribution = std::min<double>(
                std::min<double>(2000000.0, registrationCost - founder->GetBalance(Account::CreditType::ISK)),
                available);
            if (contribution <= 0.0)
                continue;

            try {
                AccountService::TransferFunds(
                    donor->GetCharacterID(), founderID, contribution,
                    "DESC: PlayerBot corporation startup contribution",
                    Journal::EntryType::PlayerDonation, founder->GetStationID(),
                    Account::KeyType::Cash, Account::KeyType::Cash, donor);
            } catch (...) {
                sLog.Warning("PlayerBot", "%s could not contribute to corporation startup.", donor->GetName());
            }
        }

        if (founder->GetBalance(Account::CreditType::ISK) < registrationCost)
            return;

        std::ostringstream name;
        name << "Forge " << (group->first % 2 == 0 ? "Industrial Cooperative " : "Frontier Logistics ")
             << std::setw(2) << std::setfill('0') << group->first;
        std::ostringstream ticker;
        ticker << "EV" << std::setw(2) << std::setfill('0') << group->first;
        std::string corpTicker = ticker.str();
        if (corporationDB.IsTickerTaken(corpTicker)) {
            ticker.str("");
            ticker.clear();
            ticker << "B" << std::setw(3) << std::setfill('0') << group->first;
            corpTicker = ticker.str();
        }

        if (!CorpRegistryBound::CreatePlayerBotCorporation(
                *m_services, corporationDB, *founder, name.str(), corpTicker, corporationID)) {
            sLog.Warning("PlayerBot", "Group %u could not register its corporation at %u ISK.",
                         group->first, static_cast<uint32>(registrationCost));
            return;
        }
        sLog.Green("PlayerBot", "%s founded %s [%s] with real wallet contributions.",
                   founder->GetName(), name.str().c_str(), corpTicker.c_str());
    }

    if (!IsPlayerCorp(corporationID))
        return;

    if (!CorpRegistryBound::EnsurePlayerBotCorporationLogo(*m_services, corporationID, founderID))
        sLog.Warning("PlayerBot", "Corporation %u could not load or assign its PlayerBot logo.", corporationID);

    LSCService* lsc = m_services->Lookup<LSCService>("LSC");
    uint16 joined = 0;
    for (uint32 memberID : group->second) {
        std::map<uint32, std::unique_ptr<BotClient> >::iterator memberIt = m_clients.find(memberID);
        if (memberIt == m_clients.end() || memberIt->second->GetChar().get() == nullptr ||
            memberIt->second->GetCorporationID() == corporationID)
            continue;
        // A player may have recruited this pilot into another corporation.
        // Automatic roster formation must not undo that membership.
        if (IsPlayerCorp(memberIt->second->GetCorporationID()))
            continue;
        if (corporationDB.GetCorpMemberCount(corporationID) >=
            corporationDB.GetCorpMemberLimit(corporationID))
            break;

        BotClient* member = memberIt->second.get();
        CorpData data = member->GetChar()->GetCorpData();
        data.corporationID = corporationID;
        CorporationDB::GetCorpData(data);
        data.baseID = data.corpHQ != 0 ? data.corpHQ : member->GetStationID();
        data.corpRole = Corp::Role::Member;
        data.rolesAtAll = Corp::Role::Member;
        data.rolesAtBase = Corp::Role::Member;
        data.rolesAtHQ = Corp::Role::Member;
        data.rolesAtOther = Corp::Role::Member;
        data.grantableRoles = Corp::Role::Member;
        data.grantableRolesAtBase = Corp::Role::Member;
        data.grantableRolesAtHQ = Corp::Role::Member;
        data.grantableRolesAtOther = Corp::Role::Member;
        member->GetChar()->JoinCorporation(data);
        if (lsc != nullptr)
            lsc->CharacterLogin(member);
        ++joined;
    }
    if (joined != 0)
        sLog.Green("PlayerBot", "Corporation %u accepted %u PlayerBot member(s).", corporationID, joined);

    // Revisit pending human applications as well as new ones. Only online
    // applicants are processed because the normal acceptance path updates the
    // live character session and employment history.
    if (founder->GetCorporationID() == corporationID) {
        DBQueryResult applications;
        if (sDatabase.RunQuery(applications,
            "SELECT a.characterID, a.applicationText FROM crpApplications a "
            "JOIN chrCharacters c ON c.characterID=a.characterID "
            "LEFT JOIN botCharacters b ON b.characterID=a.characterID "
            "WHERE a.corporationID=%u AND a.status=%u AND a.deleted=0 "
            "AND c.online=1 AND b.characterID IS NULL AND c.corporationID<>a.corporationID "
            "AND NOT EXISTS(SELECT 1 FROM crpCorporation oldCorp "
            "WHERE oldCorp.corporationID=c.corporationID AND oldCorp.ceoID=c.characterID) "
            "ORDER BY a.applicationID LIMIT 1",
            corporationID, Corp::AppStatus::AppliedByCharacter)) {
            DBResultRow application;
            if (applications.GetRow(application)) {
                Client* applicant = sEntityList.FindClientByCharID(application.GetUInt(0));
                CorpRegistryService* registry = m_services->Lookup<CorpRegistryService>("corpRegistry");
                if (applicant != nullptr && registry != nullptr)
                    registry->AcceptPlayerBotApplication(corporationID, *founder, *applicant,
                        application.GetText(1) == nullptr ? "" : application.GetText(1));
            }
        }
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
    for (auto& entry : m_clients)
        sEntityList.Remove(entry.second.get());
    m_clients.clear();
    m_loginQueue.clear();
    m_pendingLogin.clear();
    m_corporationRequiredOnline.clear();
    m_processRetryAt.clear();
    m_loginRetryAt.clear();
    m_records.clear();
    m_processCursor = 0;
    m_corporationCursor = 0;
    m_nextLoginAt = 0;
    m_nextCorporationProcessAt = 0;
    m_nextCorporationPresenceAt = 0;
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
        const uint16 onlineLimit = std::min(sConfig.playerbots.maxBots,
                                            sConfig.playerbots.maxOnlineBots);
        if (m_pendingLogin.find(entry.first) == m_pendingLogin.end() &&
            m_clients.size() + m_pendingLogin.size() >= onlineLimit)
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
    sEntityList.Remove(client->second.get());
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
    bool blockedBySecurity = false;
    for (auto& entry : m_brains) {
        if (botID != 0 && entry.first != botID)
            continue;
        std::map<uint32, std::unique_ptr<BotClient> >::iterator clientIt = m_clients.find(entry.first);
        if (clientIt == m_clients.end())
            continue;
        Client* client = clientIt->second.get();
        if (!client->IsInSpace() || client->SystemMgr() == nullptr)
            continue;
        SystemEntity* target = client->SystemMgr()->FindEntityByID(targetID);
        if (target == nullptr)
            continue;

        // This server build does not provide complete Crimewatch/CONCORD and
        // low-sec sentry consequences. Keep player-versus-player bot orders in
        // null security, where illegal high/low-sec aggression cannot be
        // silently treated as lawful. NPC combat is unaffected.
        if (target->GetPilot() != nullptr && client->SystemMgr()->GetSystemSecurityRating() > 0.0f) {
            blockedBySecurity = true;
            continue;
        }
        if (target->GetPilot() == client)
            continue;

        {
            entry.second->RequestAttack(targetID);
            ++ordered;
        }
    }
    result = ordered != 0 ? "Attack order sent to " + std::to_string(ordered) + " PlayerBot(s)." :
        (blockedBySecurity ?
            "Player targets are restricted to null security until the server applies the full Crimewatch and CONCORD response." :
            "No online PlayerBot has that target on grid.");
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
    if (sFltSvc.GetFleetLeader(fleetID) != &leader) {
        result = "Only the fleet commander can order PlayerBots into this fleet.";
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

bool BotManager::FleetJoin(uint32 fleetID, uint32 botID, std::string& result)
{
    Client* fleetLeader = sFltSvc.GetFleetLeader(fleetID);
    if (!IsFleetID(fleetID) || fleetLeader == nullptr) {
        result = "That fleet is not active on this server.";
        return false;
    }

    uint32 joined = 0;
    for (auto& entry : m_clients) {
        if (botID != 0 && entry.first != botID)
            continue;
        BotClient* bot = entry.second.get();
        if (bot->InFleet())
            continue;
        int32 wingID = 0, squadID = 0;
        sFltSvc.GetRandUnitIDs(fleetID, wingID, squadID);
        if (sFltSvc.AddMember(bot, fleetID, wingID, squadID, Fleet::Job::None,
                              Fleet::Role::Member, Fleet::Booster::No))
            ++joined;
    }
    result = joined == 0 ? "No eligible online PlayerBot joined that fleet." :
        "Accepted the fleet invitation for " + std::to_string(joined) + " PlayerBot(s).";
    return joined != 0;
}

bool BotManager::FleetLeave(uint32 botID, uint32 fleetID, std::string& result)
{
    uint32 left = 0;
    for (auto& entry : m_clients) {
        if (botID != 0 && entry.first != botID)
            continue;
        if (!entry.second->InFleet() || (fleetID != 0 && entry.second->GetFleetID() != fleetID))
            continue;
        sFltSvc.LeaveFleet(entry.second.get());
        ++left;
    }
    result = left == 0 ? "No selected online PlayerBot was in a fleet." :
        std::to_string(left) + " PlayerBot(s) left their fleet.";
    return left != 0;
}

bool BotManager::FleetOrder(Client& commander, const std::string& order, uint32 targetID,
                             std::string& result)
{
    const uint32 fleetID = commander.GetFleetID();
    if (!IsFleetID(fleetID) || sFltSvc.GetFleetLeader(fleetID) != &commander) {
        result = "You must be the active fleet commander to issue fleet-wide bot orders.";
        return false;
    }

    uint32 ordered = 0;
    for (auto& entry : m_brains) {
        std::map<uint32, std::unique_ptr<BotClient> >::iterator client = m_clients.find(entry.first);
        if (client == m_clients.end() || client->second->GetFleetID() != fleetID)
            continue;
        if (order == "follow" || order == "come")
            entry.second->SetManualFollow(commander);
        else if (order == "stay")
            entry.second->SetManualStay();
        else if (order == "dock")
            entry.second->RequestDock();
        else if (order == "mine")
            entry.second->RequestMine(targetID);
        else if (order == "attack") {
            std::string attackResult;
            if (!Attack(targetID, attackResult, entry.first)) {
                if (attackResult.find("restricted to null security") != std::string::npos)
                    result = attackResult;
                continue;
            }
        } else {
            result = "Fleet orders: follow, stay, dock, mine [asteroidTypeID], attack <entityID>.";
            return false;
        }
        ++ordered;
    }
    if (ordered != 0)
        result = "Fleet order " + order + " sent to " + std::to_string(ordered) + " PlayerBot(s).";
    else if (result.empty())
        result = "No online PlayerBots in your fleet could take that order.";
    return ordered != 0;
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
    if (m_services != nullptr) {
        LSCService* lsc = m_services->Lookup<LSCService>("LSC");
        if (lsc != nullptr)
            lsc->CharacterLogin(bot);
    }
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

bool BotManager::CreateCourierContract(uint32 botID, uint32 assigneeID, uint32 reward,
                                       uint32 destinationStationID, std::string& result)
{
    std::map<uint32, std::unique_ptr<BotClient> >::iterator it = m_clients.find(botID);
    if (it == m_clients.end() || it->second->GetChar().get() == nullptr) {
        result = "The contract issuer bot is not online.";
        return false;
    }
    BotClient& bot = *it->second;
    if (!bot.IsDocked() || bot.GetStationID() == 0 || bot.GetShip().get() == nullptr) {
        result = "The bot must be docked at a station to issue a contract.";
        return false;
    }
    ContractProxy* contractProxy = m_services == nullptr
        ? nullptr : m_services->Lookup<ContractProxy>("contractProxy");
    if (contractProxy == nullptr) {
        result = "The standard contract service is unavailable.";
        return false;
    }

    const uint32 stationID = bot.GetStationID();
    if (destinationStationID == 0) {
        DBQueryResult destinationResult;
        const uint32 regionID = sDataMgr.GetStationRegion(stationID);
        if (!sDatabase.RunQuery(destinationResult,
                "SELECT stationID FROM staStations WHERE regionID=%u AND stationID<>%u ORDER BY stationID LIMIT 1",
                regionID, stationID)) {
            result = "Could not select a courier destination station.";
            return false;
        }
        DBResultRow row;
        if (!destinationResult.GetRow(row)) {
            result = "No other station is available in the issuer's region.";
            return false;
        }
        destinationStationID = row.GetUInt(0);
    }
    if (!sDataMgr.IsStation(destinationStationID) || destinationStationID == stationID) {
        result = "The destination must be a different valid station.";
        return false;
    }

    const double cargoCapacity = bot.GetShip()->GetMyInventory()->GetCapacity(flagCargoHold);
    if (cargoCapacity <= 0.0) {
        result = "The issuer's ship has no courier cargo capacity.";
        return false;
    }

    StationItemRef station = sItemFactory.GetStationRef(stationID);
    if (station.get() == nullptr) {
        result = "The issuer's station inventory is unavailable.";
        return false;
    }
    std::vector<InventoryItemRef> hangarItems;
    station->GetMyInventory()->GetItemsByFlag(flagHangar, hangarItems);

    const double packageCapacity = std::min(cargoCapacity * 0.5, 20.0);
    for (const InventoryItemRef& item : hangarItems) {
        if (item.get() == nullptr || item->ownerID() != bot.GetCharacterID() ||
            item->locationID() != stationID || item->quantity() == 0 ||
            item->categoryID() != EVEDB::invCategories::Asteroid)
            continue;

        const double unitVolume = item->GetAttribute(AttrVolume).get_float();
        if (unitVolume <= 0.0)
            continue;
        const uint32 maxPackageQuantity = static_cast<uint32>(std::floor(packageCapacity / unitVolume));
        const uint32 quantity = std::min<uint32>(item->quantity(), maxPackageQuantity);
        if (quantity == 0 || quantity > static_cast<uint32>(std::numeric_limits<int32>::max()))
            continue;

        InventoryItemRef package = item;
        if (quantity < item->quantity()) {
            package = item->Split(static_cast<int32>(quantity));
            if (package.get() == nullptr)
                continue;
        }

        const ItemType* type = sItemFactory.GetType(package->typeID());
        if (type == nullptr)
            continue;
        const double collateralValue = std::ceil(type->basePrice() * quantity);
        const uint32 collateral = static_cast<uint32>(std::min<double>(
            std::max(0.0, collateralValue),
            static_cast<double>(std::numeric_limits<int32>::max())));
        uint32 contractID = 0;
        std::string error;
        if (!contractProxy->CreateBotCourierContract(bot, package->itemID(), quantity,
                assigneeID, reward, collateral, destinationStationID, contractID, error)) {
            result = "Courier contract creation failed: " + error;
            return false;
        }

        result = "Created courier contract " + std::to_string(contractID) + " from " +
            bot.GetName() + " with " + std::to_string(quantity) + " " + type->name() +
            " to station " + std::to_string(destinationStationID) + ".";
        return true;
    }

    result = "The bot needs an owned mineral stack small enough for its ship to issue a courier contract.";
    return false;
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
    } else if (command == "fleet invite" || command == "fleet join") {
        Fleet(speaker, result);
    } else if (command == "fleet leave") {
        if (IsFleetID(speaker.GetFleetID()))
            FleetLeave(0, speaker.GetFleetID(), result);
        else
            result = "You are not in a fleet; no bot fleet state was changed.";
    } else if (command.compare(0, 12, "fleet order ") == 0) {
        std::istringstream arguments(command.substr(12));
        std::string order;
        uint32 targetID = 0;
        arguments >> order;
        if (order == "attack" || order == "mine")
            arguments >> targetID;
        FleetOrder(speaker, order, targetID, result);
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
    const uint16 onlineLimit = std::min(sConfig.playerbots.maxBots,
                                        sConfig.playerbots.maxOnlineBots);
    stream << "PlayerBots online: " << m_clients.size() << "/" << onlineLimit
           << " (persistent records: " << m_records.size() << "/" << sConfig.playerbots.maxBots << ")<br>";
    for (const auto& entry : m_brains)
        stream << entry.first << " " << entry.second->GetStateName() << "<br>";
    return stream.str();
}

std::string BotManager::Locations() const
{
    struct SystemBotCount {
        uint32 total = 0;
        uint32 inSpace = 0;
        uint32 docked = 0;
    };
    std::map<uint32, SystemBotCount> counts;
    for (const auto& entry : m_clients) {
        BotClient* bot = entry.second.get();
        uint32 systemID = 0;
        if (bot->IsInSpace() && bot->SystemMgr() != nullptr) {
            systemID = bot->SystemMgr()->GetID();
        } else if (bot->GetStationID() > 0) {
            systemID = sDataMgr.GetStationSystem(static_cast<uint32>(bot->GetStationID()));
        }
        if (systemID == 0)
            continue;
        SystemBotCount& count = counts[systemID];
        ++count.total;
        if (bot->IsInSpace())
            ++count.inSpace;
        else
            ++count.docked;
    }

    if (counts.empty())
        return "Online PlayerBots have no resolved solar-system location.";

    std::ostringstream stream;
    stream << "Online PlayerBots by solar system:<br>";
    for (const auto& entry : counts) {
        SystemData system;
        const bool haveData = sDataMgr.GetSystemData(entry.first, system);
        const char* security = !haveData ? "unknown" :
            (system.securityRating >= 0.5f ? "highsec" :
             (system.securityRating > 0.0f ? "lowsec" : "nullsec"));
        stream << (haveData ? system.name : sDataMgr.GetSystemName(entry.first))
               << " (" << entry.first << ", " << security << "): " << entry.second.total
               << " [" << entry.second.inSpace << " in space, " << entry.second.docked << " docked]<br>";
    }
    stream << "Total: " << m_clients.size() << " online PlayerBot(s).";
    return stream.str();
}

Client* BotManager::Find(uint32 characterID) const
{
    std::map<uint32, std::unique_ptr<BotClient> >::const_iterator it = m_clients.find(characterID);
    return it == m_clients.end() ? nullptr : it->second.get();
}

} // namespace PlayerBot
