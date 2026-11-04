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
#include "character/Skill.h"
#include "corporation/CorporationDB.h"
#include "inventory/Inventory.h"
#include "inventory/ItemFactory.h"
#include "imageserver/ImageServer.h"
#include "station/StationDataMgr.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace {

using PlayerBot::Record;

const std::array<const char*, 6> kProfessions = {
    "MINER", "HAULER", "MISSION_RUNNER", "RAT_HUNTER", "TRADER", "INDUSTRIALIST"
};

uint16 BotSlotFromAccountName(const char* accountName)
{
    static const char prefix[] = "evemu_bot_";
    if (accountName == nullptr || std::strncmp(accountName, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    const unsigned long slot = std::strtoul(accountName + sizeof(prefix) - 1, nullptr, 10);
    return slot > 0 && slot <= UINT16_MAX ? static_cast<uint16>(slot) : 0;
}

bool EnsureBotPortraitImage(uint32 characterID, uint16 slot)
{
    if (characterID == 0 || slot == 0 || sConfig.playerbots.portraitDir.empty())
        return false;

    std::string category("Character");
    const std::string destination = sImageServer.GetFilePath(category, characterID, 512);
    {
        std::ifstream existing(destination.c_str(), std::ios::binary);
        if (existing.good())
            return true;
    }

    const uint16 portraitSlot = static_cast<uint16>(((slot - 1) % 150) + 1);
    std::ostringstream sourcePath;
    sourcePath << sConfig.playerbots.portraitDir << "/"
               << std::setw(3) << std::setfill('0') << portraitSlot << ".jpg";
    std::ifstream source(sourcePath.str().c_str(), std::ios::binary);
    if (!source.good())
        return false;

    std::ofstream target(destination.c_str(), std::ios::binary | std::ios::trunc);
    if (!target.good())
        return false;
    target << source.rdbuf();
    return target.good();
}

uint32 FindPortraitTemplateCharacter()
{
    // Reuse only the vanilla paper-doll schema of an existing player. Never
    // depend on a specific account name or copy that character's portrait JPEG.
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT c.characterID FROM chrCharacters c "
        "JOIN account a ON a.accountID=c.accountID "
        "JOIN chrPortraitData p ON p.charID=c.characterID "
        "JOIN avatars v ON v.charID=c.characterID "
        "JOIN avatar_colors ac ON ac.charID=c.characterID "
        "JOIN avatar_modifiers am ON am.charID=c.characterID "
        "JOIN avatar_sculpts ascp ON ascp.charID=c.characterID "
        "WHERE a.accountName NOT LIKE 'evemu\\_bot\\_%' "
        "ORDER BY c.characterID LIMIT 1"))
        return 0;

    DBResultRow row;
    if (result.GetRow(row))
        return row.GetUInt(0);

    // Keep provisioning portable if this is deployed to another local DB.
    if (!sDatabase.RunQuery(result,
        "SELECT p.charID FROM chrPortraitData p "
        "JOIN avatars v ON v.charID=p.charID "
        "JOIN avatar_colors c ON c.charID=p.charID "
        "JOIN avatar_modifiers m ON m.charID=p.charID "
        "JOIN avatar_sculpts s ON s.charID=p.charID "
        "GROUP BY p.charID ORDER BY p.charID LIMIT 1"))
        return 0;
    return result.GetRow(row) ? row.GetUInt(0) : 0;
}

