# Captain's Quarters, hangar and character handoff

Status: compared with the official Crucible server, this fork had changed
docked-session `shipid` handling. The original behavior is restored, built, and
deployed to `192.168.1.158:26000`. The user's active client remains on the
official comparison instance at `127.0.0.1:26002`; its next start is configured
for the patched SSH server. Client confirmation on that patched server is still
pending. Walking in stations is **not verified as working**.

## Environment and symptoms

- The target client is Crucible 7.31.360229. On the affected fork, the player
  could log in, use the station and leave Captain's Quarters, but entering the
  quarter stayed on `LOADING`. The same client has since loaded that quarter on
  the official comparison server. This points to a server regression; hangar
  and quarter transitions still need separate confirmation on the patched fork.
- The installed client's `start.ini` now points at `192.168.1.158:26000` with
  premium edition. The client process was left connected to the separate
  official comparison instance at `127.0.0.1:26002`; changing `start.ini` does
  not switch an already-running session. The new
  [Proton test launcher](../utils/dev/test_crucible_client.sh) checks the
  target and starts the installed `bin/ExeFile.exe` with Proton 7 + WineD3D in
  its own compatibility prefix. The installed binary reached the login screen,
  which reports `192.168.1.158`, status `OK`, and zero players. It showed the
  EULA; the test process exited without accepting it. Do not accept the EULA
  on the user's behalf.
- On 2026-11-04, an authenticated run against this fork reached character
  creation and called `CreateCharacterWithDoll`, then `SelectCharacterID`. The
  server returned the new character, but the client raised
  `ViewStateError: There is not a valid transition from charactercreation to
  charactercreation` from `GameUI::GoCharacterCreation`. The corresponding log
  contains `Session::LocationID == 0` before `SelectCharacterID`, localizing the
  regression to the character-creation/session handoff.
- The official Crucible source clears `shipid` in `InitSession` and
  `UpdateSession` for docked characters, and `SetShip` updates session `shipid`
  only in a solar system. This fork had diverged in both places: commit
  `cdbb21b4` made `SetShip` update it unconditionally, and commit `318e3496`
  added it to docked sessions. During character creation `m_locationID` is
  still zero, so the unconditional update dirtied the session and made the
  automatic RPC session notification emit the logged `LocationID == 0` change.
  Commit `8d463e2e` restores the official behavior in all three paths.
- The user verified that the official comparison instance enters the quarter.
  The corrected fork was rebuilt from commit `8d463e2e` and is online at
  `192.168.1.158:26000/26001`; the client has not yet logged into that patched
  instance. The launcher now targets it on the next start.
- Deployment initially exposed that the server's `.env` stores
  `EVEMU_DB_PASSWORD` while Compose did not pass it to the startup migration
  script. Commit `8c107170` passes that configured value to the server and to
  future database initialization. The currently running DB authenticated with
  that value and retains the existing `evemu_db` volume; no database data was
  reset.
- A direct launch temporarily selected Proton 7 for the installed prefix. It
  was migrated back using Proton Experimental 11's `wineboot -u`; the prefix
  compatibility version reads `11.0-100`. No account was logged in through
  that launch.
- The remote Compose project `evemu` is running its `server` and `db` services.
  The new server reports `EVEmu Server is Online`, uses the existing
  `evemu_db` volume, and listens on host ports `26000` and `26001`. No client
  calls to the patched server have been recorded since its deployment.
- The customization spinner occurs before `CreateCharacterWithDoll` is sent.
  On the local test server, four runs received only
  `GetCharCreationInfo` and `GetCharNewExtraCreationInfo`; no later character
  creation call arrived. The DXVK capture still showed the spinner after 30
  seconds. A Proton 7 + `PROTON_USE_WINED3D=1` capture showed the male avatar
  rendered on that same customization step after 20 seconds. This is a
  reproducible renderer workaround, not yet a completed remote-server creation
  test.
