# Multiplayer completion review and plan

Reviewed October 1, 2026, on `multiplayer-plan`, HEAD `a4038eb`, including
the current uncommitted parity fixes. This plan targets a complete two-player
game first. New work must remain compatible with the planned four-player
branch, without claiming that the current transport supports four players.

## Assessment

The foundation supports synchronized exploration, host combat rules, shared
dialogue, inventory transactions, NPC barter, stealing, travel, timed effects,
and recovery. Passing those individual fixtures does not establish a complete
campaign. Character advancement, initiating combat as the guest, explosive
timer selection, and story presentation have concrete remaining gaps.

The review follows the native entry points through commands, actor context,
host execution, replica presentation, and persistence. It covers character
editing, combat/AI, skills, inventory, script opcodes, companions, map travel,
automap, movies/endings, save/recovery, protocol limits, and the test matrix.
It includes the recent dirty changes. It is not an execution audit of every
installed quest script, mod, or possible story branch. Content gaps below are
explicitly distinguished from confirmed code gaps.

## Completion checklist

Checkmarks cover the stated implementation and validation scope. A completed
item does not imply that the full campaign acceptance gate has passed.

- [x] M0 / F14: repair baseline reward and initiative regressions; 47-case run passes.
- [x] F01: host-authorized advancement, per-player grants, native editor cancel/spend/perk tests, and recovery save.
- [x] F02 input: guest A, guest first attack, and another-floor entry. Broader AI and encounter coverage remains under M2.
- [x] F15: correct poison decay message, cure timer removal, stale tick protection, native toxin checks.
- [x] F17: replica ground-item placement removes its floating-list entry; native combat loot exits cleanly.
- [x] F16: map-entry scripts use a surviving actor; failing exit and map-route regression tests pass.
- [x] F03: local timer selection, host arming, stack splitting, native roll/source checks, combat AP checks, recovery save, and armed reconnect. Full disk reload and armed map travel remain under M4 / M5.
- [x] F04: host-selected movies/slides, per-player completion, actual native departure and credits; reconnect, missing asset and inactive-window playback checks pass. Quest-route outcome coverage remains under M4.
- [x] F05 / F06 implementation: host-only native Steal hooks and explicit inventory-open outcomes; script-visible Sneak for each registered player. Native script, ownership/lifetime, and focused recovery checks pass. Content-specific delayed reactions and stealth dialogue remain under M4 / M5.
- [x] F07: normal loot approach and combat interaction/AP parity.
- [x] F07 exploration slice: normal corpse/container approach, checkpointed replica inventory opening, native script vetoes, stable target lifetime checks, and host/guest native resource collection.
- [x] F07 combat slice: turn-aware door/pickup/loot commands, native movement and interaction AP, and turn-scoped inventory access.
- [x] F08 implementation: local floor/equipment and roster markers; host-owned scanner charges; stack, ownership, replay, combat AP, native UI, save, and reconnect checks pass. Full disk reload and broader campaign map travel remain under M4 / M5.
- [x] F09 identity groundwork: current actor lookup uses the roster; retained old bodies and replacement bodies have independent identities in core tests. This does not implement death or respawn.
- [x] F09 initial defeat slice: roster-based failure, authoritative shared ending, incapacitated-command rejection, and preservation of the prior recovery save; native checks and visible host/guest deaths pass.
- [x] F09 nested-screen and timer slice: native knockout wake-up for both actors, host modal unwind / guest Ending wait, and terminal queue boundaries in rest/travel.
- [x] F09 native-window slice: actual automap death for either player, held inventory drag/action-menu shutdown, and knockout disk-load/reconnect timer preservation.
- [x] F09 dialogue eligibility: incapacitated players cannot hold up an awake teammate’s ballot.
- [x] F09 hazard slice: real native poison/radiation events kill either player, preserve the other player’s effects, and synchronize terminal automap exit.
- [x] F09 timer-screen slice: death of either player closes both actual explosive-timer prompts without splitting or arming the explosives.
- [x] F09 Pip-Boy slice: terminal exit from the main screen and idle screensaver passes for either player.
- [x] F09 terminal dialogue/barter slice: actual parent and quantity windows unwind, either player can trigger defeat, simultaneous native casualties and authenticated reconnect during defeat pass.
- [ ] F09: shared death/knockout policy and recovery behavior under the three planned modes below.
- [ ] Future persistent corpse piles: each death retains a distinct body or remains; empty corpses can stay visible.
- [ ] Future death-mode selector: any-player death ends the run, cooperative downing/revival, or corpse-and-gear respawn.
- [x] F10 Ian slice: real recruitment, dismissal, gear, combat, corpse and recovery. Other companions remain under content acceptance.
- [ ] F10 remaining companion and quest-specific loyalty acceptance.
- [x] F11: independent active-hand persistence, authority-confirmed HUD changes, versioned save migration and native disk/reconnect recovery.
- [ ] F12: scaling, disconnect/recovery, and checkpoint retry hardening.
- [ ] F13: final documentation and expanded acceptance coverage.
- [ ] M4 / M5: full content campaign, final uninterrupted regression run, and release gates.
- [ ] Future dialogue option: party vote, conversation starter, or host decides.
- [ ] M6: four-player transport integration and campaign acceptance on its separate branch.

The next five goals (current work):

- [x] 1. Finish the current strict-death terminal cases. Native dialogue, barter and its quantity child close on defeat; the native damage action used by scripts and simultaneous casualties pass; a disconnected guest reconnects to the acknowledged terminal checkpoint. Alternative death modes remain future work.
- [x] 2. Shared story movies and actual ending presentation. Both clients render the destruction movies, identical host-selected settlement slides, departure and credits. Independent skipping, reconnect after playback, missing movie assets and unfocused guest playback pass. This validates presentation, not completion of the underlying quests.
- [x] 3. Real recruited Ian lifecycle: recruitment, dismissal, gear, native retaliation, battle, death, travel and fresh-session recovery pass. Other companions and quest-specific loyalty remain content acceptance in goal 5.
- [x] 4. Independent active hand. Save format 8 retains each actor's hand, versions 1–7 default to left, snapshot/wire formats reject invalid values, rejected requests leave the HUD unchanged. The 50-scenario interim run includes native disk/reconnect recovery with host left and guest right.
- [ ] 5. Complete content acceptance and final release gates on the final code. Builds 45, 51, 62, 64, 66 and 72 each passed all 50 scenarios. Build 79 passed all 50 scenarios and the core gate with clean exits; candidate 81 adds focused fixes and its full run is underway. Ordinary full-story acceptance remains open.

Goal 5 acceptance slices:

- [x] Ordinary Shady Sands cave completion: all nine scorpions killed, bodies looted, native 500 XP and Seth completion dialogue, real Ian recruited and saved.
- [x] Ordinary shared-rest HUD refresh: both actual HAL/testbox HP labels match native state after healing.
- [x] Definitive checkpoint capacity: commands/native rules freeze, saving refuses before mutation, prior save hashes remain unchanged, ordinary Busy retries continue.
- [x] Native shutdown and recovery: exact-slot script deletion, unique human script IDs, checkpoint-ACK transport disconnect remains reconnectable; focused build 78 exits cleanly.
- [x] Companion duplicate cleanup: distinct/shared SID cases, nested timers, native first-tile iterator, travel/save/authenticated recovery, active guest execution counters and ASAN negative/positive proof on build 79.
- [x] Failed native rollback retry: blocked map destination retains all backups; removing the obstruction allows complete restoration with unchanged multiplayer metadata.
- [ ] Already-open guest Pip-Boy alarm HP/time refresh: code built in candidate 80; visible acceptance pending.
- [ ] Latest complete 50-scenario compatibility campaign and emitted digest comparisons.
- [ ] Ordinary Junktown and subsequent story, other companions, quest loyalty and alternative routes.
- [ ] Checkpoint application rejection leaves native state unchanged across every fallible construction step.
- [ ] Full interrupted-save/sidecar publication acceptance and platform/long-session release checks.

Evidence for this batch: `/var/tmp/fallout-five-goals-interim22-campaign/summary.tsv`, `/var/tmp/fallout-five-goals-dialogue-script-host22-20261001.log`, `/var/tmp/fallout-five-goals-barter-guest22-20261001.log`, `/var/tmp/fallout-five-goals-quantity-death23-20261001.log`, `/var/tmp/fallout-five-goals-simultaneous-death-20261001.log`, and `/var/tmp/fallout-five-goals-reconnect-death-20261001.log`. The script-death fixture invokes `action_dmg`, the native action called by `op_critter_damage`; it is not a claim that every installed lethal script has been played.


Story evidence: `/var/tmp/fallout-five-goals-native-story29-20261001.log`, `/var/tmp/fallout-five-goals-story-reconnect33-20261001.log`, `/var/tmp/fallout-five-goals-story-missing33-20261001.log`, and `/var/tmp/fallout-five-goals-story-unfocused36-20261001.log`. The final native sequence includes both destruction movies, settlement slides and departure/credits, followed by acknowledged Ending. Reconnect does not replay an already completed presentation. Inactive multiplayer audio continues consuming buffers silently so movie timing advances; solo focus behavior is preserved. Existing native animation is drained before a complete story checkpoint. Story state blocks saving and native queue advancement; final presentation preserves the prior recovery save.

Full guest playback without skipping passed in `/var/tmp/fallout-five-goals-story-watch52-20261001.log`, including all six selected settlement narrations and the actual departure movie and credits, with acknowledged terminal completion. The story barrier now allows ten minutes for movies and thirty minutes for slides or finale, so a host skipping ahead does not cut off a guest watching the full sequence. This remains presentation evidence rather than quest-route completion.

The current schema uses gameplay wire 47, snapshot 26, sidecar 8 and recovery wire 4. Story completion carries an authenticated player ID rather than an implicit guest identity. Snapshot actor ownership accepts bounded IDs 1–16, with unique roster membership; a four-actor completion/independent-hand recovery test passes. This does not add four-player transport.

