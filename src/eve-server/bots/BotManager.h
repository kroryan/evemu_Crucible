#ifndef EVE_SERVER_BOTS_BOTMANAGER_H
#define EVE_SERVER_BOTS_BOTMANAGER_H

#include "utils/Singleton.h"
#include "bots/BotChatter.h"
#include "bots/BotDB.h"

#include <deque>
#include <map>
#include <set>

class Client;
class EVEServiceManager;

namespace PlayerBot {

class BotClient;
class BotBrain;

struct ChatReply {
    uint32 characterID = 0;
    std::string text;
};

class BotManager : public Singleton<BotManager> {
public:
    BotManager();
    ~BotManager();

    void Initialize(EVEServiceManager& services);
    void Process();
    void Shutdown();
    bool Spawn(const std::string& profession, uint32 count, std::string& result);
    bool Dismiss(uint32 characterID, std::string& result);
    void Follow(Client& leader, uint32 botID = 0);
    void Stay(uint32 botID = 0);
    void Mine(uint32 botID = 0, uint32 asteroidTypeID = 0);
    void Dock(uint32 botID = 0);
    bool Attack(uint32 targetID, std::string& result, uint32 botID = 0);
    bool Fleet(Client& leader, std::string& result);
    bool FleetJoin(uint32 fleetID, uint32 botID, std::string& result);
    bool FleetLeave(uint32 botID, uint32 fleetID, std::string& result);
    bool FleetOrder(Client& commander, const std::string& order, uint32 targetID,
                    std::string& result);
    bool CorpInvite(uint32 corporationID, uint32 botID, std::string& result);
    bool Give(uint32 typeID, uint32 quantity, uint32 botID, std::string& result);
    bool CreateCourierContract(uint32 botID, uint32 assigneeID, uint32 reward,
                               uint32 destinationStationID, std::string& result);
    std::vector<ChatReply> HandleChat(Client& speaker, const std::string& message);
    std::string Status() const;
    std::string Locations() const;
    Client* Find(uint32 characterID) const;

private:
    bool Login(Record& record);
    void RefreshCorporationPresence();
    void ProcessBotCorporations();
    void Save(Record& record);

    EVEServiceManager* m_services;
    bool m_initialized;
    uint32 m_processCursor;
    uint16 m_corporationCursor;
    uint64_t m_nextLoginAt;
    uint64_t m_nextCorporationProcessAt;
    uint64_t m_nextCorporationPresenceAt;
    std::deque<uint32> m_loginQueue;
    std::set<uint32> m_pendingLogin;
    std::set<uint32> m_corporationRequiredOnline;
    std::map<uint32, uint64_t> m_processRetryAt;
    std::map<uint32, uint64_t> m_loginRetryAt;
    std::map<uint32, Record> m_records;
    std::map<uint32, std::unique_ptr<BotClient> > m_clients;
    std::map<uint32, std::unique_ptr<BotBrain> > m_brains;
    BotChatter m_chatter;
};

} // namespace PlayerBot

#define sBotMgr ( PlayerBot::BotManager::get() )
#endif