- A Windows minidump from a failed attempt reports an access violation while
  executing `_trinity_deploy.dll`, at module offset `0x512214`. The module
  list contains Wine components. This is evidence of where the client crashed,
  not proof that Wine, a graphic asset, a character model, or a server response
  caused it. The minidump alone does not contain a useful symbolized stack.
- The SSH checkout is now at `8c107170`. During an earlier quarters entry,
  the client reached `UpdateCQMainScreen`; its news ticker then raised an SSL
  handshake error after a redirect to `www.eveonline.com`. The client
  continued running, so this is an observed error, not a proven cause of a
  stuck scene.
- The startup `GetCharSelectInfo` query returns `0` for
  `worldSpaceID` so the selection screen does not claim the character is
  already walking in a station.
- A separate launch with Proton Experimental 11 ended in an access violation
  in `PhysXCore.dll`; Proton 7 reached the station UI. Do not conflate that
  renderer crash with the reproducible quarters-loading issue.
- A separate local-server character-creation run reached the Customization
  page, where the avatar preview stayed on its loading spinner. Restarting the
  client reset the wizard to Bloodline; advancing reproduced the spinner.
  `CreateCharacterWithDoll` was not called. The server returned
  `GetCharCreationInfo` and `GetCharNewExtraCreationInfo` without cache query
  errors, then received no further calls while the spinner remained. The local
  appearance tables are populated, and all 11 bloodline-to-appearance table
  references checked resolve. This points toward the client paper-doll/render
  path, but does not establish the cause. The latest clean-cache launch has
  not logged back in yet, so that trial remains incomplete.
- CCP's [Crucible patch notes](https://www.eveonline.com/news/view/patch-notes-for-crucible-1)
  confirm that the original game supported hangar and Captain's Quarters
  transitions, and that *Load Station Environments* controls availability.
  Original game support does not establish emulator support.

## Client and server boundary established from client scripts

- In `eveWorldSpaceClient.GetWorldSpaceTypeIDFromWorldSpaceID`, station IDs
  take a local branch: station type and race select one of the four built-in
  Captain's Quarters type IDs. `worldSpaceServer` is only called for a
  non-station world-space ID. The C++ placeholder's two `None` responses
  therefore do not explain a normal station CQ load by themselves.
- The client world-space service constructs a scene from its local
  `cfg.worldspaces` data, loads local path data, then uses a station spawn
  locator (bedroom, balcony, or new-character spawn). The station movement
  component disables movement broadcasts for client-side-only station scenes.
- `EveMovementClient` uses the client's `gameWorldClient` floor height and a
  local `GameWorld.PathToMode` for double-click movement. Since CQ scenes are
  client-side-only, their floors, pathing, collision, static props, and spawn
  locators come from the Crucible client assets; the server does not simulate
  the avatar's walking. The server supplies the station `worldspaceid` and
  character/avatar RPC data. A server-side scene or movement implementation
  would not repair a missing local navmesh or renderer issue.
- `EveEntitySpawnClient` uses the client-only player spawner for these scenes.
  That spawner requests `paperdoll.GetMyPaperDollData(charID)` and
  `charMgr.GetPublicInfo(charID)` in parallel to build the avatar locally.
  Those are the server calls to trace once a character exists and the user can
  enter quarters.
- The Crucible client's `worldspaces` bulk cache (`2001700035`) contains eight
  interior types. Four are Captain's Quarters: Amarr `32578`, Gallente `32579`,
  Minmatar `32580`, Caldari `32581`. The other four are unpublished
  Establishment types: Amarr `32574`, Caldari `32575`, Gallente `32576`, and
  Minmatar `32577`. The station client code selects the CQ IDs; it has no
  matching branch for the Establishments. Their `entitySpawns` rows exist
  (91, 20, 56, and 2 respectively), but the safe spot is `(0,0,0)` for
  Caldari and Minmatar Establishment. Treat these as legacy candidates until a
  client path and valid spawn are proven.
- CQ `entitySpawns` data has 330–418 static records per race (Amarr 382,
  Gallente 330, Minmatar 418, Caldari 338), with client-side records for local
  scene assets. This confirms local data is bundled; it does not prove that
  runtime spawn locators, collisions, or walking work.
