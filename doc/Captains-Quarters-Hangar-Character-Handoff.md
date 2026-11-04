# Captain's Quarters, hangar and character handoff

Status: investigation in progress. The reported client remains usable in the
station, but entering Captain's Quarters leaves a `LOADING` display and the
player can leave that view. Walking in stations is **not verified as working**.
Do not advertise it as implemented or treat a successful character-creation
screen as a successful station-interior test.

## Environment and symptoms

- The target client is Crucible 7.31.360229. The player can log in, use the
  station and leave Captain's Quarters. Entering Captain's Quarters leaves a
  `LOADING` overlay while the client process remains alive. Verify the ship
  hangar and quarters transitions independently.
- The installed client's `start.ini` points at `192.168.1.158:26000` with
  premium edition. The extracted test client's `start.ini` had pointed at
  `127.0.0.1`; it now points at the SSH server and was restarted with
  `PROTON_USE_WINED3D=1`. Its login screen reports `192.168.1.158`, status
  `OK`, and zero players. It is currently stopped at the EULA; no agreement
  was accepted. Do not accept the EULA on the user's behalf.
- A direct launch temporarily selected Proton 7 for the installed prefix. It
  was migrated back using Proton Experimental 11's `wineboot -u`; the prefix
  compatibility version reads `11.0-100`. No account was logged in through
  that launch.
- The remote `evemu-server` container is running and `evemu-db` is healthy.
  The server restarted once at 10:12 UTC on 2026-11-04. Its logs since that
  restart contain bot activity but no character-creation, paper-doll, or
  world-space calls from the client. The current test cannot reach those
  calls until the EULA is accepted.
- The customization spinner occurs before `CreateCharacterWithDoll` is sent.
  Existing server traces show the two character-creation data requests
  succeeding without cache-query errors, followed by no request while the
  preview remains loading. A separate trial rendered the avatar under
  `PROTON_USE_WINED3D=1`, which makes a client renderer issue a strong lead;
  the server has not yet been cleared by a completed client-driven creation.
- A Windows minidump from a failed attempt reports an access violation while
  executing `_trinity_deploy.dll`, at module offset `0x512214`. The module
  list contains Wine components. This is evidence of where the client crashed,
  not proof that Wine, a graphic asset, a character model, or a server response
  caused it. The minidump alone does not contain a useful symbolized stack.
- The SSH server runs the same `8c8fe833` revision as this checkout and logs
  service calls. During a reproduced quarters entry, the client reached
  `UpdateCQMainScreen`; its news ticker then raised an SSL handshake error
  after a redirect to `www.eveonline.com`. The client continued running, so
  this is an observed error, not yet a proven cause of the stuck overlay.
- `Client::InitSession` and `Client::UpdateSession` both clear `shipid` while
  docked and set `worldspaceid` to the station ID. The current source matches
  the expected station session and should not be reverted without a new
  reproduction. The startup `GetCharSelectInfo` query returns `0` for
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
- `EveEntitySpawnClient` uses the client-only player spawner for these scenes.
  That spawner requests `paperdoll.GetMyPaperDollData(charID)` and
  `charMgr.GetPublicInfo(charID)` in parallel to build the avatar locally.
  Those are the server calls to trace once a character exists and the user can
  enter quarters.
- The Crucible client's `worldspaces` bulk cache (`2001700035`) contains four
  Establishment types (`32574`–`32577`) and four Captain's Quarters types:
  Amarr `32578`, Gallente `32579`, Minmatar `32580`, and Caldari `32581`.
  Each CQ entry is marked interior and unique and has a local safe position.
  Its `entitySpawns` cache (`2002200006`) contains 330–418 static records per
  race (Amarr 382, Gallente 330, Minmatar 418, Caldari 338). This confirms the
  client has the local scene data; those counts do not prove that runtime
  spawn locators, collisions, or walking work.
- `UpdateCQMainScreen` registers the holoscreen UI. Its RSS ticker fetches
  `eveonline.com` externally. The observed TLS error can affect that ticker;
  it is not evidence that the world-space scene failed. The same holoscreen
  service displays `loadingScreen.png` when `cqScreensEnabled` is false, so
  identify whether the reported `LOADING` is that screen or a full-screen
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

1. **Unblock a controlled client run.** Use a dedicated test account and a
   disposable Proton prefix. The installed client already points at the SSH
   server. Run customization once with `PROTON_USE_WINED3D=1`, finish creating
   a test character, and record whether the preview spinner clears. Do not
   alter existing accounts or reset the remote database.
2. **Separate the two loading displays.** Capture the exact screen at quarters
   entry and inspect `cqScreensEnabled`, the full-screen loading layer, and
   client stack traces. Compare hangar-to-quarters, quarters-to-hangar, and
   undock independently.
3. **Trace avatar spawn.** Correlate client and server timestamps for
   `GetMyPaperDollData`, `GetPublicInfo`, scene spawn, and avatar render. Fix
   server responses only if the request or returned data is wrong; otherwise
   keep the renderer workaround isolated to the client launch configuration.
4. **Verify every station race.** Check Caldari, Minmatar, Amarr, and Gallente
   station type selection, local world-space assets, spawn points, collision
   floors, pathing, holoscreens, and exit transitions. Record the first action
   and complete error stack for each failure.
5. **Implement non-station world spaces separately.** Capture a real
   `worldSpaceServer` request, determine how the server resolves its type ID
   and Macho address, then implement those exact mappings and register the
   service. Station CQ behavior must remain independent of this work.
6. **Deploy by verified milestone.** Build, exercise login/dock/hangar/CQ/exit/
   undock with the Crucible client, commit and push, then deploy to
   `krory90@192.168.1.158`. Preserve the live database and recheck its service
   health after deployment.

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