Actual Ian evidence so far: host and guest recruitment, dismissal and recruitment again through installed native voted dialogue, then real disk load and authenticated reconnect, pass in `/var/tmp/fallout-five-goals-companion-lifecycle35-20261001.log` and `/var/tmp/fallout-five-goals-companion-guest36-20261001.log`. Host and replica retain native party membership and equal digests. The extended host lifecycle passed native weapon instruction, gear preparation from Ian's existing inventory, two shared native map loads (Junktown and Shady Sands), a guest-floor/host-leader check, and disk/reconnect gear and caps preservation in `/var/tmp/fallout-five-goals-companion-travel46-20261001.log`. The guest-initiated extended lifecycle and real native death/corpse/resources recovery passed in `/var/tmp/fallout-five-goals-companion-corpse50-20261001.log`; both digests were `18218681099470628262`. The latest corpse recovery run also passed in `/var/tmp/fallout-five-goals-companion-corpse54-20261001.log`, after preserving dead companions during duplicate cleanup, collecting duplicate objects before erasure to avoid iterator invalidation, and bounding native party load counts. Battle and native retaliation now pass on build 64; the other installed companions remain content checks. NPC party membership now travels in snapshots; human roster bodies cannot enter the legacy AI party. Ambient companion scripts, AI following and floor synchronization use the shared host leader, independent of temporary guest action context. Map recovery releases copied companion/item local-variable buffers after restoring them.


## Implementation progress

The following results are subsequent to the original review. Work remains on
F09 death-mode options, remaining companion content, F12/F13, and the campaign completion gates.

- F14: the expanded 47-case matrix passes. The reward fixture waits for native
  animation readiness, and combat assertions accept native initiative order
  while retaining ownership checks.
- F01: added host-validated advancement intents and independent processed-level
  and pending-perk persistence. Native editor tests pass for cancel, skill
  spending, perk selection, and recovery save on both players. Old sidecars
  cannot reconstruct unrecorded pending perks; migration avoids duplicate level
  grants. This slice introduced gameplay 40, snapshot 22, save sidecar 6.
- F02: added host-authorized combat entry through native A and attack inputs,
  with the initiating player's elevation selecting the battle. Focused native
  tests pass for guest A, guest first attack, and first attack on another floor.
  Core tests cover ownership, replay, missing targets, phase, and wire validation.
  All 50 installed-data scenarios have passing results across the initial run
  and repair reruns, plus the core gate. This is not an uninterrupted full run
  on the final binary. Broader AI and campaign acceptance still remain.
- F03: each player selects the explosive timer on their own client. Cancel
  sends no command; the host validates the choice and arms exactly one item
  from a stack. Native Traps success, shortened failure timers, and immediate
  critical failure remain authoritative. Player-owned queued explosions retain
  the armer's stable player ID across recovery and reconnect, including dropped
  or planted explosives. Inventory use retains native AP behavior; combat HUD
  use costs 2 AP. Ownership, replay, stale turns, and already-armed items are
  checked before mutation. New command/event formats use gameplay 41, snapshot
  23, and save sidecar 7; old native explosion event types remain readable.
  Focused native checks cover both actors, roll outcomes, dropped/planted damage
  attribution, event payload read/write, and combat AP. Both real timer dialogs
  pass cancel and arm, followed by a host recovery save and guest reconnect with
  one correctly attributed timer per bomb. The muted HAL/codex-testbox pair also
  passed visible cancel/arm testing (role 80), then returned to Shady Sands.
  Fresh-session disk reload with armed explosives, actual map departure while
  armed, and explosive kill XP remain explicit campaign checks under M4 / M5.
- F05 / F06: Steal now uses the same native skill-script gate as solo play,
  including jammed-lock checks. The host invokes USE_SKILL_ON under the thief's
  scope, honors script overrides, and resolves stable actor/target IDs again
  before granting access. Moved, hidden, destroyed, or invalid targets cannot
  retain theft access. The event explicitly reports whether inventory opened;
  a script-handled action therefore carries its effects without asking the
  guest to open inventory or rerun scripts. Caught theft retains its native
  PICKUP hook. Script procedure cleanup now resolves the script ID again after
  execution because owner destruction can remove or compact its script storage.
  `using_skill` resolves the explicitly supplied registered player's Sneak
  flags, restores the calling scope, and rejects NPCs and unrelated skills.
  Native interpreter fixtures pass for both players: ordinary access, script
  override with shared effects, moved target, destroyed target, jammed lock,
  Sneak query, and caught callback. Existing guest theft transfer/plant/XP/caught
  checks and focused movement, skill, combat inventory, and recovery also pass.
  Visible role 82 testing opened and closed guest theft on Katrina and host
  theft on Seth in Shady Sands; both clients remain muted and connected.
  Gameplay wire version is now 42; snapshot 23 and sidecar 7 are unchanged.
  See `/var/tmp/fallout-steal-sneak-focused2-20261001` and the updated
  [script-role audit](player-role-audit.md). Full stealth-dependent dialogue,
  delayed reactions, and campaign quest branches remain under M4 / M5.
- F07 exploration: ordinary corpse and container loot now uses the host's
  native walk/run approach before granting inventory access. Armed movement
  falls back to walking when the current weapon has no running art. The host
  resolves stable actor/target IDs again after movement, container USE, and
  PICKUP hooks; script vetoes, locks, moved/hidden/deleted targets, interrupted
  animations, and invalid phases prevent inventory access. Loot immediately
  requests an authoritative checkpoint, and the replica waits for that state
  before checking distance and opening its local window. Event acknowledgements
  wait for the covering state so recovery cannot skip pending loot presentation.
  A hidden target also
  invalidates an already-open native loot window. Production uses existing
  player/entity IDs and event formats; protocol versions are unchanged.
  Native interpreter fixtures cover successful corpse access, script veto,
  moved target, self-destruction, and locked container for every roster actor.
  Real native UI tests pass for both host and guest from five tiles away,
  collecting both resource stacks and completing following combat with equal
  world digests. The native fixture now checks reachability in the direction
  the engine actually approaches; an empty starting tile alone was insufficient.
  These are isolated muted Xvfb tests. HAL/testbox remained in their existing
  Shady Sands session for this exploration slice. Combat coverage follows below.
  Native UI artifacts: `/var/tmp/fallout-native-container-loot-j16xkomj`
  (host) and `/var/tmp/fallout-native-container-loot-arh870j7` (guest, final
  acknowledgement change). Leaving guest loot open also passes phase/range
  invalidation in `/var/tmp/fallout-native-container-loot-rwnohmr1`. The default
  18-scenario regression run passes in
  `/var/tmp/fallout-loot-approach-default3-20261001`; final acknowledgement changes
  receive focused loot/container/recovery reruns rather than counting that run
  as the final full campaign gate. Enabled CTest passes 3/3; disabled passes 2/2.
- F07 combat: door, pickup, and loot commands carry the exact combat turn
  revision. The host validates the acting roster player, approaches with native
  movement AP, and charges the native 3 AP interaction once. Insufficient AP,
  foreign turns, stale revisions, and repeated commands cannot repeat an action.
  Completed door state and pickup inventory changes arrive through authoritative
  checkpoints. Rejected interactions also checkpoint movement AP already spent.
  Corpse/container loot grants access only for the current turn and target.
  Native stacked-corpse navigation keeps that authorization without another
  charge; closing the window removes it. Ordinary inventory opening still costs
  its own 4 AP, adjusted by Quick Pockets. Replica loot waits for its exact turn
  and covering checkpoint before opening. Native UI close also clears access
  during exploration. Tests iterate registered players rather than assuming two
  fixed actor slots.
  The native guest combat loot window collected both resource stacks, consumed
  exactly 3 AP, and completed a following attack with equal peer digests and
  clean process exits. Evidence is
  `/var/tmp/fallout-native-container-loot-lkqf2038`. Exploration UI still passes in
  `/var/tmp/fallout-native-container-loot-ijzw6i3n`. Native host/guest AP,
  ownership, stale turn, movement, close, pickup, and door checks pass in
  `/var/tmp/fallout-combat-interactions-focused3-20261001`.
  The final default 18-scenario run passes in
  `/var/tmp/fallout-combat-interactions-default2-20261001`. Enabled CTest passes
  3/3 and disabled CTest passes 2/2. Gameplay wire is now 44; snapshot 23 and sidecar 7 remain unchanged. These UI tests are isolated,
  muted Xvfb sessions, separate from the visible HAL/testbox game.
  Both visible clients were updated together to wire 44 as role 84 after saving
  into unused SLOT09. Reload preserved host tile 14112 / HP 43 and guest tile
  14314 / HP 31, resources, activity, and poison 0. Both remain connected and
  muted in Shady Sands. Evidence is
  `/var/tmp/fallout-combat-interactions-visible84.json`.
- F09 initial defeat slice: party failure scans registered current bodies, not
  `obj_dude`. Any player death or zero HP ends the party; one native knockout
  does not, while knockout of the whole roster is terminal in the initial mode.
  NPC corpses are excluded. The host decides when to end; the guest waits for the
  authoritative Ending checkpoint and shows the same defeat presentation.
  Combat checks party failure in local input, remote turn waiting, and turn
  completion. Native `critter_kill` no longer independently exits multiplayer
  when its target is the story actor. Ordinary knockout recovery stays in the
  native host queue for both players, without the host-only combat-end wake-up.
  A command gate rejects incapacitated actors before effects and remembers that
  rejection across replay after recovery. The gate permits turn yielding, window
  close, and trade cancellation for cleanup; their normal checks still apply. Terminal defeat preserves the
  previous recovery save rather than overwriting it with a losing state.
  Core tests pass 3/3; multiplayer-disabled tests pass 2/2. Native roster checks,
  combat status, inventory actions, and recovery pass in
  `/var/tmp/fallout-party-failure-focused-20261001`. A visible friendly-fire test
  on role 85 used the host's pistol to kill the guest; both clients exited the
  world. The reverse test on role 86 used the guest's pistol to kill the host;
  both clients exited again, without a checkpoint acknowledgement timeout.
  SHA-256 hashes of RECOVERY/SAVE.DAT and MULTI.DAT match before and after this
  defeat. Evidence is `/var/tmp/fallout-visible-guest-death85-events.json` and
  `/var/tmp/fallout-visible-host-death86-events.json`.
  Both muted clients were restored from SLOT09 as role 87, with prior positions,
  HP, pistol ammunition, inventory, and poison 0 intact. Evidence is
  `/var/tmp/fallout-party-failure-visible87-restored.json`.
  Full F09 remains unchecked pending additional hazard/modal/load tests and
  policy-specific behavior. Revival, respawn, a selector, and corpse piles are
  not implemented by this slice. Protocol versions are unchanged.
