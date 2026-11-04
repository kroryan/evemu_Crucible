# Captain's Quarters, hangar and character handoff

Status: investigation in progress. Captain's Quarters and the transition from
the hangar are **not verified as working**. Do not advertise walking in
stations as implemented or treat a successful character creation screen as a
successful station-interior test.

## Environment and symptoms

- The target client is Crucible 7.31.360229. The player can log in and select
  a character, but attempts to enter Captain's Quarters have caused the client
  to hang or terminate. The ship-hangar transition has also been reported to
  hang; verify the two transitions independently.
- A Windows minidump from a failed attempt reports an access violation while
  executing `_trinity_deploy.dll`, at module offset `0x512214`. The module
  list contains Wine components. This is evidence of where the client crashed,
  not proof that Wine, a graphic asset, a character model, or a server response
  caused it. The minidump alone does not contain a useful symbolized stack.
- Server logs from that attempt show normal station calls, including
  `stationSvc::GetStation`, but no observed `worldSpaceServer` request before
  the crash. Log configuration and event order limit what can be concluded.
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
- `Client::InitSession` and `Client::UpdateSession` now retain the active
  `shipid` while docked. This is a targeted candidate fix, not proof that the
  hangar transition works. Revalidate character login, docking, undocking,
  ship switching and both station views. If it causes a regression, revert
  these two lines independently; do not reintroduce PR #323 wholesale.
- The active ship and station data were checked for the affected player at the
  time of investigation. That is a point-in-time observation, not a guarantee
  for other characters or subsequent sessions.
- Character appearance records and portrait JPEGs are separate. PlayerBots
  currently use persistent avatar data and generated portrait files; those
  JPEGs are not proof that their 3D avatars can load or animate in quarters.
  See `doc/PlayerBots.md` for the current provisioning approach and limits.

## Next investigation

1. Identify the machine and launcher running the actual client, its Wine or
   Proton version, GPU/driver and graphics settings. Preserve its current
   prefix, settings, game files, cache and crash dumps. Do not reset them.
2. Capture a detailed client log and corresponding server timestamps for each
   action: login, character selection, initial station view, enter hangar,
   enter quarters, walking, return to hangar, and undock. Capture the first
   failing action and full error/stack, not just the final crash dialog.
3. Verify the full Crucible client assets and renderer independently. Compare
   one character across graphics settings and, if available, a known-working
   client environment. A change in outcome is evidence, not automatic proof of
   the root cause. Do not delete caches as a diagnostic step.
4. Trace the actual network calls, session keys, avatar data and client asset
   requests. Implement only server responses demonstrated to be required;
   derive packet shapes from the target client and real captures. Do not invent
   responses for the placeholder world-space service.
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
- The locally deployed server includes ongoing PlayerBot, market, industry
  and planetary work. Those systems have outstanding integration tests; see
  `doc/PlayerBots-ActionPlan.md`. A successful compilation or login is not a
  claim that the PlayerBot economy or planetary production loop is complete.
