#ifndef EVE_SERVER_BOTS_BOTMANAGER_H
#define EVE_SERVER_BOTS_BOTMANAGER_H

#include "utils/Singleton.h"
#include "bots/BotChatter.h"
#include "bots/BotDB.h"

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
    void Follow(Client& leader);
    void Stay();
    void Mine();
    void Dock();
    bool Attack(uint32 targetID, std::string& result);
    bool Fleet(Client& leader, std::string& result);
    bool CorpInvite(uint32 corporationID, uint32 botID, std::string& result);
    bool Give(uint32 typeID, uint32 quantity, uint32 botID, std::string& result);
    bool CreateCourierContract(uint32 botID, uint32 assigneeID, uint32 reward, std::string& result);
    std::vector<ChatReply> HandleChat(Client& speaker, const std::string& message);
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
    BotChatter m_chatter;
};

} // namespace PlayerBot

#define sBotMgr ( PlayerBot::BotManager::get() )
#endif
