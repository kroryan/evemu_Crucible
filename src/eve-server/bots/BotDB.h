/* Persistent PlayerBot configuration and brain state. */
#ifndef EVE_SERVER_BOTS_BOTDB_H
#define EVE_SERVER_BOTS_BOTDB_H

#include "eve-server.h"

class EVEServiceManager;

namespace PlayerBot {

struct Record {
    uint32 characterID = 0;
    uint16 slot = 0;
    uint32 homeStationID = 0;
    uint32 currentBeltID = 0;
    uint64_t respawnAt = 0;
    bool enabled = false;
    std::string profession;
    std::string state;
    std::string personality;
};

class BotDB {
public:
    static bool Load(std::vector<Record>& records);
    static bool SaveState(const Record& record);
    // Idempotently provisions dedicated bot accounts/characters and refreshes
    // their generated identity. Existing player accounts are never selected.
    static bool EnsureProvisioned(uint32 maxBots, EVEServiceManager& services, uint32& created);
};

} // namespace PlayerBot
#endif
