# PlayerBots: Completion Plan

## Goal

Build a configurable population of up to 150 persistent player characters that
uses EVEmu's normal game systems. Mining, combat, industry, contracts, market,
inventory, ISK, losses, fleet membership and killmails must be server-owned
outcomes. The optional LLM writes chat only; it never chooses targets, routes,
prices, jobs or rewards.

This plan complements [PlayerBots.md](PlayerBots.md), which describes the
implemented foundation and available commands. A source change is not a live
feature until it has compiled, been deployed and been observed in-game and in
the database.

## Phase 1: Sustainable industry and market

This is the only active delivery phase. The requested market policy is a
single seeded Jita hub with lots of 10,000 units. NPC seed orders outside Jita
must be removed only after a recoverable local backup; player and PlayerBot
orders and escrow must be preserved. PlayerBots must not sell in Jita: they
should use it as a source when goods are unavailable elsewhere and carry goods
through real travel to supply other markets and player buy orders. The live
NPC orders outside Jita have been removed after a verified local backup and
the remaining legacy seed orders in Jita were set to 10,000 units each. The
new delivery logic is deployed but still needs an observed player-order fill.

1. Miners travel through gates, mine actual belt ore, and either sell it into
   real trader bids or reprocess it through the station reprocessing service.
2. Refined minerals are listed from the miner's actual hangar inventory.
3. Industrialists buy a real Miner I blueprint and quoted materials through
   existing market orders, pay normal installation costs, install a
   `ramJobs` manufacturing job, and complete it through `ramProxy`.
4. Traders bid on Miner I output and relist purchased modules through normal
   market services. Wallet limits, escrow, fees, taxes, inventory and order
   persistence remain authoritative.
5. Bots also try to fill player buy orders. Orders reachable by station,
   system, constellation or region are settled where the bot's stock is held.
   If a station-specific order is elsewhere, a trader can carry a bounded
   cargo load over accessible stargates and sell it through the same market
   settlement path.
6. Stargate autopilot warps land at the gate rather than the configured
   kilometre offset, so a pilot can issue its normal jump immediately.
7. Planetary Industry runs alongside other work: bots use real colonies,
   extraction and production, return to collect output, and transport and
   sell that output through normal game services.

**Phase 1 acceptance:** after deployment and restart, observe at least one
complete ore-to-minerals-to-manufacturing-to-market loop in authoritative
inventory, `ramJobs`, `mktOrders`, `mktTransactions` and wallet records. The
server must remain up while the 150 bot records log in and process. The local
mining/refining/manufacturing/sale loop has passed. The added remote market
delivery path still needs an observed fill of a player buy order outside Forge.
PI also requires a verified setup, production and collection cycle while the
pilot carries out another profession. Do not start the later command, corporation, mission, alliance or
sovereignty phases until that check passes and the phase is playable.

## Current state

- The live database has 150 dedicated bot records. The latest restart queued
  all 150 for staggered virtual login; all 150 were online before the latest
  market/industry hotfix. They are logging in again after that restart. No
  active courier offer has been observed, so mission behavior is not verified.
- PlayerBots use real `Client`, `Character`, `ShipItem`, inventory, mining,
  target-lock, market and fleet service paths. Miners mine live belts, rat
  hunters use fitted weapons against NPCs, and traders place character-owned
  orders with wallet escrow or inventory backing.
- The 150-character target, simultaneous-login cap, Forge spawn, identity
  generation, fleet commands and `/botlocali` are deployed. The startup log
  confirms 150 records were queued, but staggered login and profession behavior
  still need runtime verification before claiming 150 active pilots.
- The Server Panel capacity editor is being added for `maxBots`,
  `maxOnlineBots` and `cpuBudget`. Increasing the target should provision only
  missing dedicated bot accounts on restart. Lowering the online cap must not
  delete any accounts, characters, inventory, wallets or market orders.
- `HAULER` still uses the mining fallback. A complete Phase 1 cycle has been
  observed: actual mining and reprocessing, a completed `ramJobs` Miner I job,
  and an item sale to a trader's backed buy order. The latest hotfix makes
  industry buy the quote's full shortage, including waste, and makes the job
  validator count all material stacks. It also lets traders haul stock by
  real cargo and gate travel to station-specific bids. The updated
  cross-region route still needs a live external order to verify it.
- A MISSION_RUNNER courier loop is deployed but no offer or turn-in has been
  observed in the live database yet.
- Planetary Industry is part of Phase 1 and is not complete. Combat mission
  objectives, autonomous alliance creation, sovereignty capture and nullsec
  structure deployment are also not complete. The
  active DB lacks `qstEncounter`, so combat missions cannot be validated against
  its current data.
- The current source includes an initial hauler PI path: buy a command center,
  deploy and configure it using planetary services, launch extracted output,
  travel to its owned container and deliver it to a non-Jita station. The
  additive `20261104090000-playerbot-planetary-tasks.sql` migration persists
  pickup and delivery state. This code has compiled but has not yet been
  deployed or verified through a complete extraction and sale cycle. The
  existing server PI yield model remains simplified and factory processing
  still needs live verification.

