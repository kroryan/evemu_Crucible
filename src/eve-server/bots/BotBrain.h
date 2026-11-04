#ifndef EVE_SERVER_BOTS_BOTBRAIN_H
#define EVE_SERVER_BOTS_BOTBRAIN_H

#include "bots/BotDB.h"

class Client;
class SystemEntity;
class Colony;
class PlanetSE;

namespace PlayerBot {

class BotBrain {
public:
    enum class State { Docked, Undocking, WarpToBelt, Mining, Returning, Selling, Attacking, Traveling, Manual, Respawning };

    BotBrain(Client& client, Record& record);
    void Process();
    void SetManualFollow(Client& leader);
    void SetManualStay();
    void RequestDock();
    void RequestMine(uint32 asteroidTypeID = 0);
    void RequestAttack(uint32 targetID);
    const char* GetStateName() const;

private:
    void SetState(State state);
    void ProcessMiner();
    void ProcessRatHunter();
    void ProcessTrader(bool allowRemoteDelivery = true);
    void ProcessIndustrialist();
    void ProcessMissionRunner();
    bool PreparePlanetary();
    bool RelocateIndustrialBase();
    bool DeployPlanetary();
    bool ConfigurePlanetary(PlanetSE& planet, Colony& colony);
    bool ProcessPlanetaryPickup();
    bool ProcessPlanetarySale();
    bool SavePlanetaryTask();
    bool ProcessActiveIndustryJob();
    bool EnsureIndustryBlueprint(uint32 stationID, uint16 blueprintTypeID, uint32& blueprintItemID);
    bool BuyIndustryMaterial(uint32 stationID, uint16 typeID, uint32 quantity);
    bool PlaceIndustrySellOrder(uint32 stationID, uint16 productTypeID);
    bool SellTraderStockAtStation(uint32 stationID);
    bool StartRemoteMarketDelivery(uint32 stationID, uint32 itemID);
    bool SourcePlayerDemand(uint32 stationID);
    bool WarpToBelt();
    bool StartMining();
    void StopMining();
    void SellOre();
    void PlaceRefinedMineralOrders(uint32 stationID);
    void ProcessAttack();
    bool IsMiningShipReady() const;
    bool IsCombatShipReady() const;
    bool EnsureProfessionShip();
    bool SelectNpcTarget();
    bool ProcessBeltSystemTransit();
    bool SelectAlternateBelt();
    void ReturnToHome();
    bool ProcessHomeTransit();
    bool ProcessStationTransit(uint32 stationID);

    Client& m_client;
    Record& m_record;
    State m_state;
    uint64_t m_stateSince;
    uint64_t m_autoStartAt;
    uint32 m_targetID;
    uint32 m_requestedAsteroidTypeID;
    bool m_manualMining;
    bool m_returnWarpStarted;
    bool m_homeTransitComplete;
    uint32 m_missionAgentID;
    uint32 m_missionAgentStationID;
    uint32 m_transitStationID;
    uint64_t m_missionRetryAt;
    uint64_t m_lastTransitDiagAt;
    uint64_t m_lastMarketActionAt;
    uint64_t m_lastJitaOrderCheckAt;
    uint64_t m_lastPlanetaryCheckAt;
    uint64_t m_lastPlanetarySpaceCheckAt;
    uint32 m_planetaryPickupID;
    uint32 m_planetarySystemID;
    uint32 m_planetarySaleStationID;
    uint16 m_planetaryCargoTypeID;
    bool m_planetaryTaskStoreAvailable;
    uint32 m_demandOrderID;
    uint32 m_demandSourceStationID;
};

} // namespace PlayerBot
#endif
