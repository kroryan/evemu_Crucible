#include "eve-server.h"
#include "bots/BotDB.h"

#include "bots/BotClient.h"
#include "Client.h"
#include "EntityList.h"
#include "EVEServerConfig.h"
#include "POD_containers.h"
#include "ServiceDB.h"
#include "character/Character.h"
#include "character/CharacterDB.h"
#include "corporation/CorporationDB.h"
#include "inventory/Inventory.h"
#include "inventory/ItemFactory.h"
#include "station/StationDataMgr.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace {

using PlayerBot::Record;

const std::array<const char*, 6> kProfessions = {
    "MINER", "HAULER", "MISSION_RUNNER", "RAT_HUNTER", "TRADER", "INDUSTRIALIST"
};

std::string BotName(uint32 ordinal)
{
    char name[64];
    snprintf(name, sizeof(name), "EVEMU Bot %03u", ordinal);
    return name;
}

std::string BotAccountName(uint32 ordinal)
{
    char name[64];
    snprintf(name, sizeof(name), "evemu_bot_%03u", ordinal);
    return name;
}

uint32 FindCharacterByName(const std::string& name)
{
    std::string escaped;
    sDatabase.DoEscapeString(escaped, name);
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT characterID FROM chrCharacters WHERE characterName='%s' LIMIT 1", escaped.c_str()))
        return 0;
    DBResultRow row;
    return result.GetRow(row) ? row.GetUInt(0) : 0;
}

uint32 FindAccountByName(const std::string& name)
{
    std::string escaped;
    sDatabase.DoEscapeString(escaped, name);
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT accountID FROM account WHERE accountName='%s' LIMIT 1", escaped.c_str()))
        return 0;
    DBResultRow row;
    return result.GetRow(row) ? row.GetUInt(0) : 0;
}

uint32 ChooseStation(uint32 careerID)
{
    if (sConfig.character.startStation != 0 && sDataMgr.IsStation(sConfig.character.startStation))
        return sConfig.character.startStation;

    uint32 stationID = CharacterDB::GetStartingStationByCareer(careerID);
    if (stationID != 0 && sDataMgr.IsStation(stationID))
        return stationID;

    DBQueryResult result;
    if (sDatabase.RunQuery(result, "SELECT stationID FROM staStations ORDER BY stationID LIMIT 1")) {
        DBResultRow row;
        if (result.GetRow(row) && sDataMgr.IsStation(row.GetUInt(0)))
            return row.GetUInt(0);
    }
    return 0;
}

void AddProfileSkills(const std::string& profession, std::map<uint32, uint8>& skills)
{
    // These are real typeIDs from EVE_Skills.h.  The base/race skills are
    // loaded first and profile values only raise their level.
    std::map<uint32, uint8> profile;
    if (profession == "MINER") {
        profile = {{EvESkill::Mining, 4}, {EvESkill::Industry, 3}, {EvESkill::Refining, 3},
                   {EvESkill::RefineryEfficiency, 2}, {EvESkill::Astrogeology, 3},
                   {EvESkill::Survey, 3}, {EvESkill::MiningUpgrades, 2},
                   {EvESkill::MiningDroneOperation, 2}};
    } else if (profession == "HAULER") {
        profile = {{EvESkill::Navigation, 4}, {EvESkill::EvasiveManeuvering, 3},
                   {EvESkill::WarpDriveOperation, 3}, {EvESkill::Trade, 3},
                   {EvESkill::Accounting, 2}, {EvESkill::BrokerRelations, 2},
                   {EvESkill::HullUpgrades, 3}};
    } else if (profession == "MISSION_RUNNER") {
        profile = {{EvESkill::Social, 4}, {EvESkill::Connections, 3},
                   {EvESkill::Negotiation, 3}, {EvESkill::Drones, 3},
                   {EvESkill::Gunnery, 3}, {EvESkill::Targeting, 3},
                   {EvESkill::Afterburner, 3}};
    } else if (profession == "RAT_HUNTER") {
        profile = {{EvESkill::Gunnery, 4}, {EvESkill::SmallHybridTurret, 3},
                   {EvESkill::Drones, 4}, {EvESkill::DroneInterfacing, 3},
                   {EvESkill::Targeting, 3}, {EvESkill::ShieldManagement, 3},
                   {EvESkill::Navigation, 3}};
    } else if (profession == "TRADER") {
        profile = {{EvESkill::Trade, 4}, {EvESkill::Accounting, 4},
                   {EvESkill::BrokerRelations, 4}, {EvESkill::Marketing, 3},
                   {EvESkill::Retail, 3}, {EvESkill::Procurement, 3},
                   {EvESkill::Wholesale, 2}};
    } else { // INDUSTRIALIST
        profile = {{EvESkill::Industry, 4}, {EvESkill::ProductionEfficiency, 4},
                   {EvESkill::MassProduction, 3}, {EvESkill::Science, 3},
                   {EvESkill::Research, 3}, {EvESkill::Metallurgy, 3},
                   {EvESkill::Refining, 3}};
    }

    for (const auto& entry : profile)
        skills[entry.first] = std::max(skills[entry.first], entry.second);
}