bool EnsureBotPortraitMetadata(uint32 characterID, uint16 slot, uint32 templateCharacterID)
{
    if (characterID == 0 || slot == 0 || templateCharacterID == 0 ||
        characterID == templateCharacterID)
        return false;

    DBerror error;
    const double hairVariation = (static_cast<int>(slot % 11) - 5) * 0.035;
    const double colorVariation = (static_cast<int>(slot % 9) - 4) * 0.015;
    const double sculptX = (static_cast<int>(slot % 7) - 3) * 0.025;
    const double sculptY = (static_cast<int>(slot % 5) - 2) * 0.03;
    const double sculptZ = (static_cast<int>(slot % 3) - 1) * 0.035;

    DBQueryResult existing;
    if (!sDatabase.RunQuery(existing,
        "SELECT (SELECT COUNT(*) FROM avatars WHERE charID=%u), "
        "(SELECT COUNT(*) FROM avatar_colors WHERE charID=%u), "
        "(SELECT COUNT(*) FROM avatar_modifiers WHERE charID=%u), "
        "(SELECT COUNT(*) FROM avatar_sculpts WHERE charID=%u), "
        "(SELECT COUNT(*) FROM chrPortraitData WHERE charID=%u)",
        characterID, characterID, characterID, characterID, characterID))
        return false;
    DBResultRow counts;
    if (!existing.GetRow(counts))
        return false;

    // Vanilla character creation persists a complete paper-doll description
    // in these tables in addition to uploading the portrait JPEG. Copy the
    // local server's compatible resource IDs and vary only bounded appearance
    // weights so the client can also render the character customization view.
    // Check destination tables first instead of selecting from an INSERT target
    // in a NOT EXISTS clause; that form is rejected by some MariaDB versions.
    if (counts.GetUInt(0) == 0 && !sDatabase.RunQuery(error,
        "INSERT INTO avatars (charID, hairDarkness) "
        "SELECT %u, LEAST(1.0, GREATEST(0.0, hairDarkness + %f)) FROM avatars WHERE charID=%u",
        characterID, hairVariation, templateCharacterID))
        return false;

    if (counts.GetUInt(1) == 0 && !sDatabase.RunQuery(error,
        "INSERT INTO avatar_colors (charID, colorID, colorNameA, colorNameBC, weight, gloss) "
        "SELECT %u, colorID, colorNameA, colorNameBC, "
        "LEAST(1.0, GREATEST(0.0, weight + %f)), gloss FROM avatar_colors WHERE charID=%u",
        characterID, colorVariation, templateCharacterID))
        return false;

    if (counts.GetUInt(2) == 0 && !sDatabase.RunQuery(error,
        "INSERT INTO avatar_modifiers (charID, modifierLocationID, paperdollResourceID, paperdollResourceVariation) "
        "SELECT %u, modifierLocationID, paperdollResourceID, paperdollResourceVariation "
        "FROM avatar_modifiers WHERE charID=%u",
        characterID, templateCharacterID))
        return false;

    if (counts.GetUInt(3) == 0 && !sDatabase.RunQuery(error,
        "INSERT INTO avatar_sculpts (charID, sculptLocationID, weightUpDown, weightLeftRight, weightForwardBack) "
        "SELECT %u, sculptLocationID, "
        "LEAST(1.0, GREATEST(-1.0, weightUpDown + %f)), "
        "LEAST(1.0, GREATEST(-1.0, weightLeftRight + %f)), "
        "LEAST(1.0, GREATEST(-1.0, weightForwardBack + %f)) "
        "FROM avatar_sculpts WHERE charID=%u",
        characterID, sculptX, sculptY, sculptZ, templateCharacterID))
        return false;

    if (counts.GetUInt(4) == 0 && !sDatabase.RunQuery(error,
        "INSERT INTO chrPortraitData "
        "(charID, backgroundID, lightID, lightColorID, cameraX, cameraY, cameraZ, "
        "cameraPoiX, cameraPoiY, cameraPoiZ, headLookTargetX, headLookTargetY, headLookTargetZ, "
        "lightIntensity, headTilt, orientChar, browLeftCurl, browLeftTighten, browLeftUpDown, "
        "browRightCurl, browRightTighten, browRightUpDown, eyeClose, eyesLookVertical, "
        "eyesLookHorizontal, squintLeft, squintRight, jawSideways, jawUp, puckerLips, frownLeft, "
        "frownRight, smileLeft, smileRight, cameraFieldOfView, portraitPoseNumber) "
        "SELECT %u, backgroundID, lightID, lightColorID, cameraX, cameraY, cameraZ, "
        "cameraPoiX, cameraPoiY, cameraPoiZ, headLookTargetX, headLookTargetY, headLookTargetZ, "
        "lightIntensity, headTilt, orientChar, browLeftCurl, browLeftTighten, browLeftUpDown, "
        "browRightCurl, browRightTighten, browRightUpDown, eyeClose, eyesLookVertical, "
        "eyesLookHorizontal, squintLeft, squintRight, jawSideways, jawUp, puckerLips, frownLeft, "
        "frownRight, smileLeft, smileRight, cameraFieldOfView, portraitPoseNumber "
        "FROM chrPortraitData WHERE charID=%u "
        , characterID, templateCharacterID))
        return false;

    return true;
}