- F09 nested-screen and timer slice: background networking now requests native
  screen exit after host-authorized party defeat outside combat. The replica
  waits for the authoritative Ending phase before requesting exit. Native
  combat still finishes through its own turn/animation checks. Rest and travel
  reject an already-terminal party and stop at the exact queue boundary that
  causes defeat, even when the callback itself returns zero. The main loop
  exits before processing queued talk, map entry, or another action after a
  terminal quit request. Network automap and inventory quantity/explosive timer
  prompts now honor that request too.
  Native fixtures exercise the real knockout queue callback separately for every
  roster player, clearing the consumed event and permitting action afterward.
  They check host exit during dialogue/travel phases and replica waiting in those
  phases. A simulated scripted lethal queue outcome with a zero callback result
  verifies rest/travel stop at its one-tick deadline. Fixture cleanup restores
  actor flags/HP/art, original queue payloads, clock, map and travel state. Guest
  fixtures execute no queue rules and mirror phase revisions before normal
  gameplay. The added travel test accounts for native city steps that move
  without advancing time; its first-step clock assumption initially failed.
  Final movement passes in `/var/tmp/fallout-party-modal-movement-20261001`;
  core tests pass 3/3 and disabled tests pass 2/2. Final-build guest dialogue,
  timed rest, world-map queue interruption, and recovery pass in
  `/var/tmp/fallout-party-modal-focused2-20261001`. Native automap and explosive
  timer UI regressions pass, including save and guest reconnect, in
  `/tmp/fallout-native-automap-89y248sm` and
  `/tmp/fallout-native-explosive-timer-xp_hf_ug`. These are isolated muted tests,
  not the desktop session. Both visible clients are updated as role 88 and
  restored from SLOT09 in Shady Sands, connected and muted, with prior positions,
  HP 43/31 and poison 0. Evidence is
  `/var/tmp/fallout-party-modal-visible88-restored.json`.
  The earlier central modal/queue changes passed all 18
  default scenarios in `/var/tmp/fallout-party-modal-default-20261001`, before
  the automap/prompt exit guards and expanded zero-return fixture. This is not
  the final uninterrupted full-campaign gate. Full F09 still requires terminal
  outcomes inside actual native modal windows, lethal poison/radiation/script
  content, fresh disk reload and disconnect during knockout/defeat. Revival,
  respawn, corpse piles, and their selector remain future modes.
- F09 native-window and saved-knockout slice: real native lethal damage to
  either player while both automap windows are open unwinds both windows and
  reaches the acknowledged Ending boundary. The fixture requires scanner input
  from both windows before applying damage; it does not substitute a phase flag
  for an open window. Evidence: `/tmp/fallout-native-automap-al9lb2u3` (guest)
  and `/tmp/fallout-native-automap-ug392spm` (host).
  Review found inventory, loot, and barter drag loops and the inventory action
  menu could wait indefinitely for mouse release after terminal shutdown. Those
  loops now cancel, release cursor art/menu resources, and skip the pending
  action. Actual held-drag and held-action-menu tests leave the mouse pressed
  through shutdown and verify both native windows close. The action-menu test
  also verifies the host inventory is unchanged. Evidence:
  `/tmp/fallout-native-inventory-t99nk9b1` and
  `/tmp/fallout-native-inventory-wjdlzvrn`. Normal native equipment drags,
  use/drop/unload/ammo reload, and subsequent combat still pass on both clients
  in `/tmp/fallout-native-inventory-b_v6ef3m`.
  Recovery fixtures now save one knocked-out player with one exact wake deadline,
  load from both the hidden recovery save and a normal slot into a fresh session,
  and reconnect that player. Both peers must retain knockout, its single timer
  at the original deadline, and matching checkpoint digests. Evidence:
  `/var/tmp/fallout-knockout-disk-reconnect-20261001`.
  A related review found the new incapacitated-action gate rejected dialogue
  votes while the ballot still waited for the knocked-out participant. Native
  ballot eligibility now excludes dead/missing/knocked-out bodies, and host
  connectivity refresh covers the host as well as the guest. The native fixture
  knocks out each roster member in turn and proves awake voters can resolve
  immediately, with no vote from the incapacitated player. Future selectors
  remain separate from this eligibility fix.
  Final dialogue, recovery-save, and normal-slot scenarios pass with matching
  peer digests in `/var/tmp/fallout-party-recovery-final12-20261001`. Enabled
  CTest passes 3/3; disabled build and CTest pass 2/2. Python drivers compile
  and `git diff --check` is clean.
  These tests were muted isolated Xvfb sessions. The visible HAL/testbox pair
  remains on role 88; this slice has not restarted or updated that pair.
  Full F09 still needs terminal dialogue/barter/Pip-Boy/timer windows, lethal
  poison/radiation/script content, disconnect during defeat, and the future
  death-mode implementations. The earlier remaining-work paragraph describes
  the previous slice; disk reload/reconnect during ordinary knockout and terminal
  automap/inventory checks are now covered.
- F09 native lethal hazards and timer prompts: the muted fixture creates poison
  through `critter_adjust_poison`, or radiation through `critter_adjust_rads` and
  `critter_check_rads`, preserving each native payload and handler. Only that
  hazard’s wait is shortened to one tick. Native queue processing then kills
  the selected player. Both host and guest deaths pass for both hazards, with
  the other roster actors’ HP, poison, radiation and primary-stat bonuses
  unchanged. The host publishes Ending and both actual automap windows unwind.
  Evidence: `/tmp/fallout-native-automap-w15eb0vj` (guest poison),
  `/tmp/fallout-native-automap-5sir9uhd` (guest radiation),
  `/tmp/fallout-native-automap-ixpfbmuc` (host poison), and
  `/tmp/fallout-native-automap-nvrnbean` (host radiation).
  Actual native explosive-timer prompt tests wait until both prompt windows
  have opened before holding the host mouse to trigger native lethal damage.
  For either victim, both prompts cancel and close, the original two-item
  dynamite stacks remain, no armed dynamite appears, and Ending is acknowledged.
  Evidence: `/tmp/fallout-native-explosive-timer-lk__dl3y` (host death) and
  `/tmp/fallout-native-explosive-timer-9ic4et63` (guest death).
  Review found the Pip-Boy’s idle screensaver did not check terminal shutdown.
  It now unwinds on the quit request, and the main Pip-Boy input loop exits
  before refreshing or processing further actions. Native fixtures can shorten
  the idle wait only when smoke testing is enabled; ordinary play keeps its
  two-minute timeout. The actual screensaver and its main window close on
  guest death in `/var/tmp/fallout-native-automap-p3x49453`; the ordinary Pip-Boy
  closes in `/var/tmp/fallout-native-automap-jun6c6p6`. Host death passes for
  the main screen in `/var/tmp/fallout-native-automap-zaxp9q0h` and screensaver
  in `/var/tmp/fallout-native-automap-r6j7wveg`. The final ordinary timer
  regression passes cancel/arm/split for both players plus save/reconnect in
  `/var/tmp/fallout-native-explosive-timer-7d5058q7`. Enabled and disabled builds
  pass, CTest passes 3/3 and 2/2, Python drivers compile, and diff checks are clean.
  Initial Pip-Boy tests and one disabled-build attempt hit `/tmp` capacity
  before running. Disposable DATA copies from this turn’s completed/failed
  runs were removed while retaining logs; retries use `/var/tmp`.
  The visible role-88 pair has not been restarted or updated. These are engine
  fixtures, not acceptance of radiation exposure in the Glow or a full content
  campaign. Terminal dialogue/barter, scripted deaths, disconnect during defeat,
  and the future death modes remain pending.
- F09 identity groundwork: removed duplicate host/guest actor ID caches from
  LocalSession. Actor lookup now follows the registered player's current body;
  unregistering a player cannot leave the session resolving a retired actor.
  Core tests cover four registered actors, map-object rebinding for an additional
  actor, and retaining an unowned old body while the same player ID resolves a
  new body with its character build preserved. The tests exercise registry
  lifecycle boundaries, not implemented corpse spawning or respawn gameplay.
  Enabled CTest passes 3/3. Native map transition and recovery both pass with
  matching peer digests in `/var/tmp/fallout-roster-body-focused-20261001`. Wire,
  snapshot, and sidecar versions are unchanged.
- F17, found during native combat loot testing: placing a new replica item on
  the ground left both its original floating-object list entry and a map list
  entry pointing to the same allocation. Game reset freed it twice. Ground
  placement now detaches the old list entry before connecting the object to
  its map tile. The combat loot test above verifies clean guest shutdown after
  creating, looting, and synchronizing that container.
- F08: the live automap uses the presented actor's elevation and sensor hands.
  The local actor has the large marker; other registered players on that floor
  have distinct teammate markers. Network maps refresh moving players while
  open. A personal multiplayer map keeps host animation/AI running, while solo
  retains the native pause. Sensor activation carries an owned item ID and the
  current phase/turn to the host. Replicas enable scanning only after an accepted
  result and its charge checkpoint. Repeated S input within a scan does not
  consume another charge. Stacked sensors split one item before consumption.
  Inventory and HUD sensor use consume on the host and open only the acting
  client's map, avoiding a modal UI inside authoritative execution. Automap
  activation costs no AP, inventory use retains the inventory-open charge, and
  combat HUD use costs 2 AP. Empty, unequipped, foreign, replayed, and stale-turn
  cases pass focused native checks; marker checks cover both players/floors.
  Both native map windows pass two scans, repeated S, empty-charge refusal,
  host recovery save, and guest reconnect with equipment and charges intact.
  The final native UI run places host and guest on floors 0 and 1; the guest
  map shows its own marker without markers from the host's floor. Visible
  role 83 testing in Shady Sands confirms guest movement to tile 14314 while
  HAL's map stays open, with matching host/guest positions. Both maps were
  closed afterward and the muted clients remain connected.
  Evidence is `/var/tmp/fallout-automap-focused1-20261001` and
  `/tmp/fallout-native-automap-1vb3v7d6`. Visible movement evidence is
  `/var/tmp/fallout-automap-visible-movement83.json`. Gameplay wire is now 43, snapshot 23,
  and sidecar 7. Full fresh-session disk reload with used sensors and campaign
  map/travel interactions remain under M4 / M5.