## Compatibility and data rules

- Keep Crucible 7.31, build 360229 and MachoNet 320. Do not reintroduce client
  response formats that break login, character selection or navigation.
- Keep the local revert of PR #323. The upstream PR remains open because the
  current GitHub account cannot close it; do not merge it into this branch.
- Do not delete or recreate MariaDB, player accounts, market data or Docker
  volumes. PlayerBot migrations must be additive.
- Never run `clean_db.sql`, the region-wide `seed_market` script or database
  recreation scripts during bot deployment. Apply the requested Jita-only
  market transition with a recoverable backup and preserve player property.
- Bootstrap may provide a bounded initial wallet, skills and starter ship.
  After bootstrap, income and losses must come from real gameplay services and
  respect the configured wealth ceiling.
- Do not insert fabricated outcomes into market, kill, mission, manufacturing,
  PI, contract or sovereignty tables. Use the same service path as players.
- Do not order illegal highsec/lowsec PvP while this server lacks complete
  Crimewatch consequences. Security classification and PvP limitations are
  documented in `PlayerBots.md`.

## Milestones

The following later milestones are deliberately deferred until Phase 1 passes.

### 1. Capacity, identities and safe startup

- [x] Use dedicated bot accounts and persistent records; never repurpose player
  accounts.
- [x] Add fair round-robin brain scheduling and stagger virtual logins.
- [x] Separate the persistent provisioning target from the simultaneous online
  cap; set both source defaults to 150.
- [ ] Deploy and observe 150 logins in stages while measuring CPU, memory,
  database load and tick delay on the actual host.
- [ ] Generate and install a unique portrait for each configured bot and
  verify the Crucible image endpoint and character-selection portrait cache.
- [ ] Validate each race, bloodline, gender, ancestry, school, NPC corporation,
  portrait and name after a restart. Do not change an invited bot back to an
  NPC corporation on restart.
- [ ] Finish death/respawn using the regular clone, pod and rookie-ship paths;
  losses must not be reimbursed for free.
- [ ] Persist only state transitions or bounded periodic snapshots to reduce
  repeated DB writes.

**Acceptance:** raising the target creates only missing dedicated accounts;
lowering the online limit only prevents additional sessions; restarts preserve
characters, skills, ISK, ships, cargo and orders.

### 2. Commands, chat, corporations and fleets

- [x] Keep `/bot` restricted to the configured administrator/programmer role.
- [x] Add fleet invitation/join, leave and fleet-wide follow, stay, dock, mine
  and attack orders through `FleetService` and bot brains.
- [x] Add `/botlocali`, listing online bot counts by system, with docked/space
  breakdown and security classification; expose it through `/commandlist`.
- [ ] Add selection by named group, profession, corporation and fleet to every
  order; report progress and specific failure reasons.
- [ ] Complete ordinary player corporation applications and invitations using
  corporation permissions and membership services. Preserve voluntary player
  corp membership across restarts.
- [ ] Create player corporations of roughly 10–15 bots, distribute roles,
  accept eligible human applications automatically when configured, and let
  humans request CEO transfer from the bot CEO using `/transfercorp me`.
- [ ] Implement alliance creation, applications, acceptance and executor
  transfer only through the real alliance service, fees and corporation roles.
- [ ] Add targeted private messages, local/corp/fleet channel responses,
  mention resolution, cooldowns and anti-spam behavior.
- [ ] Keep `/commandlist` descriptions in sync with implemented commands.

**Acceptance:** clients see correct membership and leadership; the database
and service notifications agree; a bot cannot accept an invitation to an
unrelated fleet/corporation without an explicit authorized order.

### 3. Economy and services

- [x] Mine belts with real mining modules and preserve ore if no executable buy
  order exists.
- [x] Place trader buy/sell orders through `MarketProxyService`, with wallet
  escrow, fees and real inventory backing.
- [x] Create courier contracts through `ContractProxy` using bot-owned cargo.
- [x] Fill station, solar-system, constellation and region buy orders through
  real market settlement, excluding the seller's own orders.
- [x] Verify a miner fills a bot buy order and persist both sides of the
  transaction, wallet, tax and inventory records.
- [ ] Observe a trader transport stock to and fill a player buy order outside
  Forge using its cargo hold and highsec stargate route.
- [ ] Integrate `MarketBotMgr` as background market maker without replacing or
  reseeding its existing orders.
- [ ] Complete hauling with real contracts/cargo and stargate travel.
- [x] Complete refining through the real reprocessing service and costs.
- [x] Complete a Miner I manufacturing job using a real blueprint, skills,
  quoted materials, fees, output and completion service.
- [ ] Complete PI using command centers, links, routes, extractors and actual
  service jobs. Treat extraction and production as background activities: bots
  should pursue other work while jobs run, then return to collect, transport
  and sell their actual output when it is ready.
- [ ] Verify the level-1 courier loop uses persisted agent offers, real courier
  packages, station travel, turn-in rewards and standings after deployment.
  The brain calls shared `Agent` helpers and validates objectives through
  `Client::IsMissionComplete`.
- [ ] Add combat, mining and trade mission objectives only when their real
  service paths and data are available.