bool CreateCharacter(EVEServiceManager& services, uint32 accountID, const std::string& name,
                     const std::string& profession, uint32& characterID, uint32& stationID)
{
    // State War Academy is present in the Crucible careers dataset and maps
    // to the Caldari bloodline used below.  Do not use school 1: it is not a
    // valid school/career pair in a stock EVEmu database.
    const uint8 bloodlineID = 1;
    const CharacterType* charType = sItemFactory.GetCharacterTypeByBloodline(bloodlineID);
    if (charType == nullptr)
        return false;

    CharacterData cdata = CharacterData();
    cdata.accountID = accountID;
    cdata.gender = 0;
    cdata.ancestryID = 1;
    cdata.bloodlineID = bloodlineID;
    cdata.schoolID = 17;
    cdata.description = "EVEMU PlayerBot created on ";
    cdata.description += currentDateTime();
    cdata.securityRating = sConfig.character.startSecRating;
    cdata.title = "No Title";
    cdata.createDateTime = static_cast<int64>(GetFileTimeNow());
    cdata.balance = static_cast<float>(std::min(sConfig.playerbots.bootstrapISK, sConfig.playerbots.maxISK));
    cdata.aurBalance = 0.0f;

    if (!CharacterDB::GetCareerBySchool(cdata.schoolID, cdata.raceID, cdata.careerID)) {
        cdata.raceID = 1;
        cdata.careerID = 11;
    }
    cdata.careerSpecialityID = cdata.careerID;

    stationID = ChooseStation(cdata.careerID);
    if (stationID == 0)
        return false;
    StationData stationData = StationData();
    if (!stDataMgr.GetStationData(stationID, stationData))
        return false;
    cdata.stationID = stationID;
    cdata.locationID = stationID;
    cdata.solarSystemID = stationData.systemID;
    cdata.constellationID = stationData.constellationID;
    cdata.regionID = stationData.regionID;
    cdata.typeID = charType->id();
    cdata.name = name;
    cdata.logonMinutes = 0;

    CorpData corpData = CorpData();
    corpData.startDateTime = cdata.createDateTime;
    corpData.corpRole = Corp::Role::Member;
    corpData.corpAccountKey = Account::KeyType::Cash;
    corpData.rolesAtAll = Corp::Role::Member;
    corpData.rolesAtBase = Corp::Role::Member;
    corpData.rolesAtHQ = Corp::Role::Member;
    corpData.rolesAtOther = Corp::Role::Member;
    corpData.grantableRoles = Corp::Role::Member;
    corpData.grantableRolesAtBase = Corp::Role::Member;
    corpData.grantableRolesAtHQ = Corp::Role::Member;
    corpData.grantableRolesAtOther = Corp::Role::Member;
    if (sConfig.character.startCorporation != 0 &&
        CorporationDB::DoesCorporationExist(sConfig.character.startCorporation))
        corpData.corporationID = sConfig.character.startCorporation;
    else if (!CorporationDB::GetCorporationBySchool(cdata.schoolID, corpData.corporationID))
        return false;
    corpData.baseID = stationID;

    PlayerBot::BotClient client(services);
    client.SetVirtualAccount(accountID);
    sItemFactory.SetUsingClient(&client);

    CharacterRef character = sItemFactory.SpawnCharacter(cdata, corpData);
    if (character.get() == nullptr) {
        sItemFactory.UnsetUsingClient();
        return false;
    }
    character->SetClient(&client);
    client.SetChar(character);

    uint8 intelligence = charType->intelligence();
    uint8 charisma = charType->charisma();
    uint8 perception = charType->perception();
    uint8 memory = charType->memory();
    uint8 willpower = charType->willpower();
    CharacterDB::GetAttributesFromAttributes(intelligence, charisma, perception, memory, willpower);
    const uint8 multiplier = sConfig.character.statMultiplier;
    character->SetAttribute(AttrIntelligence, intelligence * multiplier, false);
    character->SetAttribute(AttrCharisma, charisma * multiplier, false);
    character->SetAttribute(AttrPerception, perception * multiplier, false);
    character->SetAttribute(AttrMemory, memory * multiplier, false);
    character->SetAttribute(AttrWillpower, willpower * multiplier, false);

    std::map<uint32, uint8> skills;
    CharacterDB::GetBaseSkills(skills);
    CharacterDB::GetSkillsByRace(charType->race(), skills);
    AddProfileSkills(profession, skills);
    for (const auto& entry : skills) {
        if (sItemFactory.GetType(static_cast<uint16>(entry.first)) == nullptr)
            continue;
        ItemData skillData(entry.first, character->itemID(), character->itemID(), flagSkill);
        SkillRef skill = sItemFactory.SpawnSkill(skillData);
        if (skill.get() == nullptr)
            continue;
        skill->SetAttribute(AttrSkillLevel, entry.second, false);
        const uint32 skillPoints = skill->GetSPForLevel(entry.second);
        skill->SetAttribute(AttrSkillPoints, skillPoints, false);
        skill->SaveItem();
        character->SaveSkillHistory(EvESkill::Event::SkillPointsApplied, GetFileTimeNow(),
                                    character->itemID(), entry.first, entry.second, skillPoints);
        cdata.skillPoints += skillPoints;
    }

    ItemData cloneData(itemCloneAlpha, character->itemID(), stationID, flagClone, 1);
    InventoryItemRef clone = sItemFactory.SpawnItem(cloneData);
    if (clone.get() != nullptr)
        clone->SaveItem();

    client.CreateNewPod();
    client.GetPod()->Move(cdata.solarSystemID, flagCapsule, false);
    ShipItemRef rookie = client.SpawnNewRookieShip(stationID);
    if (rookie.get() == nullptr) {
        sItemFactory.UnsetUsingClient();
        character->LogOut();
        return false;
    }
    client.SetShip(rookie);
    character->SaveFullCharacter();
    CharacterDB::AddOwnerCache(character->itemID(), character->itemName(), charType->id());
    characterID = character->itemID();
    sItemFactory.UnsetUsingClient();

    // Run the regular login/logout lifecycle once.  This registers the
    // virtual pilot with EntityList/SystemBubble and flushes all item state,
    // then leaves it offline for BotManager to log in normally.
    if (!client.Login(characterID)) {
        character->LogOut();
        return false;
    }
    return true;
}

} // anonymous namespace