// The generated contact sheet is indexed by the stable bot slot. Keep the
// character metadata aligned with the portrait shown by the image server.
const std::array<uint8, 60> kPortraitGenders = {{
    0,1,0,1,0,1,0,1,0,1, 1,0,1,0,1,0,1,0,1,0,
    0,1,0,1,0,1,0,1,0,1, 0,1,1,0,1,0,1,0,0,1,
    1,0,1,0,1,0,0,1,0,1, 1,0,1,0,0,1,1,0,0,1
}};

const std::array<uint8, 12> kPlayableBloodlines = {{
    1, 2, 11, 3, 4, 14, 5, 6, 13, 7, 8, 12
}};

struct BotIdentity {
    uint8 bloodlineID = 1;
    uint8 raceID = 1;
    uint8 gender = 0;
    uint16 typeID = 0;
    uint32 ancestryID = 0;
    uint32 schoolID = 17;
    uint32 careerID = 0;
};

uint8 BotGender(uint32 ordinal)
{
    if (ordinal <= kPortraitGenders.size())
        return kPortraitGenders[ordinal - 1];
    // Portrait sheets for bot slots 61 onward alternate masculine/feminine.
    return ((ordinal - 61) % 2 == 0) ? 0 : 1;
}

uint8 BotBloodline(uint32 ordinal)
{
    const uint32 group = (ordinal - 1) / 6;
    const uint32 profession = (ordinal - 1) % 6;
    return kPlayableBloodlines[(group * 5 + profession * 7) % kPlayableBloodlines.size()];
}

const std::array<const char*, 15>& GivenNames(uint8 raceID, uint8 gender)
{
    static const std::array<const char*, 15> caldariMale = {{
        "Akio", "Daichi", "Eito", "Hiro", "Kaito", "Kenji", "Makoto", "Riku",
        "Shin", "Takeshi", "Yori", "Satoshi", "Jiro", "Ren", "Taro"
    }};
    static const std::array<const char*, 15> caldariFemale = {{
        "Aiko", "Akari", "Emi", "Hana", "Kaori", "Mika", "Nami", "Rina",
        "Saki", "Yuna", "Yuki", "Mei", "Aya", "Natsumi", "Nozomi"
    }};
    static const std::array<const char*, 15> minmatarMale = {{
        "Alek", "Bragi", "Dagr", "Einar", "Jarek", "Kari", "Marek", "Ragnar",
        "Rolf", "Sten", "Tyr", "Ulf", "Vidar", "Yngvar", "Orin"
    }};
    static const std::array<const char*, 15> minmatarFemale = {{
        "Asta", "Eira", "Freya", "Hilde", "Inga", "Kara", "Liv", "Mira",
        "Runa", "Svala", "Tova", "Ylva", "Brynja", "Nanna", "Signe"
    }};
    static const std::array<const char*, 15> amarrMale = {{
        "Amon", "Azar", "Darius", "Farid", "Hassan", "Jamal", "Khalid", "Malik",
        "Nadir", "Omar", "Rashad", "Samir", "Tarek", "Zahir", "Navid"
    }};
    static const std::array<const char*, 15> amarrFemale = {{
        "Amina", "Dalia", "Farah", "Imani", "Laleh", "Mariam", "Nadia", "Nura",
        "Parisa", "Roya", "Samira", "Soraya", "Yasmin", "Zara", "Nadira"
    }};
    static const std::array<const char*, 15> gallenteMale = {{
        "Adrien", "Bastien", "Cyril", "Dorian", "Etienne", "Felix", "Lucien", "Marcel",
        "Nicolas", "Remy", "Sebastien", "Theo", "Valentin", "Julien", "Luc"
    }};
    static const std::array<const char*, 15> gallenteFemale = {{
        "Amelie", "Camille", "Celeste", "Elise", "Juliette", "Leonie", "Mireille", "Noelle",
        "Odette", "Sabine", "Sylvie", "Vivienne", "Claire", "Delphine", "Isabelle"
    }};

    switch (raceID) {
        case 2: return gender == 0 ? minmatarMale : minmatarFemale;
        case 4: return gender == 0 ? amarrMale : amarrFemale;
        case 8: return gender == 0 ? gallenteMale : gallenteFemale;
        default: return gender == 0 ? caldariMale : caldariFemale;
    }
}

