# PlayerBots

PlayerBots are opt-in virtual logins backed by real `Client`, `Character`,
`ShipItem`, inventory, market and destiny objects. They are not NPCs and do
not use a parallel economy. A bot therefore appears in the same EntityList,
bubble and local roster as a player and uses the normal mining module and
market settlement paths.

During virtual login, the server also joins bots to their system Local and
corporation/alliance channels through the normal `LSCChannel::JoinChannel`
path. Real clients make this subscription with `LSC.JoinChannels`; socketless
bots cannot issue that RPC themselves.

## Enablement

1. Apply `sql/migrations/20261103090000-playerbots.sql`.
2. Set `<playerbots><enabled>true</enabled>` in `config/eve-server.xml`.
   With `autoProvision=true` the manager creates up to `maxBots` dedicated
   `evemu_bot_NNN` accounts and `EVEMU Bot NNN` characters, with real skills,
   rookie ships, clone records and wallet balances. Provisioning is idempotent.
   Existing player characters are never repurposed. You can also insert a
   dedicated character into `botCharacters` manually.

`maxBots` defaults to 60. `cpuBudget` caps brains evaluated per one-Hz tick,
so a large population is spread over ticks rather than stalling the world.
Existing enabled bot records are queued for staggered virtual login at one bot
per second, and each brain applies a deterministic startup delay before its
first autonomous action. This avoids a 60-ship undock/warp burst on server boot.

## Real service lifecycle

`BotManager` loads persisted records and virtual-logs them in after static data
is ready. The miner undocks through `Client`, selects a real belt, warps using
`DestinyManager`, activates fitted mining lasers against `AsteroidSE` targets,
then warps home. Ore is moved using `Client::MoveItem` and sold only to an
existing `mktOrders` buy order through `MarketMgr::ExecuteBuyOrder`. If no buy
order exists, it remains in the bot's hangar. ISK, ore depletion, tax and item
ownership remain server-authoritative and persistent.

The existing `MarketBotMgr` remains untouched and can provide background buy
orders. No market reseeding or deletion is performed by PlayerBots.

`RAT_HUNTER` uses the same target manager and fitted weapon effects to select
NPCs on-grid, engage them, and return to its home station. `TRADER` periodically
settles a real sell order through `MarketMgr::ExecuteSellOrder`, subject to the
bot wallet. `HAULER`, `MISSION_RUNNER`, and `INDUSTRIALIST` currently use the
productive miner fallback until a route/agent/manufacturing job is available;
they never fabricate mission, PI, industry, or market outcomes.

## Operations

All bot control uses one documented slash command (PROGRAMMER role):

```
/bot status
/bot spawn miner <n>
/bot dismiss <characterID>
/bot follow | come | stay | dock | mine
/bot attack <target entityID>
/bot attack <target entityID> <botID>
/bot order <botID|all> <follow|stay|dock|mine|attack> [targetID]
/bot fleet
/bot corp invite [corporationID] [botID]
/bot give <typeID> <quantity> [botID]
/bot contract <botID> [assigneeID] [reward]
```

`spawn` activates registered, offline records. Provisioning creates only the
dedicated bot identities and grants the same starter inventory path used by
normal character creation; it does not alter player accounts or seed/delete
market orders.

`attack` uses the normal target-lock and fitted-module activation path; it is
restricted to targets already on the bot's grid. `fleet` creates the caller's
normal fleet when needed and joins all online bots through `FleetService`.
The `order` form can target one online bot by characterID or all online bots;
for `mine`, its optional `targetID` is an asteroid typeID.

## Channel control

Bots also inspect normal channel messages. Mention an online bot by name for a
short response, or use the independent PlayerBot prefix (default `!`) for
orders without colliding with the server's `.` slash-command prefix:

```
!follow
!stay
!dock
!mine
!attack <entityID>
!fleet
!status
```

Replies are emitted through the actual `LSCChannel` notification path. This
fallback is deterministic and works with no LLM configured.

## Extending professions

Add a `BotBrain` profession handler under `bots/professions/`; the handler must
drive public server services and `Client` methods, never write outcome rows
directly. Persist only intent/state in `botCharacters`. The migration and
configuration already provide profession-specific skills and personality data.
LLM chatter belongs in a separate asynchronous queue and must never run game
decisions or block the world tick. Any future agent, PI, industry, or route
planner should submit a real service job and let that service write the outcome.