- [ ] Apply `maxISK` to rewards and transfers. Only bootstrap, bounties, mission
  rewards and settled sales may create bot wealth.

**Acceptance:** after restart, each item, mineral, ISK transfer, fee, tax,
contract, job, order, fill, kill and loss is visible in the authoritative game
records with its real owner.

### 4. Professions, factions and geography

- [ ] Complete MINER cycle handling for depleted rocks, cargo limits, station
  return, sell/refine decisions and ship replacement.
- [ ] Complete HAULER routes and delivery with real gates, cargo, risk and
  contracts.
- [ ] Complete RAT_HUNTER belt/anomaly selection, weapon cycles, bounties,
  loot, damage response, retreat and return.
- [ ] Deploy and verify MISSION_RUNNER level-1 courier offers, acceptance,
  real package pickup, gate travel, turn-in, rewards and standings.
- [ ] Add combat, mining and trade mission objectives; the current courier
  implementation does not cover those mission types.
- [ ] Complete INDUSTRIALIST production and research jobs using real
  blueprints and materials.
- [ ] Add EXPLORER and PIRATE/PvP only with actual probe/loot or combat mechanics
  and security-aware behavior. No fabricated scan results or killmails.
- [ ] Spread activity across the four empires, NPC factions, professions,
  regions, stations and security bands; avoid clustering all pilots in Jita.
- [ ] Use gate routes for expansion. Forge is the initial market hub, not a
  teleport destination for already-existing bots.
- [ ] Build corporation fleets and nullsec operations only after travel,
  alliance, fleet, structure and sovereignty service paths are verified.

**Acceptance:** profession and faction diversity is visible in characters and
behavior; system changes occur through valid gates; nullsec sovereignty changes
only after the normal structure mechanics succeed.

### 5. Security, PvP and sovereignty

- [x] Classify highsec (`security >= 0.5`), lowsec (`0 < security < 0.5`) and
  nullsec (`security <= 0`) from the server's map data.
- [x] Restrict bot-ordered player attacks to nullsec until full Crimewatch,
  CONCORD and lowsec sentry consequences are available.
- [ ] Audit and implement server-side criminal/suspect flags, kill rights,
  faction warfare permissions, CONCORD response and sentry gun behavior before
  enabling autonomous player piracy in high/low security.
- [ ] Make bots evaluate route risk, local population, standings, fleet support
  and security before travel or combat.
- [ ] Make sovereignty expansion deploy real TCUs, IHubs and related structures
  through their normal services, with fuel, vulnerability, timers and attack
  consequences.
- [ ] Verify alliance ownership, system maps and notifications after actual
  sovereignty capture; never create claims directly as a bot shortcut.

**Acceptance:** high/low/null decisions match server map/security data, illegal
actions have real game consequences, and sovereignty is the result of valid
structure gameplay.

### 6. Chat and optional LLM

- [x] Keep deterministic fallback replies when no LLM is configured.
- [x] Run HTTP chatter outside the world thread with a bounded queue.
- [ ] Add TLS certificate verification, bounded response size, robust JSON,
  request timeout, queue backpressure and safe shutdown.
- [ ] Enforce per-bot/per-channel cooldown and probability limits.
- [ ] Send only authorized context to the LLM; never claim an action unless the
  server confirmed it.
- [ ] Keep the LLM out of movement, targeting, pricing, profession, rewards and
  every other game decision.

**Acceptance:** a failed or disabled provider never blocks the game loop or
changes game outcomes.

### 7. Deploy and verify

- [ ] Compile C++17 and deploy only the EVEmu server container. Do not restart
  or recreate the database container or its volume.
- [ ] Validate Crucible 7.31 login, character list, selection, navigation
  loading, system entry, Local, Overview and clean logout after each deploy.
- [ ] Verify all 150 identities and portraits, online cap, per-system counts,
  corps, fleets, market orders, wallets, contracts, skills, ships and cargo.
- [ ] Measure server tick latency, CPU, memory, DB usage and login time at
  60/100/150 online bots on the actual machine before declaring 150 stable.
- [ ] Verify failure cases: no asteroid, no buy order, low wallet, destroyed
  hull, restart in warp, full fleet/corporation, invalid mission/blueprint,
  unavailable market and unauthorized high/low-sec target.
- [ ] Check container binary SHA against the built artifact; keep DB and market
  volumes mounted on internal storage.
- [ ] Push small reversible commits after compilation and source review.

## Work sequence

1. Finish and verify Phase 1's mining, real reprocessing, manufacturing,
   off-Jita sales, delivery to player buy orders, concurrent Planetary Industry
   and restart persistence.
2. Only after Phase 1 is playable, continue with capacity measurement and
   identity/portrait validation.
3. Finish chat, fleet, corporation, CEO transfer and alliance workflows.
4. Complete hauling, missions and the remaining professions one role at a
   time.
5. Add security-aware faction behavior, nullsec structures and sovereignty
   through their real services.
6. Validate chatter, schedules, anti-inflation and ongoing persistence.

Enable each profession only after its success and failure paths use real
services and pass the corresponding acceptance checks.
