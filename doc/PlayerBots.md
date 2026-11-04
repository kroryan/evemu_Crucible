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
   `evemu_bot_NNN` accounts and EVE-style characters with varied race, bloodline,
   gender, school, ancestry, names, skills, rookie ships, clone records and
   wallet balances. Provisioning is keyed by the dedicated account, not by the
   display name, so portrait/name refreshes cannot create duplicate characters.
   Existing player accounts are never selected or repurposed. You can also
   insert a dedicated character into `botCharacters` manually.

`maxBots` is the persistent account/character provisioning target (configured
to 150 on this server). `maxOnlineBots` normally caps simultaneous virtual
logins and can be lowered without deleting bot accounts or their assets. When
a human is online, all enabled bots in that character's corporation are queued
with priority and may temporarily exceed the cap. Corporations with a human
member retain up to three enabled bot members online even when all their humans
are offline, preferring mining, hauling and industry roles. A corporation with
fewer than three bot members cannot reach three without recruiting more bots.
Excess bots that are no longer required are logged out gradually when the cap
can be restored.
`cpuBudget` caps brains evaluated per one-Hz tick. Existing enabled records are
queued for staggered virtual login at one bot per second, and each brain applies
a deterministic startup delay before its first autonomous action. A 150-bot
login therefore takes at least 150 seconds; it does not cause a one-tick login
burst.

The EVEmu administrator panel exposes these three settings: total bot account
target, simultaneous online cap and brains processed per tick. Saved values
take effect on the next EVEmu restart. Increasing the account target creates
only missing dedicated accounts. Reducing the online cap does not delete bot
records or any persistent game data.

New bots of every player bloodline start at station `60003760` (Jita IV - Moon
4, The Forge) by default, where this server's NPC market is seeded. Their race,
bloodline, school, career and starting NPC corporation still follow the real
Crucible character data. The PlayerBot provisioner never clears, reseeds, or
edits existing market orders. Existing characters keep their persisted
location; the Forge setting never teleports an already-created bot.

Each dedicated bot slot is assigned one of 150 generated 512x512 pilot
portraits in `data/playerbot-portraits/`. Provisioning copies that JPEG to the
normal ImageServer cache path for the bot's character ID, so the client uses
the same portrait URL and cache as it does for player characters. Portrait
assignment is idempotent and does not touch human portraits. The accompanying
`avatars`, `avatar_colors`, `avatar_modifiers`, `avatar_sculpts`, and
`chrPortraitData` rows are initialized from a complete existing player avatar
record, with bounded per-slot color and sculpt variation; the player's JPEG is
never copied.

The ImageServer address in `<net><imageServer>` must be reachable from the game
client. `127.0.0.1` only works when the client itself runs on the server host;
for a client on the LAN, configure the server's LAN address and allow TCP port
26001 through the host firewall. Otherwise portraits can be stored and served
locally while remaining blank in the remote client.

At the first station visit, each role boards a persistent hull from this
instance's Crucible `invTypes` data. Mining, combat and industrial hulls vary by
the character's empire rather than assigning every bot the same ship:

| Empire | Miner | Combat frigate | T1 industrial |
| --- | --- | --- | --- |
| Caldari | Bantam (582) | Merlin (603); mission pilots use Kestrel (602) | Badger (648) |
| Minmatar | Burst (599) | Rifter (587) | Wreathe (653) |
| Amarr | Tormentor (591) | Punisher (597) | Bestower (1944) |
| Gallente | Navitas (592) | Incursus (594) | Iteron Mark III (655) |

The Minmatar mining hull is Burst; Probe (586) is an exploration frigate and is
not assigned to a mining profile. The role bootstrap replaces only recognized
character-creation starter hulls. It leaves a later ship the bot acquired for
itself alone, and keeps the replaced ship in the station hangar.

