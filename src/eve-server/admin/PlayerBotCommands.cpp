#include "eve-server.h"

#include "admin/AllCommands.h"
#include "admin/CommandDB.h"
#include "bots/BotManager.h"
#include "Client.h"

PyResult Command_bot(Client* pClient, CommandDB* db, EVEServiceManager& services, const Seperator& args)
{
    if (args.argCount() < 2)
        return new PyString("Usage: /bot spawn <profession> <n> | status | dismiss <characterID> | follow | come | stay | dock | mine | attack <entityID> | fleet | corp invite [corpID] [botID] | give <typeID> <quantity> [botID] | contract <botID> [assigneeID] [reward]");

    const std::string action = args.arg(1);
    std::string result;
    if (action == "status")
        return new PyString(sBotMgr.Status());
    if (action == "spawn") {
        if (args.argCount() != 4 || !args.isNumber(3))
            throw CustomError("Usage: /bot spawn miner <n>");
        sBotMgr.Spawn(args.arg(2), atoi(args.arg(3).c_str()), result);
        return new PyString(result);
    }
    if (action == "dismiss") {
        if (args.argCount() != 3 || !args.isNumber(2))
            throw CustomError("Usage: /bot dismiss <characterID>");
        sBotMgr.Dismiss(atoi(args.arg(2).c_str()), result);
        return new PyString(result);
    }
    if (action == "follow" || action == "come") {
        sBotMgr.Follow(*pClient);
        return new PyString("Online PlayerBots are following you.");
    }
    if (action == "stay") {
        sBotMgr.Stay();
        return new PyString("Online PlayerBots are holding position.");
    }
    if (action == "dock") {
        sBotMgr.Dock();
        return new PyString("Online PlayerBots were ordered home.");
    }
    if (action == "mine") {
        sBotMgr.Mine();
        return new PyString("Online PlayerBots were ordered to mine.");
    }
    if (action == "attack") {
        if (args.argCount() != 3 || !args.isNumber(2))
            throw CustomError("Usage: /bot attack <target entityID>");
        sBotMgr.Attack(atoi(args.arg(2).c_str()), result);
        return new PyString(result);
    }
    if (action == "fleet") {
        sBotMgr.Fleet(*pClient, result);
        return new PyString(result);
    }
    if (action == "corp") {
        if (args.argCount() < 3 || args.arg(2) != "invite")
            throw CustomError("Usage: /bot corp invite [corporationID] [botID]");
        uint32 corpID = pClient->GetCorporationID();
        uint32 botID = 0;
        if (args.argCount() >= 4) {
            if (!args.isNumber(3))
                throw CustomError("corporationID must be numeric");
            corpID = static_cast<uint32>(strtoul(args.arg(3).c_str(), nullptr, 10));
        }
        if (args.argCount() >= 5) {
            if (!args.isNumber(4))
                throw CustomError("botID must be numeric");
            botID = static_cast<uint32>(strtoul(args.arg(4).c_str(), nullptr, 10));
        }
        sBotMgr.CorpInvite(corpID, botID, result);
        return new PyString(result);
    }
    if (action == "give") {
        if (args.argCount() < 4 || args.argCount() > 5 || !args.isNumber(2) || !args.isNumber(3))
            throw CustomError("Usage: /bot give <typeID> <quantity> [botID]");
        uint32 botID = 0;
        if (args.argCount() == 5) {
            if (!args.isNumber(4))
                throw CustomError("botID must be numeric");
            botID = static_cast<uint32>(strtoul(args.arg(4).c_str(), nullptr, 10));
        }
        sBotMgr.Give(static_cast<uint32>(strtoul(args.arg(2).c_str(), nullptr, 10)),
                     static_cast<uint32>(strtoul(args.arg(3).c_str(), nullptr, 10)), botID, result);
        return new PyString(result);
    }
    if (action == "contract") {
        if (args.argCount() < 3 || args.argCount() > 5 || !args.isNumber(2))
            throw CustomError("Usage: /bot contract <botID> [assigneeID] [reward]");
        uint32 assigneeID = 0;
        uint32 reward = 0;
        if (args.argCount() >= 4) {
            if (!args.isNumber(3))
                throw CustomError("assigneeID must be numeric");
            assigneeID = static_cast<uint32>(strtoul(args.arg(3).c_str(), nullptr, 10));
        }
        if (args.argCount() == 5) {
            if (!args.isNumber(4))
                throw CustomError("reward must be numeric");
            reward = static_cast<uint32>(strtoul(args.arg(4).c_str(), nullptr, 10));
        }
        sBotMgr.CreateCourierContract(static_cast<uint32>(strtoul(args.arg(2).c_str(), nullptr, 10)),
                                      assigneeID, reward, result);
        return new PyString(result);
    }
    throw CustomError("Unknown /bot action.");
}