- F15, found during testing: SLOT08 already contained poison 10 before the
  characters moved. Native decay incorrectly printed the poisoning message
  again. Decay now avoids that message, curing clears the timer, and stale
  callbacks cannot damage a cured actor. Native toxin checks pass, including
  cure-timer removal and stale ticks. Journals now expose poison/radiation.

- F16, reproduced during the expanded regression run: a guest-owned exit
  command retains its acting-player scope while the native map loader destroys
  the guest object. A destination entry script can then receive the freed
  object from `dude_obj` and crash while granting inventory. Shared map loading
  now explicitly scopes entry scripts to the surviving native story actor.
  The failing exit and four other map-loading routes now pass. Final-build
  exit and guest attack on another floor also pass. Pending combat entry is
  cleared on map replacement so a queued request cannot start in a new area.
- Visible testing also exposed a progression-checkpoint warning on dialogue
  close before the deferred map transition completed. Both clients eventually
  reached CAVES.MAP. Investigate checkpoint retries and guest Enter-to-end-combat
  behavior; neither path is declared clean based on the focused entry tests.

Combined evidence is in
`/var/tmp/fallout-multiplayer-combat-start-combined-20261001.tsv`; the initial
exit-grid failure and GDB reproduction remain recorded separately.

Focused evidence is in `/var/tmp/fallout-multiplayer-advancement-focused-20261001`,
`/tmp/fallout-native-character-editor-rkqgvuz8`, and
`/var/tmp/fallout-multiplayer-combat-start-focused2-20261001`. Visible testing
runs on the updated muted HAL/codex-testbox pair, role 83. Separate local
sessions are automated tests and are not the desktop sessions the user watches.

F03 evidence is in `/var/tmp/fallout-explosive-focused6-20261001`,
`/tmp/fallout-native-explosive-timer-yhprgs8t`, and
`/var/tmp/fallout-explosive-default-matrix-20261001`. The default 18 scenarios
passed in one uninterrupted run. The expanded 50-scenario gate has not yet
been rerun uninterrupted with F03.

## Findings

This table records the original review baseline. The checklist and evidence above
track repairs; an original gap below is not a claim that it remains unfixed.

P0 means a campaign blocker or authority/persistence defect. P1 means a
remaining gameplay or story feature. P2 means hardening or presentation work.

| ID | Priority and evidence | Finding and consequence | Implementation and acceptance |
| --- | --- | --- | --- |
| F01 | P0, confirmed code gap | `editor_design` installs local player context and directly changes skills/perks. There is no advancement command in `GameCommandPayload`. `last_level` and `free_perk` remain editor globals. Guest edits have no host commit; checkpoints can replace them. Level-up grants and pending perks are not independently persisted. See `src/game/editor.cc:635`, `:528`, `:5220`, and `src/multiplayer/player_character_state.h:20`. | Host computes level grants exactly once. Store processed advancement level and pending perk entitlement per actor. Use a revisioned draft/commit command for skill investment and perk choices; validate costs, prerequisites, caps, and ownership rather than accepting a client build. Both characters must advance through different levels, cancel drafts, reopen the editor, and save/reconnect without lost or repeated grants. |
| F02 | P0, confirmed input gap; floor risk needs live reproduction | A outside combat calls `combat(NULL)`; guest `combat` returns immediately. Attack-from-exploration also reaches native combat before the network attack hook. Network attack submission requires an existing Combat phase. `combatai_want_to_join` rejects NPCs whose elevation differs from `obj_dude`. See `src/game/game.cc:574`, `src/game/combat.cc:2488`, `:4682`, `src/multiplayer/network_runtime.cc:5871`, and `src/game/combatai.cc:1253`. | Add authenticated combat initiation, including an optional first attack and its intended mode. Host selects combat elevation from the initiating actor, validates targets and phase, and admits eligible actors/NPCs to that battle. Test A and actual HUD attacks from both players, including a guest initiating on another floor. Existing separated-elevation coverage only moves the guest away from a host-started battle. |
| F03 | P0 for complete item support, confirmed code gap | Using explosives reaches `inven_set_timer` inside host execution. A guest action therefore asks the physical host to select the timer. No command carries the guest's chosen seconds. See `src/game/protinst.cc:875` and `src/game/inventry.cc:5947`. | Select the timer in the acting player's UI, then send the choice with stable item identity and ownership/revision checks. Enforce native 10–180 seconds in steps of 10. Host alone splits/arms the item, rolls Traps, and queues the explosion. Test cancel, stacked explosives, failure/critical failure, planting/drop, damage attribution, and save/map/reconnect while armed. |
| F04 | P0 for completing the story together, confirmed code gap | `op_play_gmovie`, queued endgame slides, and `op_endgame_movie` call local native presentation. Guests suppress script execution, and no movie/slide result exists in the event/snapshot types. The tested Ending phase is orderly session shutdown/recovery, not proof that both players see the actual story ending. See `src/int/support/intextra.cc:2955`, `:3868`, `:3929`, `src/game/scripts.cc:921`, and `src/game/endgame.cc:294`. | Add a host-selected story presentation sequence with a revision, movie/slide identifiers, authoritative outcome, and per-player completion. Keep story mutation separate from presentation, including the native slideshow's global mutation. Handle local skipping, missing movie assets, disconnect, replay, and terminal save policy. Both clients must see the destruction movies, chosen settlement outcomes, final departure, and credits without rerunning scripts. |
| F05 | P1, confirmed parity gap | The dedicated Steal executor grants access directly. Native `obj_use_skill_on` first runs USE_SKILL_ON and honors script override/jammed locks. The new path bypasses that hook. See `src/multiplayer/network_world.cc:1802` and `src/game/protinst.cc:1523`. | Run the target skill hook once on the host under the actual thief, honor overrides, and revalidate object lifetime after scripts. Carry an explicit inventory-open outcome so a successful script override does not accidentally open theft on the guest. Cover scripted refusal, quest effects, destroyed/moved targets, and ordinary theft. |
| F06 | P1, confirmed script API gap | Sneak state and timers are actor-owned now, but `op_using_skill` still accepts only `object == obj_dude`. A script asking whether the guest is sneaking gets zero. `dude_obj` already returns the scoped acting player, contrary to the older role-audit document. See `src/int/support/intextra.cc:334` and `:841`. | Query the explicitly supplied registered player's build in its scope. Audit perception, disguise, reaction, and delayed script context. Document the actual story/local/acting contract and test stealth-dependent dialogue/hostility for both players. Do not globally replace `obj_dude`. |
| F07 | P1, confirmed native action restrictions | Ordinary Loot requires adjacency and rejects combat; Door use and pickup also reject combat. Native corpse loot approaches and charges combat movement/interaction AP. Steal approach does not repair ordinary loot clicks. See `src/multiplayer/network_world.cc:1740`, `:1764`, `:1066`, and `src/game/actions.cc:1199`. | Add approach/completion for normal loot and turn-aware commands for native legal doors, pickup, corpse loot, and container interaction. Apply AP once on the host; retain script vetoes, locks, and inventory access restrictions. Verify exact AP, unreachable targets, stale turns, and target deletion before opening the replica UI. |
| F08 | P1, confirmed automap resource/role gap | Automap reads `obj_dude` hands and highlights that actor. Scanner use directly decrements item charges locally. A guest can consult the host's equipment/marker and mutate a replica charge without a host command. See `src/game/automap.cc:313`, `:323`, `:419`, and `:458`. | Present the local actor and roster markers. Route scanner activation/charge consumption through the host; keep map display local. Test separate floors, guest-only sensors, empty charges, repeated input, and checkpoint/save restoration. |
| F09 | P1, design gap with confirmed asymmetry | Main-loop death/knockout termination reads `obj_dude`; guest death does not take the same path. See `src/game/main.cc:476`. There is no documented complete cooperative death policy. | Implement the three-mode design below by roster actor IDs. Preserve truly fatal injuries in cooperative mode. Synchronize the outcome, block inappropriate native wake-up/death side effects, and persist downed state and the selected policy. Test every roster actor, simultaneous casualties, critical injuries, poison/radiation death, and reconnect during the outcome. |
| F10 | P1, unverified content/role risk | Native companion recruitment changes party membership, IDs, scripts, and save flags. AI follow behavior remains host-centered. Current fixtures cover companion-shaped critters, not a recruited companion's complete lifecycle. See `src/game/party.cc:72`, `src/int/support/intextra.cc:3301`, and `src/game/combatai.cc:1243`. | Declare NPC companions shared and host-simulated. Test actual recruitment, dismissal, equipment/trade, loyalty/hostility, death, travel, floors, save and fresh-session recovery. Confirm whether following the story actor is intended. Human players must never become native AI party members. |
| F11 | P2, confirmed persistence omission | Equipment carries an active hand during commands, but player builds/sidecars lack an independently saved HUD hand. Pending advancement state is also missing, covered by F01. | Persist gameplay-relevant hand selection per actor and keep local UI preferences separate. Restore appearance and attack mode coherently. Bump formats only when new serialized fields require it, with explicit older-save defaults. |
| F12 | P2, unverified scale/recovery risk | Snapshots have a 512 KiB total limit and separate object-count limits. Complete periodic states are sent every 250 ms. Runtime/world adapters exceed 16,000 lines together, including extensive smoke setup. | Measure real dense late-game maps and large inventories, packet sizes, apply time, queue behavior, and delayed links. Preserve bounded queues and complete checkpoint semantics. Extract focused gameplay modules incrementally with their fixtures; avoid a wholesale rewrite. |
| F13 | P2, confirmed documentation/test coverage gap | Historical docs describe already-fixed missing Sneak/barter work and still say 13 default/42 full scenarios. The current runner contains 18 default/47 full. Fixture success does not cover ordinary guest combat initiation, character commits, or real final story presentation. | Publish a current feature/coverage index with implemented, verified, and unresolved columns. Update counts and role semantics. Add regressions at normal UI entry points, not only executor calls or host-created battles. |
| F14 | P1, reproduced regression-gate failures | The `skill` scenario fails in its subsequent authority probe at `NATIVE_SCRIPT_REWARD_CHECKPOINT_FAIL`; an isolated retry repeats it. A symbol-enabled diagnostic confirms reward registration succeeds but capture returns false, leaving the output snapshot empty. Separately, `combat-turn` passes on the host but fails on the guest after observing owners `2,1`; its assertion requires `1,2`. These results do not establish missing live quest rewards or incorrect native initiative. | Diagnose the capture refusal and make the reward fixture respect capture readiness without hiding persistent failure. Verify initiative and turn permissions, then remove the fixed host-first expectation if native ordering is valid. Retain reward/conservation and turn-ownership regressions. Restore a passing full matrix before shipping the remaining features. |