bool GetBotIdentity(uint32 ordinal, BotIdentity& identity)
{
    identity.bloodlineID = BotBloodline(ordinal);
    identity.gender = BotGender(ordinal);

    const CharacterType* charType = sItemFactory.GetCharacterTypeByBloodline(identity.bloodlineID);
    if (charType == nullptr)
        return false;
    identity.raceID = charType->race();

    uint32 schoolOffset = (ordinal / 6 + identity.bloodlineID) % 3;
    switch (identity.raceID) {
        case 1: identity.schoolID = 17 + schoolOffset; break;
        case 2: identity.schoolID = 14 + schoolOffset; break;
        case 4: identity.schoolID = 11 + schoolOffset; break;
        case 8: identity.schoolID = 20 + schoolOffset; break;
        default: return false;
    }

    uint8 schoolRace = 0;
    if (!CharacterDB::GetCareerBySchool(identity.schoolID,
                                        schoolRace, identity.careerID) || schoolRace != identity.raceID)
        return false;
    identity.typeID = charType->id();

    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT ancestryID FROM chrAncestries WHERE bloodlineID=%u ORDER BY ancestryID",
        identity.bloodlineID))
        return false;
    std::vector<uint32> ancestries;
    DBResultRow row;
    while (result.GetRow(row))
        ancestries.push_back(row.GetUInt(0));
    if (ancestries.empty())
        return false;

    uint32 bloodlineSlot = 0;
    for (uint32 candidate = 1; candidate < ordinal; ++candidate)
        if (BotBloodline(candidate) == identity.bloodlineID)
            ++bloodlineSlot;
    const uint32 ancestryIndex = bloodlineSlot % ancestries.size();
    identity.ancestryID = ancestries[ancestryIndex];
    return true;
}

uint32 FindCharacterByName(const std::string& name);

bool LoadBloodlineSurnames(uint8 bloodlineID, std::vector<std::string>& into)
{
    into.clear();
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT lastName FROM chrBloodlineNames WHERE bloodlineID=%u ORDER BY nameID",
        bloodlineID))
        return false;

    DBResultRow row;
    while (result.GetRow(row)) {
        const char* surname = row.GetText(0);
        if (surname != nullptr && surname[0] != '\0' && std::string(surname).length() <= 20)
            into.push_back(surname);
    }
    return !into.empty();
}