The loadouts use this instance's published Crucible types: Miner I (483), 1MN
Afterburner I (439), Expanded Cargohold I (1317), Small Armor Repairer I (523),
Standard Missile Launcher I (499), 150mm Light AutoCannon I (485), Gatling Pulse
Laser I (450), 75mm Gatling Rail I (561), and Light Electron Blaster I (562).
Combat weapons receive compatible small ammunition: Bloodclaw Light Missiles
(210), EMP S (185), Multifrequency S (246), or Antimatter Charge S (222), as
appropriate to the launcher/turret. The module charge groups, charge size,
weapon hardpoints, and hull slot counts were checked in the running database's
`invTypes`, `invGroups`, and `dgmTypeAttributes`; these are local typeIDs, not
IDs copied from a newer EVE SDE. Character skill plans now include the matching
racial frigate or industrial skill and the mining, turret, launcher, repair,
navigation, trade, or industry skills. The idempotent startup update raises
skills only on dedicated PlayerBot characters; it does not touch player
characters.

`RAT_HUNTER` fits a race-matched turret with ammunition. `MISSION_RUNNER` uses
the empire's cargo industrial. Source code now gives it a level-1 courier loop:
it requests a persisted agent offer, accepts the real courier package, travels
through gates, and asks the shared agent completion path to validate delivery
and issue normal rewards. This code compiled and is deployed; the live restart
has loaded 15 highsec level-1 distribution agents, but no courier offer or
turn-in has been observed yet. Combat, mining and trade mission types are not
implemented, and the active database lacks `qstEncounter`. Exploration has not
yet been added as a bot profession.

These first role hulls and fittings are one-time bootstrap assets. The next
economy phase is to have bots buy replacement hulls, modules, blueprints and
materials from NPC/player orders through the normal market settlement path;
the existing NPC-market importer's destructive cleanup is not used.

## Real service lifecycle

`BotManager` loads persisted records and virtual-logs them in after static data
is ready. The miner undocks through `Client`, selects a real belt, warps using
`DestinyManager`, activates fitted mining lasers against `AsteroidSE` targets,
then warps home. Ore is moved using `Client::MoveItem` and sold only to an
existing `mktOrders` buy order through `MarketMgr::ExecuteBuyOrder`. If no buy
order exists, it remains in the bot's hangar. ISK, ore depletion, tax and item
ownership remain server-authoritative and persistent.

Mining and rat-hunting brains do not require a belt in the current system. If
the system has none, they choose a reachable highsec belt system, spread pilots
among equally near destinations, and travel there through normal stargate
jumps. They also return to their home station through the same route path. This
matters for Forge: Jita has no asteroid belts, so miners must leave the hub
before they can mine. `HAULER` still uses this mining fallback.
When a belt has no suitable asteroid, miners select a reachable belt in a
different highsec system; rat hunters similarly move on when no target appears.
Rat hunters also prepare their fitted profession ship before leaving station.
While an industry job is blocked by missing inputs or an agent retry is pending,
the pilot can make local market orders using its own wallet and inventory. These
fallbacks are deployed, but they do not guarantee that every pilot can find a
profitable task or a reachable target.

The mining cohorts split between raw-ore sales and station reprocessing.
Reprocessing calls the existing `ReprocessingService`; refined minerals and
manufactured modules remain ordinary inventory. A live cycle has completed:
miners produced ore and minerals, industrialists bought an NPC Miner I BPO and
materials, installed and completed a real `ramJobs` job through `ramProxy`,
and a trader bought the resulting Miner I through a backed order. The item,
wallet movements and both sides of the sale were present in the normal market
and inventory records.

The manufacturing quote returns the actual missing-material map, including
character waste. Bot industry buys those shortages through existing sell
orders. The installation validator now totals all owned stacks and uses the
same skill/waste formula as the quote, so a second stack is counted correctly.
Repeated requirements of the same type are aggregated before inventory is
subtracted. Installation consumes from the owner and hangar specified by the
material path, using the same rounded requirements as the quote and validator.
The latest changes are deployed; new installations still need observation.