namespace PlayerBot {

bool BotDB::EnsureProvisioned(uint32 maxBots, EVEServiceManager& services, uint32& created)
{
    created = 0;
    if (!sConfig.playerbots.autoProvision || maxBots == 0)
        return true;

    DBQueryResult result;
    if (!sDatabase.RunQuery(result, "SELECT characterID FROM botCharacters")) {
        sLog.Warning("PlayerBot", "Cannot provision bots: botCharacters is unavailable (%s).", result.error.c_str());
        return false;
    }

    std::set<uint32> registered;
    DBResultRow row;
    while (result.GetRow(row))
        registered.insert(row.GetUInt(0));

    uint32 ordinal = 1;
    uint32 attempts = 0;
    while (registered.size() < maxBots && attempts++ < maxBots) {
        const std::string name = BotName(ordinal);
        const std::string profession = kProfessions[(ordinal - 1) % kProfessions.size()];
        const uint32 existingCharacter = FindCharacterByName(name);
        if (existingCharacter != 0) {
            if (registered.find(existingCharacter) == registered.end()) {
                std::string escapedProfession, personality;
                sDatabase.DoEscapeString(escapedProfession, profession);
                personality = "Persistent " + profession + " pilot";
                sDatabase.DoEscapeString(personality, personality);
                DBerror error;
                sDatabase.RunQuery(error,
                    "INSERT IGNORE INTO botCharacters(characterID, enabled, profession, personality) "
                    "VALUES(%u, 1, '%s', '%s')", existingCharacter,
                    escapedProfession.c_str(), personality.c_str());
                registered.insert(existingCharacter);
            }
            ++ordinal;
            continue;
        }

        const std::string accountName = BotAccountName(ordinal);
        uint32 accountID = FindAccountByName(accountName);
        if (accountID == 0)
            accountID = ServiceDB::CreateBotAccount(accountName.c_str(), Acct::Role::PLAYER);
        if (accountID == 0) {
            sLog.Error("PlayerBot", "Unable to create account %s; retrying next slot.", accountName.c_str());
            ++ordinal;
            continue;
        }

        uint32 characterID = 0, stationID = 0;
        if (!CreateCharacter(services, accountID, name, profession, characterID, stationID)) {
            sLog.Error("PlayerBot", "Unable to provision %s; no player data was modified.", name.c_str());
            ++ordinal;
            continue;
        }

        std::string escapedProfession, personality;
        sDatabase.DoEscapeString(escapedProfession, profession);
        personality = "Persistent " + profession + " pilot";
        sDatabase.DoEscapeString(personality, personality);
        DBerror error;
        if (!sDatabase.RunQuery(error,
            "INSERT INTO botCharacters(characterID, enabled, profession, homeStationID, personality) "
            "VALUES(%u, 1, '%s', %u, '%s')", characterID, escapedProfession.c_str(),
            stationID, personality.c_str())) {
            sLog.Error("PlayerBot", "Provisioned character %u but failed to register bot state: %s.",
                       characterID, error.c_str());
            ++ordinal;
            continue;
        }
        registered.insert(characterID);
        ++created;
        ++ordinal;
    }

    if (registered.size() < maxBots)
        sLog.Warning("PlayerBot", "Provisioned %zu/%u requested bot records.", registered.size(), maxBots);
    else
        sLog.Green("PlayerBot", "Provisioning complete: %u new bot(s), %zu total.", created, registered.size());
    return true;
}

bool BotDB::Load(std::vector<Record>& records)
{
    records.clear();
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT b.characterID, "
        "COALESCE(NULLIF(b.homeStationID, 0), c.stationID), "
        "b.currentBeltID, b.respawnAt, b.enabled, b.profession, b.state, b.personality "
        "FROM botCharacters b JOIN chrCharacters c ON c.characterID=b.characterID "
        "WHERE b.enabled = 1 ORDER BY b.characterID")) {
        sLog.Warning("PlayerBot", "botCharacters is unavailable (%s). PlayerBots remain inactive.", result.error.c_str());
        return false;
    }

    DBResultRow row;
    while (result.GetRow(row)) {
        Record record;
        record.characterID = row.GetUInt(0);
        record.homeStationID = row.GetUInt(1);
        record.currentBeltID = row.GetUInt(2);
        record.respawnAt = static_cast<uint64_t>(row.GetInt64(3));
        record.enabled = row.GetBool(4);
        record.profession = row.GetText(5) == nullptr ? "MINER" : row.GetText(5);
        record.state = row.GetText(6) == nullptr ? "DOCKED" : row.GetText(6);
        record.personality = row.GetText(7) == nullptr ? "" : row.GetText(7);
        records.push_back(record);
    }
    return true;
}

bool BotDB::SaveState(const Record& record)
{
    DBerror error;
    std::string state, profession;
    sDatabase.DoEscapeString(state, record.state);
    sDatabase.DoEscapeString(profession, record.profession);
    return sDatabase.RunQuery(error,
        "UPDATE botCharacters SET profession='%s', state='%s', homeStationID=%u, currentBeltID=%u, respawnAt=%" PRIu64 " WHERE characterID=%u",
        profession.c_str(), state.c_str(), record.homeStationID, record.currentBeltID, record.respawnAt, record.characterID);
}

} // namespace PlayerBot
