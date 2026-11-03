#ifndef EVE_SERVER_BOTS_BOTCLIENT_H
#define EVE_SERVER_BOTS_BOTCLIENT_H

#include "Client.h"

namespace PlayerBot {

// This is intentionally a Client, not an NPC.  EntityList, LSC, corporations,
// killmails and market code therefore see the same character/session object as
// a connected player.  It differs only by having no TCP socket.
class BotClient : public Client {
public:
    explicit BotClient(EVEServiceManager& services) : Client(services) {}
    bool Login(uint32 characterID) {
        DBQueryResult result;
        if (!sDatabase.RunQuery(result, "SELECT accountID FROM chrCharacters WHERE characterID=%u", characterID))
            return false;
        DBResultRow row;
        if (!result.GetRow(row))
            return false;
        SetVirtualAccount(row.GetUInt(0));
        return SelectCharacter(characterID);
    }
};

} // namespace PlayerBot
#endif
