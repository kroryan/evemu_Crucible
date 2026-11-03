#ifndef EVE_SERVER_BOTS_BOTMANAGER_H
#define EVE_SERVER_BOTS_BOTMANAGER_H

#include "utils/Singleton.h"
#include "bots/BotDB.h"

class Client;
class EVEServiceManager;

namespace PlayerBot {

class BotClient;
class BotBrain;

class BotManager : public Singleton<BotManager> {
public:
    BotManager();
    ~BotManager();

    void Initialize(EVEServiceManager& services);
    void Process();
    void Shutdown();
    bool Spawn(const std::string& profession, uint32 count, std::string& result);
    bool Dismiss(uint32 characterID, std::string& result);
    void Follow(Client& leader);
    void Stay();
    void Mine();
    void Dock();
    bool Attack(uint32 targetID, std::string& result);
    bool Fleet(Client& leader, std::string& result);
    std::string Status() const;
    Client* Find(uint32 characterID) const;

private:
    bool Login(Record& record);
    void Save(Record& record);

    EVEServiceManager* m_services;
    bool m_initialized;
    std::map<uint32, Record> m_records;
    std::map<uint32, std::unique_ptr<BotClient> > m_clients;
    std::map<uint32, std::unique_ptr<BotBrain> > m_brains;
};

} // namespace PlayerBot

#define sBotMgr ( PlayerBot::BotManager::get() )
#endif