The guest A input was reproduced on the muted HAL/codex-testbox pair during
this review: both remained in Exploration. This is separate from combat
execution after the host has already started a battle.

## Work order and completion gates

### M0. Establish the release contract and baseline

- Keep this branch and its dirty fixes intact; isolate implementation changes
  into reviewable commits before merging or releasing anything.
- Use one shared map and clock, host-only rules, separate player builds, shared
  quests/reputation, personal skill XP, and existing shared combat/quest XP.
- Document the proposed death, knockout, disconnect, companion, and quest-item
  policies. For disconnects, prefer pausing dangerous timed sequences until
  recovery rather than letting an absent player miss an irreversible outcome.
  This is a proposed change to the current host-continues behavior.
- Record content/build fingerprints, protocol versions, and baseline evidence.
  At the review baseline, versions were gameplay 39, snapshot 21, and save sidecar 5.
- Resolve the reproduced expanded-gate failures F14 before relying on the
  regression matrix for later implementation.

Gate: every future ticket has an actor owner, host mutation, replica outcome,
save/recovery behavior, and a reproducible acceptance case.

### M1. Close progression and authority blockers

Implement F01 first, then F02, F03, F05/F06, and F08. Advancement should start
with persisted per-player grant bookkeeping, followed by the editor draft and
host commit. Do not implement skill buttons before the entitlement boundary.

Every mutation command must validate actor ownership, phase/turn, revision,
target/item lifetime, and quantity before scripts or RNG. Replay must return
the recorded outcome without charging or granting again. Replica effects must
wait for the checkpoint that makes their objects and position available.

Gate: both players can independently advance through perk levels, initiate
combat through native inputs, arm/cancel explosives, use script-visible
stealth/theft, and consume scanner charges. Invalid/repeated commands produce
no world mutation. A save and fresh reconnect preserve every result.

### M2. Finish native interaction and combat behavior

Implement F09, then verify the full weapon/status matrix below. Audit
active combat elevation and companion admission/following with F02/F10.
Add useful rejection feedback and complete guest-visible action feedback.
Finish projectile, burst, thrown/explosion, hit, death, and sound presentation
as host-authored cues that cannot execute damage rules on the guest.

Gate: a complete encounter supports both initiators, multiple NPCs, all weapon
families, friendly fire, injuries, legal interactions, death/knockout, and
disconnect/recovery with exact resource/AP conservation and no stuck turns.

### F09 death-mode design

User direction on October 1 replaces the earlier single-policy recommendation.
Plan three modes, with a session selector as future work. Implement shared outcome
handling before adding that selector. The initial implementation uses any-player
death as game over. Revival and respawn remain future modes.

- **Any-player death ends the run.** A genuine death of any registered human
  player stops the party and offers a shared reload/quit outcome. Ordinary native
  knockout must be distinguished from death rather than silently ending the run.
- **Cooperative downing and revival.** An otherwise lethal, survivable injury
  leaves the affected player at 1 HP and downed. They cannot act or take a turn
  until a teammate revives them or the fight resolves. This applies to each
  roster actor, including the host; there is no hard-coded first/second-player
  role. Gruesome fatal injuries, such as a hole blown through the character,
  remain actual death and cannot become a revivable knockout.
- **Corpse and gear with respawn.** Death leaves the player's carried items
  behind, preferably on a persistent lootable corpse. The player can return
  after a configurable delay, potentially at the current map's entry point.
  Leaving and rejoining must retain the death state and remaining delay rather
  than grant an immediate return or restore the dropped inventory. Respawn keeps
  the player's character progression; any starter equipment requires an explicit
  policy. Gruesome death can prevent revival while still allowing respawn in this
  mode. Its corpse or remains must retain recoverable gear if that is the selected
  rule, even when the native death art destroys the body. Repeated deaths can
  leave many corpses on screen at once. Each body keeps its own entity identity,
  remaining inventory, and death appearance; looting it does not automatically
  erase it. Corpses are ordinary world entities, never additional active players.
  Any eventual cleanup limit must be explicit and preserve gear and quest items.

The host must classify the native lethal outcome before committing death flags,
death scripts, corpse conversion, kill/XP credit, or queue removal. Fallout has
explicit death animation IDs such as `ANIM_BIG_HOLE`, chunks, slicing, burning,
explosion, and melting. Use an explicit fatal-outcome table tied to the native
result, including art fallback behavior. A client animation or blood preference
must never decide whether someone survives. Do not classify every critical hit
as fatal. Poison, radiation, scripted kills, friendly fire, and repeated hits on
an already-downed player need explicit rules too.

Native knockout is not a complete downed system. `set_new_results` schedules an
automatic knockout wake-up, `combat_over` wakes only `obj_dude` in its native
cleanup, and the main loop treats that actor's knockout as game over. Keep
ordinary timed knockout distinct from cooperative downing, and route player
outcomes through roster IDs. The native timer must not revive a downed player
before the chosen fight-end/revival rule permits it.

Implementation choices still to settle before the corresponding behavior:
revival action, range, AP/resource cost and restored HP; recovery after combat;
what happens when everyone is downed; whether one gruesome death ends the party
or lets survivors continue; and how hazards affect downed actors. For respawn,
settle the delay and clock, entry-point selection and blocked placement, restored
HP/status, whether combat must finish first, and what happens when everyone is
dead or the party changes maps. A shared-map game must not send one player to a
separate map implicitly. Corpse persistence, teammate looting, repeated deaths,
and quest-item access also need explicit rules. No bleed-out timer is implied.
These remain design proposals, not implemented behavior.

Persist the mode, actor state, and outcome revision in authoritative snapshots
and saves, including respawn eligibility/deadline and corpse identity when used.
Replicas display the same status without running damage/revival
rules. Validate revive requests against the living acting player, target state,
range, current turn, and resources; replay cannot revive or charge twice. Test
host and guest separately, multiple downed actors for future four-player play,
last-active-player loss, fatal animation fallback, combat end, travel, save/load,
and disconnect/reconnect during a downed, terminal, or pending-respawn outcome.
For respawn, transfer carried inventory to the corpse exactly once on the host;
rebind the stable player identity to the new actor without reusing the corpse as
an active player. Preserve item identity and quantities through corpse looting,
respawn, replay, save/load, and map changes. Test simultaneous deaths and staggered
respawn for every roster member, including future four-player sessions. F09 stays
unchecked until its chosen behavior and recovery tests pass.

Current implementation must keep these extensions possible. Player identity,
character progression, current body, downed state, and terminal party outcome
are separate concepts. Use roster iteration and host-authorized state changes;
avoid embedding a mandatory game-over decision inside low-level damage or
inventory code. Save/recovery must eventually represent multiple old bodies
without treating them as player slots. Keep map-replacement pointer rebinding
separate from respawn, which requires a new body entity while retaining the old.

### M3. Finish shared story presentation and durable outcomes

Implement F04 and F11. Audit scripted map changes, party placement, timed escape
sequences, movies, dialogue-to-combat transitions, quest rewards, and queued
callbacks as a single story flow. Revalidate after callbacks that can delete
or move an actor, target, or item. A cutscene must not starve network polling.

Preserve native numbered saves and the hidden recovery transaction. New
progression/presentation fields need bounded, validated encoding and migration
tests. Older sidecars must not infer missing entitlement data by blindly
regranting past levels. Stage all changes before replacing live state.

Gate: both clients recover before and after a quest reward, map departure,
timed escape, and story movie without duplicate rewards or replayed world
effects. Real completion and failure outcomes reach both clients.

### Future option. Who chooses dialogue replies

Add a multiplayer session setting with these user-visible choices:

- **Party vote:** eligible connected players vote on each reply. Document the
  tie and timeout behavior; keep the current voting behavior until this option
  is implemented.
- **Conversation starter decides:** the player who initiates the conversation
  chooses its replies. This means the talker, not whichever participant clicks
  an answer first. Other players can watch without being required to vote.
- **Host decides:** the host chooses replies even when another player starts
  the conversation. Other players can watch without being required to vote.

The vote controller already has `TalkerDecides`, `HostDecides`, and majority
policies. Expose and connect these through session setup instead of inventing
another decision mechanism. Synchronize the setting from host authority and
persist it in saves/recovery. Freeze it for an active conversation; apply any
session setting change at the next conversation. Show who controls replies.
Use roster/player IDs and test with two, three, and four participants.

Acceptance includes guest-initiated dialogue under every mode, conflicting
choices, equal votes, timeouts, the deciding player disconnecting, reconnect,
save/load, quest rewards, and dialogue-triggered combat or map changes. Define
and test the fallback when the deciding player leaves. Execute each selected
reply once on the host. This is future implementation work requested during
visible testing, not a change to the current session's rules.

### M4. Run and repair the full content campaign

Start region checks while M1–M3 land, but do not declare completion until their
gates pass. Use native saved checkpoints for diagnosis; the final acceptance
run starts with new characters and uses normal UI and travel. Rotate the
initiator/talker/looter between HAL and codex-testbox. Include failure and hostile
branches by saving first. Keep audio muted and capture only useful evidence.

