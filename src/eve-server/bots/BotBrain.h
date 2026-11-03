#ifndef EVE_SERVER_BOTS_BOTBRAIN_H
#define EVE_SERVER_BOTS_BOTBRAIN_H

#include "bots/BotDB.h"

class Client;
class SystemEntity;

namespace PlayerBot {

class BotBrain {
public:
    enum class State { Docked, Undocking, WarpToBelt, Mining, Returning, Selling, Attacking, Manual, Respawning };

    BotBrain(Client& client, Record& record);
    void Process();
    void SetManualFollow(Client& leader);
    void SetManualStay();
    void RequestDock();
    void RequestMine();
    void RequestAttack(uint32 targetID);
    const char* GetStateName() const;

private:
    void SetState(State state);
    void ProcessMiner();
    void ProcessRatHunter();
    void ProcessTrader();
    bool WarpToBelt();
    bool StartMining();
    void StopMining();
    void SellOre();
    void ProcessAttack();
    bool IsMiningShipReady() const;
    bool IsCombatShipReady() const;
    bool SelectNpcTarget();
    void ReturnToHome();

    Client& m_client;
    Record& m_record;
    State m_state;
    uint64_t m_stateSince;
    uint32 m_targetID;
};

} // namespace PlayerBot
#endif
