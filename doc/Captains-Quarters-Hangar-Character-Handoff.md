# Captain's Quarters, hangar and character handoff

Status: investigation in progress. The client remains usable in the station,
but entering Captain's Quarters leaves a `LOADING` overlay; the player can
leave that view. Walking in stations is **not verified as working**. Do not
advertise it as implemented or treat a successful character creation screen as
a successful station-interior test.

## Environment and symptoms

- The target client is Crucible 7.31.360229. The player can log in, use the
  station and leave Captain's Quarters. Entering Captain's Quarters leaves a
  `LOADING` overlay while the client process remains alive. Verify the ship
  hangar and quarters transitions independently.
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
- The same session had `shipid` set while docked and `worldspaceid` equal to
  the station ID. No `worldSpaceServer` request was observed in the captured
  service-call logs. This does not prove that no later movement call is needed.
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

## Current server implementation

- `src/eve-server/system/WorldSpaceServer.cpp` contains two placeholder
  methods returning `None`; the service is not registered in the main service
  startup. The [upstream Crucible implementation](https://github.com/EvEmu-Project/evemu_Crucible/blob/f55b488a006e3090470d3b481aee47a1d7a0dade/src/eve-server/system/WorldSpaceServer.cpp)
  is also a placeholder. Do not assume these methods are required for every
  local Captain's Quarters operation until the client trace shows it.
- Commit `8c8fe833` changed `Client::InitSession` and `Client::UpdateSession`
  to retain the active `shipid` while docked. The current investigation is
  testing a revert of those two assignments because the quarters issue
  reproduces with this session state. This remains a hypothesis until the
  client transition is rechecked; do not reintroduce PR #323 wholesale.
- The active ship and station data were checked for the affected player at the
  time of investigation. That is a point-in-time observation, not a guarantee
  for other characters or subsequent sessions.
- Character appearance records and portrait JPEGs are separate. PlayerBots
  currently use persistent avatar data and generated portrait files; those
  JPEGs are not proof that their 3D avatars can load or animate in quarters.
  See `doc/PlayerBots.md` for the current provisioning approach and limits.

## Next investigation

1. Recheck the quarters transition with docked `shipid` cleared, then verify
   login, docking, undocking, ship switching and the ship hangar independently.
2. Capture corresponding client and server timestamps for entering quarters,
   walking, returning to the hangar and undocking. Record the first failing
   action and complete error/stack.
3. Determine whether the `GetNewsTickerData` TLS failure blocks only the
   holoscreen or the quarters scene. Do not treat correlation as proof.
4. Trace actual network calls, session keys, avatar data and client asset
   requests. Implement only responses shown to be required by the target
   client; derive packet shapes from the client and real captures. Do not
   invent responses for the placeholder world-space service.
5. Exercise race/station variants, ship and clothing changes, relogging,
   multiple characters and PlayerBot appearance. Confirm no player data is
   overwritten and normal login, hangar and space sessions remain intact.

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