Buy-order matching now covers the order's station, solar system, constellation
or region, and excludes the seller's own order. This lets miners, refiners,
industrialists and traders fill real player bids anywhere those ranges reach.
When stock has no reachable bid, TRADER pilots can load it into their real
industrial hull and follow a highsec stargate route to a station with a backed
buy order, then settle the sale through `MarketMgr::ExecuteBuyOrder`. The
remote buyer's escrow is debited from the station where that order was issued;
the purchased item is delivered at the seller's station. Sales tax is debited
from the sale proceeds, so a character with an empty wallet can still complete
a valid sale without receiving minted ISK. This route code is deployed; a live
fill against a buy order outside Forge is still to be observed.

The client's stargate autopilot now warps to the gate at 0 m. This keeps the
ship directly inside the jump envelope; station and other autopilot warp
distances continue to use the configured world setting.

The existing `MarketBotMgr` remains untouched and can be configured to provide
background buy/sell orders. No market reseeding or deletion is performed by
PlayerBots.

The legacy `NPCMarket` XML importer is disabled during normal startup. It is not
a verified, ready-to-use dump of every NPC corporation's supply and demand; its
input is an optional hand-authored XML file. Do not invoke it against a live
seeded market: `CreateNPCMarketFromFile` first deletes `market_orders` rows with
duration greater than 90 days. The existing `MarketBotMgr` is a generated-order
service, not proof that the historical NPC-corporation supply/demand dataset is
present. PlayerBot setup leaves all such market data untouched.

Docker market seeding remains opt-in: `SEED_MARKET` defaults to `FALSE`, and
the running server is currently configured with `SEED_MARKET=FALSE`. This keeps
the existing seeded orders in the database while preventing a fresh deployment
from automatically seeding every configured region.

The configured opening hub is Forge for every bot, regardless of faction. A
new character is created there, while an existing bot travels from its saved
station through the real stargate network; the server never teleports an
existing character. TRADER pilots can now carry market stock to highsec
buy-order stations outside Forge without seeding those regions. Broader
profession and corporation expansion to Amarr, Dodixie, Rens, Hek and smaller
markets remains follow-up work.

When each 10–15 character roster group has an online founder docked at a station,
the manager can register a player corporation through the normal corporation
database, title, registration-fee, employment-history and chat-channel paths.
Startup contributions are real transfers from bot wallets and leave a reserve
with each contributor; no ISK is minted. If a group cannot meet the configured
registration fee, it waits rather than receiving a free corporation. Pending
online human applicants to bot-founded corporations are accepted through the
corporation registry. If the current CEO is a registered bot, the first accepted
human becomes CEO; an existing human CEO is not replaced. A character who is
already CEO of another player corporation must first resolve that role. This
workflow still requires end-to-end validation with a connected human client.
Bot corporations receive a deterministic EVE logo on creation. On startup, a
corporation created by a bot roster is checked and given a logo if it still has
the blank default; existing nonblank player logos are left alone.

`RAT_HUNTER` uses the same target manager and fitted weapon effects to select
NPCs on-grid, engage them, and return to its home station. `TRADER` places real
character buy and sell orders through `MarketProxyService::PlaceCharOrder`.
Buy escrow and fees use its wallet, sell orders consume ore from its hangar, and
miners can fill trader bids through `MarketMgr::ExecuteBuyOrder`. Each bot keeps
at most one order per item and side, caps bid escrow to a small share of its
wallet, and respects the configured wealth headroom when selling.

The configured fleet target is 150 dedicated bot accounts. Startup login is
staggered, and each profession brain is separately time-sliced. The miner to
trader buy-order fill and complete ore/refining/manufacturing/module-sale cycle
have been observed in the live database. After the latest market-range and
material-shortage fix, verify a player-owned buy order in another region to
observe the new highsec haul-and-fill path.

The new courier behavior does not complete other mission types or fabricate PI,
industry, contract, mission or market outcomes.