- `UpdateCQMainScreen` registers the holoscreen UI. Its RSS ticker fetches
  `eveonline.com` externally. The observed TLS error can affect that ticker;
  it is not evidence that the world-space scene failed. The same holoscreen
  service displays `res:/UI/Texture/classes/CQLoadingScreen/loadingScreen.png`
  when the local preference `cqScreensEnabled` is false. The server registers
  `holoscreenMgr`; its empty news caches are handled by the client. Identify
  whether the reported `LOADING` is this monitor fallback or a full-screen
  transition before changing server code.
- `src/eve-server/system/WorldSpaceServer.cpp` still contains two placeholder
  methods returning `None`, and the service is not registered at startup. The
  [upstream Crucible implementation](https://github.com/EvEmu-Project/evemu_Crucible/blob/f55b488a006e3090470d3b481aee47a1d7a0dade/src/eve-server/system/WorldSpaceServer.cpp)
  is also a placeholder. Implement and register it only after an actual
  non-station world-space request and its type/address mapping are captured.
- The active ship and station data were checked for the affected player at the
  time of investigation. That is a point-in-time observation, not a guarantee
  for other characters or subsequent sessions.
- Character appearance records and portrait JPEGs are separate. PlayerBots
  currently use persistent avatar data and generated portrait files; those
  JPEGs are not proof that their 3D avatars can load or animate in quarters.
  See `doc/PlayerBots.md` for the current provisioning approach and limits.

## Action plan

1. **Verify the patched server with the client.** On the next connection, use
   `192.168.1.158:26000` and check character creation, selection, and the
   quarter transition. The current open client is still connected to the
   upstream comparison server and was left untouched.
2. **Verify character creation on the remote server.** Use a dedicated test
   account and correlate `GetCharCreationInfo`, `GetCharNewExtraCreationInfo`,
   `CreateCharacterWithDoll`, and the avatar tables. Verify appearance, sculpt,
   modifier, color, and portrait rows before entering a station.
3. **Prove the four active CQ paths.** For Amarr, Caldari, Gallente, and
   Minmatar, verify local scene load, avatar spawn, safe location, collision,
   walking, interaction objects, holoscreens, CQ-to-hangar exit, and undock.
   Distinguish the holoscreen's `loadingScreen.png` from a full-screen loading
   overlay in each capture.
4. **Resolve the four Establishments.** Find a real client transition or
   runtime scene request for IDs `32574`–`32577`; verify resource path, spawn
   locator, and navigation before deciding whether they remain reachable in
   Crucible. Do not mark them walkable from cache rows alone.
5. **Implement server world-space routing only for captured calls.** The
   current station path resolves locally. If a non-station ID reaches
   `worldSpaceServer`, capture the ID and expected type/address response, add
   the mapping and register the service, then test both entry and exit.
6. **Continue deployment verification.** The session fix is committed, pushed,
   and deployed to `krory90@192.168.1.158`; the DB volume and
   `data/market-backups/` are preserved. Confirm the client path first, then
   exercise all reachable interiors plus hangar and space transitions.

## Related stability and deployment notes

- A separate corporation tax crash was traced to `fcvt` being called with
  null output pointers in `ServiceDB::ProcessRealChange`; the conversion has
  been corrected. The server was rebuilt and deployed, but a complete
  client-driven tax-change regression test is still required.
- Use the existing MariaDB data and keep the configured market policy. Never
  reset the database or delete local backups while testing these transitions.
- A separate local Docker project has been initialized with a fresh database;
  its market seed remains disabled. It is isolated from the SSH server's
  persistent MariaDB and must not be used as a replacement for that data.
- The locally deployed server includes ongoing PlayerBot, market, industry
  and planetary work. Those systems have outstanding integration tests; see
  `doc/PlayerBots-ActionPlan.md`. A successful compilation or login is not a
  claim that the PlayerBot economy or planetary production loop is complete.