The main acceptance route returns the water chip, resolves both mutant threats,
and reaches the ending. Both orders of the two mutant objectives need tests;
alternate completion without first returning the chip should be checked
against the installed data's behavior. The quest dependencies are documented
in the [water-chip reference](https://fallout.fandom.com/wiki/Find_the_Water_Chip),
[mutant-source reference](https://fallout.fandom.com/wiki/Destroy_the_source_of_the_Mutants),
and [mutant-leader reference](https://fallout.fandom.com/wiki/Destroy_the_Mutant_leader).
Exact deadlines and script variants must come from the matched installed data.

| Region/checkpoint | Required multiplayer cases |
| --- | --- |
| Vault 13 entrance, Vault 13, Vault 15 | New/custom characters, guest first combat, bodies/resources, Pip-Boy/automap, ropes, floors, return dialogue, native save/load. |
| Shady Sands and Raiders | Actual radscorpion quest completion and reward; antidote; Tandi rescue routes; successful/failing skill hooks; theft, barter, hostility, recruited Ian. |
| Junktown | Killian/Gizmo investigation and opposing quest branches; recruited Tycho/Dogmeat where the data supports them; hotel/rest, merchants, group combat. |
| Hub | Merchant stock and caps, ammo and loaded weapons, quests and reputation, caravan departures/encounters, water delivery and shared deadline consequences. |
| Necropolis and water-chip return | Pump repair success/failure, quest-item ownership/transfer, chip acquisition, timer-dependent changes, return reward, Pip-Boy objective update, save/recovery around turn-in. |
| Glow and Brotherhood | Radiation and timed recovery, Rad-X/RadAway, holodisks/information, elevators, computer/skill gates, armor acquisition/repair, guest initiating on another floor. |
| Boneyard | Multiple connected maps, power-regulator/hydroponics routes, gang/settlement outcomes, Followers, Katja, high-stock merchants, difficult mixed-weapon combat. |
| Cathedral | Disguise/perception, Nightkin encounters, conversation and combat routes to the Master, computers/locks, timed destruction/escape, movie and outcome. |
| Military Base | Force fields, elevators, computers, Lieutenant encounters and alternate routes, vats destruction, timed escape, movie, both objective orders. |
| Final outcome and failure paths | Actual chosen settlement slides, departure and credits on both clients; water deadline failure, player loss, surrender/hostile branches supported by the installed data; terminal recovery policy. |

Gate: one complete two-player story run plus checkpoint-based alternate major
branches. Each listed region has an evidence record. Optional quests get an
explicit inventory and result rather than silently being considered covered
by a nearby generic fixture. Unsupported mod scripts remain outside the
matched base-data release claim.

### M5. Harden and release two-player v1

- Complete F12/F13 and close every P0/P1 finding or explicitly narrow the
  release claim with an agreed limitation.
- Run 3 CTest checks, 17 Python tool checks, all 50 installed-data scenarios,
  native inventory/loot UI tests, and the new advancement/combat-entry/timer/
  cutscene tests on the final build.
- Exercise delayed/disrupted connections during attacks, barter commits,
  rewards, transitions, save, and ending. Test malformed/stale commands,
  exhausted object/payload limits, and recovery snapshot failure.
- Run a long session with companions, many saves and revisits, large/nested
  inventories, chem withdrawal, poison/radiation, and repeated map switches.
  Record crashes, first divergent checksum section, packet/apply timings,
  missing presentation, and item/XP conservation failures separately.
- Build with multiplayer disabled and check ordinary single-player saves,
  menus, character editor, and native combat. Validate Linux and the Debian
  testbox builds; validate other platforms before claiming support.

Release gate: both players can finish the actual game through normal inputs;
no known progression or authority blocker, resource duplication/loss, recovery
corruption, or missing terminal story flow remains in supported base content.

### M6. Expand to four players on its separate branch

Two-player acceptance does not prove four-player support. Reuse the completion
tests with roster-size variation, then replace the known pair-shaped transport,
lobby, runtime, elevator placement, remote-object save bridge, and acknowledgement
boundaries. Use independent peer streams, credentials, ownership, recovery
cursors, and disconnect state. Bilateral trades still have two participants,
chosen from the roster, rather than a hard-coded host/guest pair.

All M1–M5 additions must use registered player/actor IDs and ordered rosters,
avoid new host/guest field pairs, and encode bounded repeated state where it
belongs to players. Keep three/four-player controller tests even while the live
transport remains two-player. Preserve old-save compatibility or make a clear
versioned migration; never decode player three into the guest slot. Refer to
[the player-count audit](player-count-debt.md) for the existing expansion seams.

## Gameplay acceptance matrix

| System | Coverage still required beyond current generic fixtures |
| --- | --- |
| Six combat skills | Small/Big/Energy Guns, Unarmed, Melee, Throwing; both actors; aimed and burst attacks, grenades, criticals, armor/lighting/range, death and friendly fire. |
| First Aid and Doctor | Success/failure, healthy and injured targets, crippled limbs, exhausted daily limits, time cost, self/other player, save between uses. |
| Sneak, Lockpick, Steal, Traps | Independent timed Sneak and script queries; real locks/jamming; scripted theft refusal/caught/success; real trap disarm and explosive timer outcomes. |
| Science and Repair | Real scripted success/failure gates, consumed tools/components, locks/force fields/computers, quest rewards and map mutations. |
| Speech, Barter, Gambling, Outdoorsman | Talker stats and branch votes; varied merchants/stacks/containers; gambling caps and actor identity; encounter avoidance/travel perks and both travel controllers. |
| Character growth | Custom/premade characters, skill costs, level grants, perk prerequisites/choices, Gifted/Skilled/Educated and Tag/Mutate where supported, cancel/commit, persistence. |
| Items and resources | Containers/nesting, caps distribution, quest-item handoff, books, holodisks/radios, sensors, armor, all reload/unload/partial-clip cases, active items, steal/barter restrictions. |
| Time and health | Drug addiction/withdrawal, poison, radiation, rest/healing, timed quests, explosive/flare timers, map changes and recovery with active effects. |
| Story and party | Shared globals/reputation, acting-player rewards, recruited companions, floor separation, scripted departures, failed quests, movies, actual final ending. |

Every content test records the initiating player, expected native/cooperative
behavior, authoritative outcome, replica presentation, resource/XP delta, and
save/reconnect result. A failed attempt that never reaches a callback is not
evidence that the skill or quest works.

## Validation from this review

- Current multiplayer build: all 3 CTest checks and all 17 Python tool tests pass.
- F05 / F06 regression gate: all 18 default installed-data scenarios pass in one
  uninterrupted run at `/var/tmp/fallout-steal-sneak-default-20261001`.
  This includes ten native Steal script cases across both actors and scoped
  Sneak queries. Final multiplayer CTest is 3/3; the multiplayer-disabled
  build and its 2 CTest checks also pass. The full 50-scenario campaign and
  content-specific stealth/delayed-script branches remain unchecked.

- F08 regression gate: all 18 default scenarios pass uninterrupted in
  `/var/tmp/fallout-automap-default-20261001`. Focused checks cover native
  scanner stack splitting, ownership/replay, empty/unequipped sensors, markers,
  charge descriptors, and combat AP. The final native UI run verifies separate
  floors, repeated input, empty refusal, recovery save, and reconnect. Enabled
  CTest is 3/3; disabled compilation and CTest are 2/2. Full fresh-session disk
  reload, campaign travel, and the expanded 50-case release gate remain open.
- A fresh multiplayer-disabled build and its 2 available CTest checks pass in
  `/var/tmp/fallout-multiplayer-review-off-build-20261001`. Configuration required
  the same local SDL dependency prefix and architecture include path as the
  existing build. This proves compilation/tests, not a full solo playthrough.
- The latest prior 18-scenario pass is preserved in
  `/var/tmp/fallout-steal-approach-verified-20261001`, with final focused native/
  recovery checks in `/var/tmp/fallout-steal-approach-last-20261001`.
- All 47 installed-data scenarios were exercised across three batches: **45
  passed and 2 failed** (`skill` and `combat-turn`, F14). The runner stops at
  its first failure, so temporary continuation runners exercised the remaining
  cases without changing repository test behavior. This is not a passing
  uninterrupted full campaign. The combined record, including evidence roots,
  is `/var/tmp/fallout-multiplayer-completion-review-full-20261001/combined-summary.tsv`.
  An isolated skill retry also failed. Symbol-enabled diagnosis is recorded in
  `/var/tmp/fallout-review-skill-debug-host.log`; that fresh multiplayer-enabled
  Debug build compiled successfully. All automated and remote tests use dummy
  audio.

## First implementation slice

First resolve F14 and rerun the uninterrupted expanded gate. Then start F01:
per-player advancement bookkeeping and a host-validated character
edit commit, followed by a native UI test that earns/spends points for both
characters and reloads them. Then add normal guest combat initiation and the
guest-on-another-floor cases in F02. These are the first two campaign gates;
more generic parity testing cannot substitute for them.


### Additional review findings during companion acceptance

- Native duplicate-party cleanup treated dead, unscripted Ian/Tycho/Dogmeat/Katja bodies as duplicates and erased them on map load, including their loot. Dead companions now bypass duplicate cleanup; native corpse aging still controls their lifetime. The native corpse save/load/reconnect test verifies a dead, non-party Ian with pistol, ammo and caps preserved on both clients. This does not implement player respawn or permanent corpse retention.
- Duplicate cleanup erased objects while traversing the native object's list iterator, then dereferenced the freed list node. It now collects pointers before removing duplicates. Native party loading also bounds its serialized member count before indexing the fixed 20-entry list.
- The build-45 uninterrupted 50-scenario matrix and 17 Python checks passed. The subsequent corpse/iterator fixes require the build-51 rerun now in progress before the final gate can be checked.
- Muted HAL/testbox sessions were updated together (role90), with prior save directories preserved. SLOT09 loaded with host HP43 and guest HP31, poison/radiation unchanged at zero. A separate SLOT10 checkpoint was saved through the native menu before further cave testing.


### Coordinated acceptance work

Visible playtesting owns the muted HAL/testbox role-91 sessions on build 54.
Companion battle development owns the native companion fixture and runtime
changes. The protocol reviewer owns transport queue bounds and focused core
checks. The coordinator owns integration, plan updates and final release gates.
Session control and source ownership are kept separate to avoid competing input
or edits. Agent results require logs and scope-matched evidence before a goal
is checked off.

Build-54 ON CTest passed 3/3; the disabled build and CTest passed 2/2; Python
tool checks passed 17/17 in `/var/tmp/fallout-five-goals-python54-20261001.log`.
These checks precede the new companion and transport changes. The full guest
story watch and latest corpse recovery evidence above have passed. Native
inventory release coverage is being rerun; full region acceptance remains open.


Native editor release checks passed on copied build 54 in
`/var/tmp/fallout-five-goals-native-editor54-20261001.log`. Copied build 55 passed
native inventory context use/drop/unload/reload and following combat in
`/var/tmp/fallout-five-goals-native-context55-20261001.log`, and nested inventory
navigation/use/unload during the guest's combat turn in
`/var/tmp/fallout-five-goals-native-combat-container55-20261001.log`.
The first combined context/nested run had incompatible fixed quantity assumptions;
the separate runs prove their respective paths. An earlier combat-container run
failed a guest drag before combat, while its rerun passed; this remains a test
reliability observation rather than evidence of a fixed game defect. Timeout
reports now retain both screens and current world records for diagnosis.

Disabled build 54 also ran ordinary single-player new-game UI at Vault 13,
character editor and Pip-Boy, native combat entry, and native numbered save/load.
The isolated artifacts are `/var/tmp/fallout-singleplayer-off54-20261001`, including
SLOT04 `OFF54 native solo gate` and the `Game Saved.` journal record. This proves
these ordinary UI paths, not full solo campaign or every combat weapon.


The first verified checkpoint commit is `86717fc`, bounding TLS outbound/inbound
queues and preserving pending TLS write buffers across retries. The focused core
gate covers packet-count and byte pressure, ordered read resumption, explicit
blocked-peer disconnect and certificate-pinned fresh connection. Additional
uncommitted changes remain under native and broader regression testing.

Recruited Ian's battle and corpse acceptance passed on build 64 in
`/var/tmp/fallout-companion-battle-corpse64-20261001.log`. Native AI fired a
pistol, reducing ammunition from 12 to 11 and a radscorpion's HP from 26 to 22.
Shared travel, native death, disk load and authenticated reconnect retained the
corpse's resources and matching digest `10643866795214206692`.

Native retaliation also passed in
`/var/tmp/fallout-companion-hostility64-20261001.log`. Ian responded to the guest's
attack, consuming two rounds and causing 15 damage. His installed script has no
damage or combat procedure that dismisses him, so the test preserves native party
membership rather than inventing a dismissal rule. Disk load and reconnect retain
HP 49, ammunition 10, caps 163 and digest `7839476183656194111`. These fixtures
validate Ian's lifecycle; they do not complete the cave quest or all companions.

Commit `3313ab4` fixes recursively freed container items retaining stale network
identities. Map loading now restores the destroyed peer body's stable actor entry
for its replacement. Focused map/recovery gates and all 50 build-62 scenarios
pass. Commit `9795f7c` clamps multiplayer Home camera movement to a legal center
near map borders. Both HAL and testbox visibly recentered at the previously
failing cave edge and at ordinary interior positions. Visible sessions are on
build 62 and continue the actual radscorpion quest.

Commit `eff1b2b` retains a bounded outbox of unacknowledged guest commands across
authenticated reconnect. Commands replay with their original IDs, so a request
lost before receipt can execute and an already executed request returns its
recorded result without consuming resources twice. Actual TLS tests cover both
loss boundaries, a subsequent command, and the 64-command bound. The isolated
commit builds and passes all three CTest checks. Build 64 also passed all 50
compatibility scenarios in `/var/tmp/fallout-five-goals-regression64-campaign`.
Native disrupted-save and full-content gates remain open.

Commit `2caedf1` rejects checksum-valid snapshots with non-critter player/NPC art,
non-critter NPC prototypes, or positions outside the native 40,000-tile grid.
The regression tests preserve legal edge tile 39,999, death animations and facing
bits. Before the fix ten new assertions fail; the isolated corrected commit
builds and passes all three CTest checks. Native final regression remains pending.

Native snapshot rejection passed all three cases on copied build 66: same-revision
phase mismatch, older phase revision and wrong actor ownership before map loading.
Each preserves the complete digest and registry size with no script, combat or
random execution. Movement, elevator, shared map transition and fresh native
recovery all pass in `/var/tmp/fallout-release-review-focused66-20261001`.
Commit `9cc29a6` moves those checks ahead of native mutation; `26cccfe` keeps the
owner test compatible with existing valid player IDs.

Commit `30fa624` defers queued-event owner flags until the entire replacement
passes validation and allocation. An invalid later event now leaves the old
queue and earlier owner flags intact. The isolated commit builds, passes all
three CTest checks and both native movement peers in
`/var/tmp/fallout-verified-queue-campaign2-20261001`.

Build 67 passed the ten-family native weapon matrix, both players attacking
with SMG, assault rifle, minigun, grenade, rocket, laser/plasma pistols, flamer
and laser/plasma rifles. Native ammunition/AP, unload/reload conservation and
friendly extras agree; the guest executes no rules. See
`/var/tmp/fallout-native-weapons67-all-20261001.log`. Native ending/reconnect,
scripted defeat during barter quantity input with terminal reconnect, and combat
Take All also pass in the build-67 story, terminal and loot logs. Weapon setup
pads native health and skills explicitly; it is acceptance of weapon mechanics,
not ordinary campaign acquisition or story completion.

Build 72 passed native capture/apply, disk save/load and authenticated reconnect
on Gun Runners, Hub Downtown, Necropolis Hall of the Dead, Mariposa storage and
the Cathedral. All snapshot sections, timer owners and final digests agree.
See `dense-map-acceptance-2026-10-01.md` and
`/var/tmp/fallout-native-dense-maps72-20261002.log`. These fixtures use approved
native arrivals; they do not prove ordinary quest completion on those maps.

Commit `cf5f99e` discovers script-created NPCs, supported scenery, ground items
and nested resources during host capture. Actual TLS reconstruction and
reconnect preserve the created objects and native timer without guest rule
execution. Commit `719e0cf` deduplicates native iterator visits before initial
registration and discovery. Gun Runners previously failed below capacity because
its first occupied tile was returned twice; the corrected native gate passes.

Commit `1dcd388` resolves native prototype names through the acting player.
Visible Natalia dialogue previously introduced her as Max Stone. The installed
Ian script obtains the name through `proto_data`, which bypassed the guest
context. Native guest-name acceptance on build 71 now introduces Smoke Guest
and receives Ian's reply with the same name; lifecycle and recovery also pass.
Visible campaign confirmation after deployment remains pending.

Commit `483e80f` reports buffered native file-close failures before publishing
save metadata. The real Linux `/dev/full` probe reproduces the prior false
success and proves subsequent normal I/O remains usable. Ordinary native save
and authenticated recovery pass in the isolated checkpoint build. Further review
found the private save-file copying helper also discarded its final output-close
result; its native kernel probe now reports failure and preserves normal copying.
Partial backup-rename failure and complete failed-save rollback still require
fault-injected acceptance before the disrupted-save release gate is closed.

Visible ordinary progress is preserved in SLOT09, `Shady cured safe six kills`,
and a separate backup. Six radscorpions were killed and looted; three remain.
Natalia earned the real Razlo antidote and Jarvis cure rewards, 250 and 400 XP.
Both players rested to full native HP, 49 and 36, with zero poison. Ian remains
unrecruited in this ordinary run. The name and post-rest host HUD fixes are
awaiting deployment; the full cave quest and full-story content gate remain open.

Commit `57c98b7` checks output-close failure in the native map/automap copying
helper. The old implementation reports success copying four buffered bytes to
`/dev/full`; the fixed implementation returns failure and copies normal bytes
intact. Commit `76d5f87` uses relative database paths and copies slot backups
without moving the original files. Failed backup creation now aborts saving.
The native private-helper probe covers an unavailable backup destination,
successful SAVE.DAT/map/automap backup, and restoration after simulated partial
native output while retaining multiplayer metadata. The old code fails the same
backup/restore assertions. Isolated build and CTest pass 3/3; actual native disk
load/authenticated reconnect also pass in
`/var/tmp/fallout-verified-save-backup-campaign-20261002`.

Candidate 73 builds with multiplayer enabled and disabled, with CTest 3/3 and
2/2 respectively. Muted HAL/testbox role 93 restored ordinary SLOT09 on matching
source and agree on both actors' resources. Natalia's actual Ian reply now says
`So, Natalia, what can I do for you?`; the name regression is visibly resolved.
Post-damage shared-rest HUD acceptance remains pending. Candidate 73's native
capacity-stop fixture found save preparation could still mutate the stopped
world before the later save rejection. Early native-save rejection and stopped
simulation guards are in progress and are not yet accepted.

Pinned build 72 completed all 50 live compatibility scenarios plus the core gate
with no failures and both peers exiting zero throughout. All 28 emitted final
digests match; 22 scenarios do not emit a final digest and are not counted as
independent digest witnesses. Evidence is in
`/var/tmp/fallout-five-goals-regression72-campaign/summary.tsv` and `digests.tsv`.
This verifies build 72. Later capacity-stop and backup changes still need their
own focused acceptance and final-source release checks.

Commit `644c4a0` refreshes the host HUD after accepted shared rest. Ordinary
role-93 testing confirms native HP 37 to 49, poison 10 to zero, and actual HUD
49 after the cave fight. Natalia remains at 36 and Ian heals from 46 to 50.
The party killed all nine cave scorpions, received the native 500 XP reward and
Seth's completed-quest dialogue, then saved SLOT09, `Nine scorpions cleared healed`.
Ian's native AI finished the three remaining scorpions after player attacks.
Exact journal chronology corrected an earlier attacker attribution; this does
not claim the guest fired when her turn had not begun.

Commit `b96cf03` makes native queue traversal safe when a callback removes other
events, including a retained earlier node. AddressSanitizer reproduces the old
use-after-free on the committed baseline. All four native regression cases pass:
next-node removal, prior-node removal, rescheduling and retained equal-time order.
New node identities stay in memory and do not change native save or wire formats.
Both enabled and disabled native queue probes pass; isolated build and CTest
pass 3/3.

Commit `abe01f0` stops commands, native simulation and saving after a definitive
checkpoint-capacity failure, while busy animations still retry. Native build-75
acceptance proves poison timers work before the failure, pending animation and
script/combat/random execution freeze afterward, shared rest stops, and the last
four saved files remain byte-identical. Leaving the stopped session permits
native load and timer progression again. The guest disconnects normally after an
explicit native-ready witness. The isolated production commit builds, passes
CTest 3/3 and native disk/authenticated recovery with both processes exiting zero.

Native shutdown fixes are accepted and committed. Commit `a06ad57` removes the
exact selected script slot rather than resolving its potentially duplicated SID;
`d8886bc` retains newly allocated human script identities during party recovery.
Read-only build-78 native recovery observes 37 unique script slots after disk load,
including owned player SID `0x0400000F`, and both peers exit zero. Commit `9e2d498`
preserves a transport-disconnected guest after an Applied ACK failure, with six
negative core assertions reproduced before the fix. Commit `1a33d20` blocks pending
combat-script requests while simulation is stopped; native ASAN proof still lets
an ordinary elevator request reach its selector.

Commit `b101957` preserves the genuine companion script while removing duplicate
objects, visits each native object pointer once and removes all owner timers before
freeing objects, including nested inventory. Canonical build 79 passes actual Ian
recruit/dismiss/recruit, two orphan SID cases, Junktown/Shady travel, disk load and
authenticated reconnect with matching digest `11010798490602263099` and clean exits.
Active guest rules remain zero in both sessions. Private negatives reproduce
orphan-script retention, dangling nested timers and iterator use-after-free;
the fixed equally instrumented ASAN build passes. Native setup mistakes and the
inactive native baseline load are excluded from multiplayer authority claims.

Commit `09f2d0c` makes failed native rollback retryable. A nonempty map destination
makes the old native restore consume backups and prevents a second restore. The
fixed path retains every backup; removing the obstruction restores SAVE.DAT,
map, automap and unchanged multiplayer metadata. Isolated build/CTest pass 3/3.
This closes this rollback slice, not the complete interrupted-save release gate.

Candidate 80 builds with multiplayer enabled and disabled and passes CTest 3/3
and 2/2. Exact-slot cleanup and stopped-combat native ASAN probes pass. A visible
Pip-Boy alarm refresh correction awaits matched deployment and real healing
acceptance. The build-78 campaign used only the default 18 scenarios, all passed;
it is not counted as a full run. The complete 50-scenario build-79 run is underway.

Build 79 completed all 50 compatibility scenarios and the core gate, with clean
zero exits throughout and all 28 emitted final digests matching. The remaining
22 scenarios do not emit a digest. Evidence is
`/var/tmp/fallout-five-goals-regression79-full/{summary,digests}.tsv`.

Commits `6d9e7f2` and `4c9d651` protect retrying failed native saves and multiplayer
metadata publication. A later save must successfully restore pending native
backups before replacing them. Metadata backups use MULTI.OLD, outside the native
map-backup wildcard; legacy MULTI.BAK is recovered before native backup cleanup.
The permanent native rename-failure probe verifies original metadata survives
failed publication, failed restore and repeated retry, then publishes exact bytes
when I/O recovers. A destination directory preserves the backup, and a legacy
backup is retained until recovery succeeds. All nine assertions pass on combined
81 and on the isolated committed production build. Isolated build/CTest pass 3/3.

Commit `54b8a26` validates required native prototypes before map, local-variable
or entity mutation. Checksummed native checkpoints containing missing creature,
scenery or item prototypes are rejected with unchanged digest/registry and zero
native execution. Combined 81's movement and disk/authenticated recovery pass
with both processes exiting zero. This closes the missing-prototype rejection
case, not allocation or placement rollback across the entire application path.

Matched muted HAL/testbox role 94 now runs pinned source 81, 306 source/config
hashes equal across platforms, restored to ordinary Junktown. Both native builds
retain saved level-three HP 55/41, Ian 50, zero poison and weapon/consumable/cap
resources. The full candidate-81 regression and actual HP-changing alarm redraw
acceptance remain pending. Source bundle is
`/var/tmp/fallout-visible-pinned81-src`; HAL binary SHA256 is
`b6b3c9ba2a7e56861375997b10f8f567911ebcdef4e743b6ac57dec8a0d4ae7d`.


Accepted follow-up checkpoints, 2 October

- [x] Builds 81 and 83 complete all 50 compatibility scenarios plus the core gate, with both processes exiting zero throughout. Each emits 28 matching final digests; 22 scenarios emit none. Evidence: `/var/tmp/fallout-five-goals-regression83-full/{summary,digests}.tsv`.
- [x] Commit `69a7529` rejects invalid native global/layout counts before local-variable resizing or entity mutation. Four invalid checkpoints preserve state; legitimate local expansion applies and restores the original digest.
- [x] Commit `a02bb5b` preflights required actor art frames and catalog bounds. Invalid art/frame checkpoints preserve state; actual fallback animation and burned-corpse aliases apply and restore correctly. Build 83 passes all five dense maps and disk/authenticated recovery.
- [x] Commit `97a5f7b` re-resolves script slots after native procedures delete or compact them. Permanent native ASAN cases verify self-removal and retained source ownership without stale-pointer access.
- [x] Commit `81780cb` binds actual NPC companion ambient/timed callbacks to the shared leader while preserving human, unrelated and ownerless script contexts. Six permanent native ASAN cases pass. A real guest-approved ten-minute rest dispatches 600 queued Ian callbacks, all following the host and restoring the caller context.
- [x] Commit `7909f76` preflights stable, reconciled and newly created NPC frames using the same native matching rules as application. All three oversized-frame cases reject without mutation; unchanged hidden art and newly created frame-zero controls apply and restore. Isolated build/CTest pass 3/3; combined build 85 passes movement, disk/authenticated recovery and all five dense maps.
- [x] Commit `97f2c2a` preflights door, scenery and item frames using shared native reconciliation rules and parent-first inventory ordering. Six invalid checkpoints preserve state; the final native door frame and hidden unchanged/new frame-zero scenery/items apply and restore. Combined build 86 passes movement, recovery, all five dense maps and TLS script-created object synchronization. Allocation, placement and cross-map rollback remain separate open gates. An isolated production build passes these controls but crashes on guest shutdown; its missing ground-item detach dependency is being investigated before isolated recovery acceptance.
- [ ] Accept the visible Pip-Boy proposal layout and already-open alarm HP/time refresh on matched build 85. The earlier rest completed after 57 seconds; a retry requested a second rest, completing after 90 seconds. No approval failure is established by those observations.

Visible build 81 ordinary Junktown testing completes Kenji's assassination encounter, Killian's gratitude and investigation assignment. Natalia earns native 400 quest XP and 250 combat XP; both peers agree on the death and rewards. The host receives the bug and tape recorder. This is additional story acceptance, not completion of the full campaign. Build 85 source and binary are pinned at `/var/tmp/fallout-visible-pinned85-src` for the next safe matched deployment.

- [x] Commit `e6f1f1c` preserves failed native rollback across process restarts. SAVE.RBK records the validated backup count before rollback erases partial outputs. Saving and loading recover that record before using the slot; malformed or blocked records refuse another save without removing backups. The native test executes a fresh child process, confirms all three backups survive, then removes the obstruction and restores the old native files and metadata. Isolated build/CTest pass 3/3 and disabled build/CTest pass 2/2; movement, native disk load and authenticated reconnect exit zero. The nine metadata publication fault assertions still pass. Evidence: `/var/tmp/fallout-verified-native-save-restart86-20261002.log`, `/var/tmp/fallout-verified-native-sidecar-rollback86-20261002.log`, `/var/tmp/fallout-verified-persistent-rollback-recovery86-20261002/summary.tsv`.
- [ ] Full interrupted-save publication remains open. SAVE.RBK protects rollback once it begins; it does not yet guarantee atomic publication across native files and metadata if the process dies during initial saving. Platform crash durability and power-loss ordering require additional work.

- [x] Commit `9cd6fa5` places shared-rest proposal text below the fixed HP redraw strip and uses a roster-neutral cooperative instruction. Matched muted build 85 visibly shows Max's ten-minute proposal on both peers; both journals agree on pending duration and proposer, then clear after guest approval.
- [ ] The same commit adds local alarm HP/max-HP/time refresh. Actual already-open page healing acceptance remains pending; the ten-minute visible rest began at full health.
- [ ] Private ordinary Tycho dialogue reproduces a deferred hostility attribution bug: Natalia insults him, but the later native critter callback targets Max. A fix must retain initiating-player provenance without changing shared companion following or explicit attack targets. Native VM flags reset on map/disk reload; provenance must follow that lifetime.

- [x] Commit `286bc5a` supplies the floating-item detach prerequisite for ordinary ground-item reconstruction. The isolated new frame-zero item control had passed before a shutdown crash; GDB identifies duplicate list nodes reaching `obj_remove` during `game_reset`. With detach, isolated build 90 passes all controls, movement, native disk load and authenticated reconnect, both peers exiting zero. Evidence: `/var/tmp/fallout-verified-ground-save-recovery90-valid-20261002/summary.tsv`.
- [x] Commit `9664697` records recovery before live checkpoint writes, backs up previous metadata with the native files, and commits only after closed native output and matching sidecar publication. Native tests execute a fresh process before the first rollback, recover previous native/metadata bytes together, refuse missing metadata backups without mutation, retain records for corrupt/blocked recovery, recover empty first saves, and preserve newly committed bytes. Slot browsing recovers before reading partial headers. Cleanup now validates and checks every SAVE.DAT/*.SAV deletion, including blocked directories omitted by native file enumeration. Isolated enabled/disabled builds pass 3/3 and 2/2; actual movement/disk/reconnect pass with clean exits. Nine metadata failure cases still pass. Evidence: `/var/tmp/fallout-verified-native-save-transaction90-20261002.log` and `/var/tmp/fallout-verified-native-sidecar-transaction90-20261002.log`.
- [ ] Power-loss durability remains open. Checked flush/close and atomic recovery-record rename support process interruption; file and directory fsync ordering and platform crash tests are not established.
- [x] Ordinary visible build 85 Tycho training and recruitment complete. Natalia's evening Outdoorsman rises 2 to 7 while INT stays 9, receives Nuka-Cola, and uses her real name. Max remains at Outdoorsman 11. Killian's naturally started investigation opens Tycho's recruitment; Tycho joins at 60 HP. Native movement then shows both Ian and Tycho following Max while Natalia remains separate. The safe checkpoint is 'Tycho trained and joined'. This does not accept insulting dialogue or deferred hostility attribution.