std::string GenerateBotName(uint32 ordinal, const BotIdentity& identity,
                            uint32 existingCharacterID,
                            const std::vector<std::string>& surnames)
{
    const std::array<const char*, 15>& givenNames = GivenNames(identity.raceID, identity.gender);
    const std::string givenName = givenNames[(ordinal - 1) % givenNames.size()];
    const size_t surnameStart = ((ordinal - 1) * 13U + identity.bloodlineID * 7U) % surnames.size();
    for (size_t offset = 0; offset < surnames.size(); ++offset) {
        const std::string candidate = givenName + " " + surnames[(surnameStart + offset) % surnames.size()];
        if (candidate.size() > 30)
            continue;
        const uint32 owner = FindCharacterByName(candidate);
        if (owner == 0 || owner == existingCharacterID)
            return candidate;
    }
    return std::string();
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

uint32 FindCharacterByAccount(uint32 accountID)
{
    DBQueryResult result;
    if (!sDatabase.RunQuery(result,
        "SELECT characterID FROM chrCharacters WHERE accountID=%u ORDER BY characterID LIMIT 1",
        accountID))
        return 0;
    DBResultRow row;
    return result.GetRow(row) ? row.GetUInt(0) : 0;
}

bool ApplyBotIdentity(uint32 accountID, uint32 characterID,
                      const BotIdentity& identity, const std::string& name)
{
    uint32 raceCorporationID = 0;
    if (!CorporationDB::GetCorporationBySchool(identity.schoolID, raceCorporationID))
        return false;

    DBQueryResult corporationResult;
    uint32 currentCorporationID = 0;
    if (!sDatabase.RunQuery(corporationResult,
        "SELECT corporationID FROM chrCharacters WHERE characterID=%u AND accountID=%u",
        characterID, accountID))
        return false;
    DBResultRow corporationRow;
    if (!corporationResult.GetRow(corporationRow))
        return false;
    currentCorporationID = corporationRow.GetUInt(0);

    std::string escapedName;
    sDatabase.DoEscapeString(escapedName, name);
    DBerror error;
    if (!sDatabase.RunQuery(error,
        "UPDATE chrCharacters SET characterName='%s', typeID=%u, gender=%u, bloodlineID=%u, "
        "raceID=%u, ancestryID=%u, schoolID=%u, careerID=%u, careerSpecialityID=%u "
        "WHERE characterID=%u AND accountID=%u",
        escapedName.c_str(), identity.typeID, identity.gender, identity.bloodlineID,
        identity.raceID, identity.ancestryID, identity.schoolID, identity.careerID,
        identity.careerID, characterID, accountID)) {
        sLog.Warning("PlayerBot", "Could not assign generated identity to bot %u: %s.",
                     characterID, error.c_str());
        return false;
    }

    // cacheOwners has no unique key in this schema. Update any old cache rows,
    // then insert a row only if this character has never been cached.
    if (!sDatabase.RunQuery(error,
        "UPDATE cacheOwners SET ownerName='%s', typeID=%u WHERE ownerID=%u",
        escapedName.c_str(), identity.typeID, characterID) ||
        !sDatabase.RunQuery(error,
        "INSERT INTO cacheOwners(ownerID, ownerName, typeID) "
        "SELECT %u, '%s', %u WHERE NOT EXISTS "
        "(SELECT 1 FROM cacheOwners WHERE ownerID=%u)",
        characterID, escapedName.c_str(), identity.typeID, characterID)) {
        sLog.Warning("PlayerBot", "Could not refresh owner-name cache for bot %u: %s.",
                     characterID, error.c_str());
    }

    // Rebalance only bots that are still in an NPC school corporation. If a
    // player has recruited a bot into their corporation, preserve that choice.
    if (IsNPCCorp(currentCorporationID) && currentCorporationID != raceCorporationID)
        CharacterDB::AddEmployment(characterID, raceCorporationID, currentCorporationID);
    return true;
}

uint32 ChooseStation(uint32 careerID)
{
    // PlayerBots use the seeded market hub as their initial trade base even
    // when their bloodline belongs to another empire. Their corporation and
    // faction identity still comes from the real school/career data.
    if (sConfig.playerbots.spawnStationID != 0 && sDataMgr.IsStation(sConfig.playerbots.spawnStationID))
        return sConfig.playerbots.spawnStationID;

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

uint32 RacialFrigateSkill(uint8 raceID)
{
    switch (raceID) {
        case 2: return EvESkill::MinmatarFrigate;
        case 4: return EvESkill::AmarrFrigate;
        case 8: return EvESkill::GallenteFrigate;
        default: return EvESkill::CaldariFrigate;
    }
}

uint32 RacialIndustrialSkill(uint8 raceID)
{
    switch (raceID) {
        case 2: return EvESkill::MinmatarIndustrial;
        case 4: return EvESkill::AmarrIndustrial;
        case 8: return EvESkill::GallenteIndustrial;
        default: return EvESkill::CaldariIndustrial;
    }
}

void AddProfileSkills(const std::string& profession, uint8 raceID,
                      bool corporationFounder,
                      std::map<uint32, uint8>& skills)
{
    // These are real typeIDs from EVE_Skills.h.  The base/race skills are
    // loaded first and profile values only raise their level.
    std::map<uint32, uint8> profile;
    profile[EvESkill::SpaceshipCommand] = 3;
    if (profession == "MINER") {
        profile = {{EvESkill::Mining, 4}, {EvESkill::Industry, 3}, {EvESkill::Refining, 3},
                   {EvESkill::RefineryEfficiency, 2}, {EvESkill::Astrogeology, 3},
                   {EvESkill::Survey, 3}, {EvESkill::MiningUpgrades, 2},
                   {EvESkill::MiningDroneOperation, 2}};
        profile[EvESkill::SpaceshipCommand] = 3;
        profile[RacialFrigateSkill(raceID)] = 3;
    } else if (profession == "HAULER") {
        profile = {{EvESkill::Navigation, 4}, {EvESkill::EvasiveManeuvering, 3},
                   {EvESkill::WarpDriveOperation, 3}, {EvESkill::Trade, 3},
                   {EvESkill::Accounting, 2}, {EvESkill::BrokerRelations, 2},
                   {EvESkill::HullUpgrades, 3}};
        profile[EvESkill::SpaceshipCommand] = 3;
        profile[RacialIndustrialSkill(raceID)] = 3;
    } else if (profession == "MISSION_RUNNER") {
        profile = {{EvESkill::Social, 4}, {EvESkill::Connections, 3},
                   {EvESkill::Negotiation, 3}, {EvESkill::Drones, 3},
                   {EvESkill::Gunnery, 4}, {EvESkill::Targeting, 3},
                   {EvESkill::Afterburner, 3}, {EvESkill::RepairSystems, 3},
                   {EvESkill::HullUpgrades, 3}};
        profile[EvESkill::SpaceshipCommand] = 3;
        profile[RacialFrigateSkill(raceID)] = 3;
        if (raceID == 1) {
            profile[EvESkill::MissileLauncherOperation] = 3;
            profile[EvESkill::LightMissiles] = 3;
        } else if (raceID == 2) {
            profile[EvESkill::SmallProjectileTurret] = 3;
        } else if (raceID == 4) {
            profile[EvESkill::SmallEnergyTurret] = 3;
        } else {
            profile[EvESkill::SmallHybridTurret] = 3;
        }
    } else if (profession == "RAT_HUNTER") {
        profile = {{EvESkill::Gunnery, 4},
                   {EvESkill::Drones, 4}, {EvESkill::DroneInterfacing, 3},
                   {EvESkill::Targeting, 3}, {EvESkill::ShieldManagement, 3},
                   {EvESkill::Navigation, 3}, {EvESkill::Afterburner, 3},
                   {EvESkill::RepairSystems, 3}, {EvESkill::HullUpgrades, 3}};
        profile[EvESkill::SpaceshipCommand] = 3;
        profile[RacialFrigateSkill(raceID)] = 3;
        if (raceID == 2)
            profile[EvESkill::SmallProjectileTurret] = 3;
        else if (raceID == 4)
            profile[EvESkill::SmallEnergyTurret] = 3;
        else
            profile[EvESkill::SmallHybridTurret] = 3;
    } else if (profession == "TRADER") {
        profile = {{EvESkill::Trade, 4}, {EvESkill::Accounting, 4},
                   {EvESkill::BrokerRelations, 4}, {EvESkill::Marketing, 3},
                   {EvESkill::Retail, 3}, {EvESkill::Procurement, 3},
                   {EvESkill::Wholesale, 2}, {EvESkill::Navigation, 4},
                   {EvESkill::Afterburner, 3}};
        profile[EvESkill::SpaceshipCommand] = 3;
        profile[RacialIndustrialSkill(raceID)] = 3;
    } else { // INDUSTRIALIST
        profile = {{EvESkill::Industry, 4}, {EvESkill::ProductionEfficiency, 4},
                   {EvESkill::MassProduction, 3}, {EvESkill::Science, 3},
                   {EvESkill::Research, 3}, {EvESkill::Metallurgy, 3},
                   {EvESkill::Refining, 3}, {EvESkill::Mining, 3},
                   {EvESkill::Navigation, 3}, {EvESkill::HullUpgrades, 3}};
        profile[EvESkill::SpaceshipCommand] = 3;
        profile[RacialIndustrialSkill(raceID)] = 3;
    }

    // Industrial-hull pilots can manage a colony alongside their primary
    // profession. These are real character skills, persisted by provisioning.
    if (profession == "HAULER" || profession == "TRADER" || profession == "INDUSTRIALIST") {
        profile[EvESkill::RemoteSensing] = 3;
        profile[EvESkill::Planetology] = 3;
        profile[EvESkill::CommandCenterUpgrades] = 3;
        profile[EvESkill::InterplanetaryConsolidation] = 2;
    }

    if (corporationFounder)
        profile[EvESkill::CorporationManagement] = 1;

    for (const auto& entry : profile)
        skills[entry.first] = std::max(skills[entry.first], entry.second);
}

bool EnsureProfileSkills(CharacterRef character, const std::string& profession,
                         uint8 raceID, uint16 slot, uint32& changed)
{
    changed = 0;
    if (character.get() == nullptr)
        return false;

    std::map<uint32, uint8> levels;
    if (!CharacterDB::GetBaseSkills(levels) || !CharacterDB::GetSkillsByRace(raceID, levels))
        return false;
    AddProfileSkills(profession, raceID, slot != 0 && ((slot - 1) % 15) == 0, levels);

    for (const auto& entry : levels) {
        if (entry.first > UINT16_MAX || sItemFactory.GetType(static_cast<uint16>(entry.first)) == nullptr)
            continue;

        SkillRef skill = character->GetCharSkillRef(static_cast<uint16>(entry.first));
        if (skill.get() == nullptr) {
            ItemData skillData(entry.first, character->itemID(), character->itemID(), flagSkill);
            skill = sItemFactory.SpawnSkill(skillData);
            if (skill.get() == nullptr)
                continue;
            character->AddItem(skill);
        }

        const uint8 targetLevel = std::max<uint8>(entry.second,
            static_cast<uint8>(skill->GetAttribute(AttrSkillLevel).get_uint32()));
        const uint32 targetPoints = skill->GetSPForLevel(targetLevel);
        if (targetLevel == skill->GetAttribute(AttrSkillLevel).get_uint32() &&
            targetPoints <= skill->GetAttribute(AttrSkillPoints).get_uint32())
            continue;

        skill->SetAttribute(AttrSkillLevel, targetLevel, false);
        skill->SetAttribute(AttrSkillPoints, targetPoints, false);
        skill->SaveItem();
        character->SaveSkillHistory(EvESkill::Event::SkillPointsApplied, GetFileTimeNow(),
                                    character->itemID(), entry.first, targetLevel, targetPoints);
        ++changed;
    }

    if (changed != 0)
        character->SaveCharacter();
    return true;
}

bool CreateCharacter(EVEServiceManager& services, uint32 accountID, const std::string& name,
                     const std::string& profession, uint16 slot, const BotIdentity& identity,
                     uint32& characterID, uint32& stationID)
{
    // Keep character type, race, school and career aligned with the selected
    // playable bloodline using this database's real Crucible career records.
    const CharacterType* charType = sItemFactory.GetCharacterTypeByBloodline(identity.bloodlineID);
    if (charType == nullptr || charType->id() != identity.typeID)
        return false;

    CharacterData cdata = CharacterData();
    cdata.accountID = accountID;
    cdata.gender = identity.gender;
    cdata.ancestryID = identity.ancestryID;
    cdata.bloodlineID = identity.bloodlineID;
    cdata.schoolID = identity.schoolID;
    cdata.description = "EVEMU PlayerBot created on ";
    cdata.description += currentDateTime();
    cdata.securityRating = sConfig.character.startSecRating;
    cdata.title = "No Title";
    cdata.createDateTime = static_cast<int64>(GetFileTimeNow());
    cdata.balance = static_cast<float>(std::min(sConfig.playerbots.bootstrapISK, sConfig.playerbots.maxISK));
    cdata.aurBalance = 0.0f;

    cdata.raceID = identity.raceID;
    cdata.careerID = identity.careerID;
    cdata.careerSpecialityID = cdata.careerID;

    // PlayerBots begin at the configured seeded trade hub so their first
    // purchases and real-market interactions have a liquid local market.
    // Their race, bloodline, school, career and NPC corporation remain tied
    // to their actual character identity; only their initial station differs.
    stationID = sConfig.playerbots.spawnStationID != 0
        ? sConfig.playerbots.spawnStationID : ChooseStation(cdata.careerID);
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
    // Keep PlayerBot NPC-corporation membership consistent with its actual
    // bloodline, regardless of the server-wide new-player corporation setting.
    if (!CorporationDB::GetCorporationBySchool(cdata.schoolID, corpData.corporationID))
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

    uint32 skillsChanged = 0;
    if (!EnsureProfileSkills(character, profession, identity.raceID, slot, skillsChanged)) {
        sItemFactory.UnsetUsingClient();
        character->LogOut();
        return false;
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

    const uint32 portraitTemplateID = FindPortraitTemplateCharacter();
    if (portraitTemplateID == 0)
        sLog.Warning("PlayerBot", "No vanilla avatar metadata template is available; portrait images will still be served.");
    bool portraitMetadataFailureLogged = false;

    // Provision by stable dedicated account slot. Character names are
    // generated EVE-style labels and may change when portraits/identities are
    // refreshed, so they must never be used as the account lookup key.
    for (uint32 ordinal = 1; ordinal <= maxBots; ++ordinal) {
        const std::string accountName = BotAccountName(ordinal);
        uint32 accountID = FindAccountByName(accountName);
        if (accountID == 0 && registered.size() < maxBots)
            accountID = ServiceDB::CreateBotAccount(accountName.c_str(), Acct::Role::PLAYER);
        if (accountID == 0)
            continue;

        BotIdentity identity;
        if (!GetBotIdentity(ordinal, identity)) {
            sLog.Warning("PlayerBot", "Unable to load playable identity data for bot slot %u.", ordinal);
            continue;
        }

        const std::string profession = kProfessions[(ordinal - 1) % kProfessions.size()];
        uint32 characterID = FindCharacterByAccount(accountID);
        uint32 stationID = 0;
        if (characterID == 0) {
            if (registered.size() >= maxBots)
                continue;

            std::vector<std::string> surnames;
            if (!LoadBloodlineSurnames(identity.bloodlineID, surnames)) {
                sLog.Error("PlayerBot", "No surname data for bloodline %u (slot %u).",
                           identity.bloodlineID, ordinal);
                continue;
            }
            const std::string name = GenerateBotName(ordinal, identity, 0, surnames);
            if (name.empty()) {
                sLog.Error("PlayerBot", "Unable to generate a unique character name for bot slot %u.", ordinal);
                continue;
            }
            if (!CreateCharacter(services, accountID, name, profession,
                                 static_cast<uint16>(ordinal), identity, characterID, stationID)) {
                sLog.Error("PlayerBot", "Unable to provision bot slot %u (%s).", ordinal, name.c_str());
                continue;
            }
        } else {
            std::vector<std::string> surnames;
            if (!LoadBloodlineSurnames(identity.bloodlineID, surnames)) {
                sLog.Error("PlayerBot", "No surname data for bloodline %u (slot %u).",
                           identity.bloodlineID, ordinal);
                continue;
            }
            const std::string name = GenerateBotName(ordinal, identity, characterID, surnames);
            if (name.empty() || !ApplyBotIdentity(accountID, characterID, identity, name)) {
                sLog.Error("PlayerBot", "Unable to refresh identity for dedicated bot slot %u.", ordinal);
                continue;
            }
            DBQueryResult characterResult;
            if (sDatabase.RunQuery(characterResult,
                "SELECT stationID FROM chrCharacters WHERE characterID=%u AND accountID=%u",
                characterID, accountID)) {
                DBResultRow characterRow;
                if (characterResult.GetRow(characterRow))
                    stationID = characterRow.GetUInt(0);
            }
        }

        CharacterRef character = sItemFactory.GetCharacterRef(characterID);
        uint32 skillsChanged = 0;
        if (character.get() == nullptr ||
            !EnsureProfileSkills(character, profession, identity.raceID,
                                 static_cast<uint16>(ordinal), skillsChanged)) {
            sLog.Warning("PlayerBot", "Could not ensure the race/profession skill plan for bot slot %u.", ordinal);
            continue;
        }
        if (skillsChanged != 0)
            sLog.Green("PlayerBot", "Updated %u persistent race/profession skills for bot %u.",
                       skillsChanged, characterID);

        if (!EnsureBotPortraitImage(characterID, static_cast<uint16>(ordinal)))
            sLog.Warning("PlayerBot", "No portrait image could be assigned to bot slot %u (%u).",
                         ordinal, characterID);
        if (portraitTemplateID != 0 &&
            !EnsureBotPortraitMetadata(characterID, static_cast<uint16>(ordinal), portraitTemplateID) &&
            !portraitMetadataFailureLogged) {
            sLog.Warning("PlayerBot", "Could not seed vanilla paper-doll metadata for the PlayerBots; check avatar table schema.");
            portraitMetadataFailureLogged = true;
        }

        if (registered.find(characterID) != registered.end())
            continue;
        if (registered.size() >= maxBots)
            continue;

        std::string escapedProfession, personality;
        sDatabase.DoEscapeString(escapedProfession, profession);
        personality = "Persistent " + profession + " pilot";
        sDatabase.DoEscapeString(personality, personality);
        DBerror error;
        if (!sDatabase.RunQuery(error,
            "INSERT IGNORE INTO botCharacters(characterID, enabled, profession, homeStationID, personality) "
            "VALUES(%u, 1, '%s', %u, '%s')", characterID, escapedProfession.c_str(),
            stationID, personality.c_str())) {
            sLog.Error("PlayerBot", "Provisioned character %u but failed to register bot state: %s.",
                       characterID, error.c_str());
            continue;
        }
        registered.insert(characterID);
        ++created;
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
        "b.currentBeltID, b.respawnAt, b.enabled, b.profession, b.state, b.personality, a.accountName "
        "FROM botCharacters b JOIN chrCharacters c ON c.characterID=b.characterID "
        "JOIN account a ON a.accountID=c.accountID "
        "WHERE b.enabled = 1 ORDER BY b.characterID")) {
        sLog.Warning("PlayerBot", "botCharacters is unavailable (%s). PlayerBots remain inactive.", result.error.c_str());
        return false;
    }

    DBResultRow row;
    while (result.GetRow(row)) {
        Record record;
        record.characterID = row.GetUInt(0);
        record.slot = BotSlotFromAccountName(row.GetText(8));
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