## Operations

All bot control uses the documented slash commands (ADMIN role or higher):

```
/bot status
/bot spawn miner <n>
/bot dismiss <characterID>
/bot follow | come | stay | dock | mine
/bot attack <target entityID>
/bot attack <target entityID> <botID>
/bot order <botID|all> <follow|stay|dock|mine|attack> [targetID]
/bot fleet [invite]
/bot fleet join <fleetID> [botID]
/bot fleet leave [botID]
/bot fleet order <follow|stay|dock|mine|attack> [targetID]
/bot corp invite [corporationID] [botID]
/bot give <typeID> <quantity> [botID]
/bot contract <botID> [assigneeID] [reward] [destinationStationID]
```

From a chat channel, slash commands accept both the documented `/command`
prefix and the legacy `.command` prefix. Their text result is shown to the
issuing character in an info dialog. `ADMIN` and `PROGRAMMER` accounts pass the
slash-service entry check; each command still enforces its own required role.

`spawn` activates registered, offline records. Provisioning creates only the
dedicated bot identities and grants the same starter inventory path used by
normal character creation; it does not alter player accounts or seed/delete
market orders.

`attack` uses the normal target-lock and fitted-module activation path; it is
restricted to targets already on the bot's grid. `fleet` creates the caller's
normal fleet when needed and adds online bots through `FleetService`. Only the
active fleet commander can invite bots into that fleet. `fleet join` accepts an
invitation into an already-active fleet through the same fleet membership
service; `fleet leave` removes the selected bots using `FleetService`. Fleet
orders apply only to bots in the caller's fleet. The standalone `order` form
can target one online bot by characterID or all online bots; for `mine`, its
optional `targetID` is an asteroid typeID.

Until this EVEmu branch applies the full Crimewatch consequences for illegal
aggression, bot-ordered attacks on player pilots are allowed only in nullsec.
Player-target attacks are blocked in highsec and lowsec; NPC combat is not
blocked by this policy. The bot AI and `/botlocali` classify space using the
server's actual security rating: highsec is 0.5 and above, lowsec is above 0
and below 0.5, and nullsec is 0 or below. This is a conservative bot policy,
not a claim that every CONCORD, criminal-flag, or sentry-gun rule is implemented
by the emulator. Nullsec PvP orders do not themselves create sovereignty.

`/botlocali` lists each solar system with online PlayerBots, the count in space
and docked, and its highsec/lowsec/nullsec classification. It is read-only and
is included by `/commandlist` for authorized roles.

`contract` selects a bot-owned mineral stack in its station hangar, splits it to
fit within the issuer's ship capacity, then calls the same
`ContractProxy::CreateContract` path as a client. It creates a courier package
with a destination station, real cargo ownership, collateral and optional
wallet-funded reward. If the destination is omitted, a different station in
the same region is selected.

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
!fleet invite
!fleet leave
!fleet order follow
!fleet order stay
!fleet order dock
!fleet order mine [asteroidTypeID]
!fleet order attack <entityID>
!status
```

Replies are emitted through the actual `LSCChannel` notification path. This
fallback is deterministic and works with no LLM configured.

`!fleet invite` creates a normal fleet if needed and accepts the online bots
into the caller's fleet when the caller is its commander. `!fleet leave` makes
the online bots leave the caller's fleet. Fleet orders are sent only to bot
members of the caller's fleet. The chat interface does not let a player add
bots to an unrelated fleet by typing an arbitrary fleet ID.

## Extending professions

Add a `BotBrain` profession handler under `bots/professions/`; the handler must
drive public server services and `Client` methods, never write outcome rows
directly. Persist only intent/state in `botCharacters`. The migration and
configuration already provide profession-specific skills and personality data.
LLM chatter belongs in a separate asynchronous queue and must never run game
decisions or block the world tick. Any future agent, PI, industry, or route
planner should submit a real service job and let that service write the outcome.
