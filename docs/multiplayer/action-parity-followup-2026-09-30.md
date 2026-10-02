# Multiplayer action follow-up

Branch: `multiplayer-plan`, in `/home/jwagner/Development/side_projects/fallout1-ce`.
These changes build on the September 29 review. The installed TTS game is not
replaced. Remote tests use separate game directories on HAL and codex-testbox.

Earlier visible testing reached Shady Sands through ordinary play on both
desktops and verified the connected session with audio muted. Weapons, reloads,
body/container loot, Stimpaks, Pip-Boy, maps, and shared Katrina dialogue were
exercised. The final production build passes 17 campaign scenarios plus the
corrected recovery scenario, all 3 CTest checks on both platforms, and 17 Python
tool tests. Remaining parity gaps are listed at the end.

## Fixes

| Action | Fix |
| --- | --- |
| Sneak shortcut and skilldex | Submit a self-target skill command instead of changing a guest flag locally. The host validates the actor and executes the toggle. Exploration and combat are supported. Combat accepts only self-target Sneak on the acting player's turn with the current turn revision. Toggling clears movement as native play does. |
| Sneak timed checks | Evaluate the queue event's actor and store its result in that player's build. Each player owns its own timer. Guests do not perform gameplay rolls. |
| NPC perception | Select the observed player's build when evaluating Sneak. A guest's stealth no longer uses the story player's result. |
| Running | Host movement chooses a valid animation, respects crippled legs, and publishes its actual running decision. Running without Silent Running cancels Sneak. Native run registrations recognize either player's build. |
| First Aid and Doctor limits | Keep separate per-player daily histories. Resetting one character clears that character's history. Native timing and three-use rules remain in effect. |
| Skill-point edits | Select the explicit actor before checking and spending points. Editing the guest no longer requires the caller to have selected the guest beforehand. |
| Addiction | Track personal addictions in each build, including the native shared Beer/Booze category. A host's addiction does not suppress guest addiction rolls or change guest recovery. Correct the legacy check to read addiction globals rather than drug prototype IDs. |
| Withdrawal | Restore the recipient's context during delayed callbacks and use the recipient's Chem Reliant/Flower Child adjustments. Guest recovery clears the guest's addiction state. |
| Corpse and ground-container looting | Open the accepted loot window on the acting client. Route unpickupable containers through Loot, run container hooks only on the host, reject locks, and retain equipped objects while checkpoints arrive. Target-side LOOK and individual transfers work without detaching the player's cached weapons. |
| Combat inventory Drop | Route the inventory menu through host authority. Preserve chosen quantities and cap amounts, register ground items, remove equipped armor bonuses, and charge no extra AP after opening inventory. |
| Explicit actor queries | Select the recipient for stat increments/decrements, weapon range/AP/called shots, and perk skill bonuses. Guest traits no longer depend on an outer host context. |
| Poison and radiation | Apply resistance, poison ticking, radiation checks, damage and healing to the affected registered player. Keep per-owner timers and pending radiation flags; replicate numeric poison and radiation without changing NPC behavior. |
| TLS send retries | Retain a stable pending record across WANT_READ/WANT_WRITE retries. Appending new snapshots no longer changes the retry length or skips bytes in the encrypted stream. Real backpressure tests cover thousands of frames in both directions. |
| Checkpoint event boundaries | Track validated received state separately from confirmed application. Skip events covered by a checkpoint, retain acknowledgement until application, and recover from the actual applied cursor with queued unapplied state cleared. |
| Initial world-map display | Reveal the party's current area on the authoritative host when the map opens, then replicate discovery. Restore the native idle hotspot marker. Stop native animations before suspending map background updates, so lingering fidgets cannot block the snapshots that carry the revealed terrain to the guest. A completely unexplored cave save no longer opens onto an entirely black terrain panel. |
| Timers across map changes | Preserve guest drug, withdrawal, poison, radiation, and wake timers while replacing the peer actor, retaining their absolute due times and payloads. Run native map-exit effects for carried items before detaching inventory, then restore surviving inventory timers with their live item owners. |
| Travel character rules | Evaluate Outdoorsman, Pathfinder, Luck, Explorer, and Scout using the player controlling the approved route. Host simulation no longer implicitly substitutes the host character for the guest controller. |
| Pip-Boy health and rest availability | Read the local player actor for alarm HP and location-dependent rest availability. The guest no longer sees the host's HP or uses the host's location to enable the alarm. |
| Guest-controlled world travel | Have the authoritative host stop blocked routes and completed routes outside cities, regardless of which player controls the map. Publish the route-clear event so a blocked guest route cannot repeatedly advance game time. |
| Cave exits | Recognize native world-travel destinations -2, -1, and 0 in automatic exit detection, consent classification, host execution, and scripted scenery departures. The actual Vault 13 cave exits use -2; the former map-zero-only check trapped both players at the brown exit. |
| Host Take All | Keep processing the current inventory index after synchronous host transfers remove a stack. Both the knife and ammunition now transfer instead of skipping every second stack. |
| Loot window lifetime | Close stale native loot windows when phase, registered actor/target identity, ownership, or adjacency changes. Keep equipped items in authoritative inventory and filter them from the displayed inventory, including after leaving nested containers. |
| Saved multiplayer games | Support ordinary host save slots through the multiplayer lobby, validate the sidecar before touching native state, and reject guest or active-session loading. Corrupt metadata cannot silently fall back to single player. |
| Wounds after loading | Apply saved builds before refreshing host stats, preserving wounded HP, poison, radiation, and crippled limbs. |
| Home camera | Center directly on the local multiplayer actor so cave scroll blockers do not leave the player off screen after walking or loading. |
| Native pickup/container animation | Correct the inverted art pointer check that dereferenced a failed load, and release the pickup animation art lock after reading its action frame. |
| Native timed-event recovery | Retain serialized owner IDs until the independently saved guest and inventory are attached. Rebind guest timers after recovery. Reject malformed queue types and preserve the old queue on decode failure. |
| Disabled text-to-speech | Skip Speech Dispatcher initialization when speech is disabled. The remote interactive test exposed a startup hang in that connection even with speech disabled in the config. Enabling speech later opens the connection lazily and can retry failed connections. |

Character state now includes healing history, Sneak results, and addictions.
Snapshot version is **21**, gameplay wire version is **36**, and new multiplayer
saves use sidecar version **5**. Versions 1 through 4 remain readable and default
the new fields. Both peers must run the updated game build.

## Verification

- Build succeeds, all 3 CTest tests pass, and all 17 Python tool tests pass with `PYTHONPATH=tools`.
- Snapshot and save tests check state roundtrip, legacy defaults, negative
  healing timestamps, malformed Sneak booleans, and invalid addiction bits.
- The native player-rules fixture checks independent healing limits, guest
  skill-point edits under a host context, and addiction separation, including
  the Beer/Booze category. The live inventory action fixture toggles Sneak on
  both peers with host authority.
- The 18-scenario compatibility campaign passes, including movement, combat,
  dialogue, timed rest, transitions, and recovery. Logs are under
  `/tmp/fallout-more-parity-campaign-20260930`. A combined rerun with combat Sneak, combat Drop, actor query fixes, and native timer recovery also passes all 18 scenarios in `/tmp/fallout-full-review-campaign-20260930`.
- Visible two-machine tests ran with HAL hosting on its physical desktop and
  codex-testbox joining on its VNC desktop. The updated inventory-action test
  finishes with matching state digest `10651644247956401401`; the guest runs
  zero scripts, damage attacks, and gameplay RNG calls. It preserves 60 rounds
  and the native inventory combat AP charge.
- A normal interactive multiplayer session also starts successfully on the
  corrected builds: Max Stone on HAL and Natalia on codex-testbox share the
  Vault 13 map. Both journals agree on the actors and turn state during natural
  rat combat. The live playtest uses normal movement and combat commands. Max fired his pistol to kill a rat, consuming one round, and Natalia equipped her knife. Rat corpse looting returned no items. The Pip-Boy status and Automaps opened; the water-chip deadline reads 150 days. Natalia killed a rat with two knife attacks and used a Stimpack. Max emptied his pistol and reloaded it for two AP. Both used the native automap to find the cave route. A native two-player save was completed before updating the visible clients. Both players subsequently reached Shady Sands through ordinary world travel. Test audio is muted.

- A native combat Sneak fixture passes with unchanged AP and two guest cues.
  Host/guest digest is `14349700109601458185`; logs are in
  `/tmp/fallout-combat-sneak-review-20260930`.
- Native combat Drop checks ordinary stacks, caps, armor removal, stale
  revisions, excessive quantities, and closed-inventory rejection. Both peers
  match digest `17924329273457068063`; logs are in
  `/tmp/fallout-combat-drop-20260930`.
- The strengthened native recovery fixture restores the guest's active Sneak,
  drug, and withdrawal timers, personal healing history, and addiction state.
  Both peers match digest `1284964983152228689`; logs are in
  `/tmp/fallout-recovery-review-final-20260930`. The queue decoder fixture also
  verifies missing actor and inventory owner binding, rebinding an inventory
  timer that initially resolves to a reused map ID, and atomic rejection of
  invalid event types. The expanded fixture passes in
  `/tmp/fallout-combat-sneak-review-20260930/{host,guest}/queue-collision-recovery-fixed.log`.

- Final focused checks for loot, inventory actions, container transfers, and
  native recovery pass on the frozen build. Logs are in
  `/var/tmp/fallout-review-final-targeted-pass-20260930`.
- The native guest loot UI fixture opens the cave Bones container, shows target
  LOOK text, drags its knife into the player's inventory, takes the remaining
  ammo, and verifies locked-container rejection. Both peers then complete
  combat with matching digest `11543255289527559611`; the guest executes zero
  scripts, attacks, or gameplay RNG calls. Logs and screenshots are in
  `/var/tmp/fallout-native-container-loot-yv_oyc5t`. Reproduce with
  `python3 tools/test_native_multiplayer_container_loot.py build/fallout-ce /path/to/game-data`.
- Final wire 35 builds are staged on HAL and codex-testbox. The Debian native
  build and all 3 CTest checks pass. The final visible check uses preserved
  native cave and Shady Sands saves.

- The combined cave-exit, save/load, loot, and camera build passes all 18 native compatibility scenarios in `/var/tmp/fallout-final-cave-exit-campaign-20260930`. Native logs explicitly confirm an accepted map -2 exit proposal. All 3 CTest and 17 Python tests pass.

- Latest ordinary-slot and recovery native pairs pass with matching digest
  `8225171871586248670`. The fixture restores HP 17, poison 7, radiation 11,
  and a crippled arm on the authoritative host. It also rejects active-session
  loading and corrupt metadata without changing native state. Logs are in
  `/tmp/fallout-combat-sneak-review-20260930/{host,guest}/{ordinary-slot-fixed,wounded-recovery}.log`.
- Native host Take All transfers both original stacks and converges at digest
  `3329600879030557364`; guest LOOK, drag and Take All converge at
  `16709483951651394801`. An additional native test closes an open loot window
  automatically when its actor is moved out of range. Artifacts are in
  `/var/tmp/fallout-native-container-loot-fc0b_bjt`,
  `/var/tmp/fallout-native-container-loot-u9orupa7`, and
  `/var/tmp/fallout-native-container-loot-0wffvjmg`.
- Actual play reached the cave's brown exit at tiles 25557/25558. Parsing the
  unchanged native save confirmed destination map -2 on all 14 exit grids.
  Natalia walked back to the Vault door and looted Bones through the native UI,
  collecting a spare knife and 24 rounds of 10mm AP ammunition. The original
  wounded-host save is preserved for verifying the corrected restoration.

- The new guest-controlled travel fixture uses the real native walkmask and confirms blocked and terrain-arrival routes clear, publish a clear event, and cannot advance time again. The full guest-controller reconnect/arrival pair passes with digest `14661921872815994391`; the guest executes zero scripts, attacks, and RNG. Logs are in `/var/tmp/fallout-native-container-loot-2b8bwrrc`. All 3 CTest checks also pass on this final travel build.

- Native movement and guest-controlled travel pairs pass with the controller rules fix in `/var/tmp/fallout-controller-rules-review-20260930`. The test installs guest Outdoorsman 100 and Pathfinder rank 2 under an outer host context, then verifies native day length 120 and travel time 3600 ticks.
- Visible ordinary main-menu Load restores the original cave save with host HP 27, guest HP 31, pistol ammunition 10, and Stimpaks 4. Home visibly centers Natalia after restoration: journal screen coordinates move from offscreen (-725,478) to (282,176). A surviving rat was killed with one pistol round, leaving both players at HP 27/31. Cave rest is correctly unavailable. The subsequent safe-town check confirms the corrected Pip-Boy HP row; native exit consent and travel also pass.

- The real native map-transition fixture preserves and rebinds all five guest actor timer types, checks exact due times, applies native wake-up behavior, destroys a carried flare safely before inventory detaches, and processes the due handlers afterward. Both peers pass the state-digest checkpoint exchange, with zero guest rules executed. Logs are in `/var/tmp/fallout-native-container-loot-jgfddzny`.

- The native renderer test starts with all exploration cells hidden, uses the production reveal/draw path, and checks current-cell discovery and over 1,000 visible map pixels. The host/guest travel pair passes with 20,996 visible pixels and matching digest `3424901506550766500`. Logs are `/tmp/fallout-combat-sneak-review-20260930/{host,guest}/worldmap-render.log`. The combined build passes all 3 CTest checks. All 18 native compatibility scenarios pass again in `/var/tmp/fallout-final-render-timers-campaign-20260930`.

- Visible restoration of SLOT03 retains HP 27/31 and nine pistol rounds. HAL now shows terrain and the Vault 13 hotspot; the initial guest fog problem was reproduced here and corrected in the final animation/capture-boundary build. Native travel exits the cave and reaches Vault 15 normally with resources preserved. Both players subsequently reached `SHADYW.MAP` through ordinary world travel. Natalia's Pip-Boy alarm shows her own 31/31 HP while Max has 43/43. Both consulted Status and maps, holstered Natalia's knife through the native inventory UI, and voted through Katrina's water-chip and equipment advice. Those dialogue branches award no XP. A fresh native SLOT04 and independent town backup preserve both players at tiles 13113/13112. The muted clients remain visible in town. Days of normal travel healed Max; this follows the earlier verified wounded restoration.

- The only equipment rejection during town testing was a stale automation entity ID reused after map load. Authority correctly rejected it; dragging the current knife in the native inventory UI succeeded. Screenshot `/var/tmp/host37-shady-final.png` records both players at the town entrance.

- Snapshot21 headless tests preserve separate nonzero host/guest poison and radiation, reject negative values on encoding and checksum-valid decoding, and detect actor-section drift. Native player-effect tests verify actual guest poison ticking, resistance, independent radiation scheduling/damage/healing, unchanged host stats/HP, numeric snapshot capture/apply, and unchanged NPC rejection. The full inventory-actions pair passes with digest `5524184697321064985` and zero guest rules in `/var/tmp/fallout-native-container-loot-mi8fnn0r`. Native map-transition timer checks also pass in `/var/tmp/fallout-native-container-loot-ctgfrfvb`.
- The world-map animation-boundary regression registers a real busy player animation, verifies authoritative capture is blocked, then calls the production preparation path and verifies idle capture succeeds and terrain renders. Both guest-controller travel peers pass with digest `583504442300124211` in `/tmp/fallout-combat-sneak-review-20260930/{host,guest}/worldmap-animation-boundary.log`.

- The earlier wire 35 / snapshot 21 build passes all 18 compatibility scenarios
  in `/var/tmp/fallout-final-wire35-campaign-20260930`. All 3 local CTest
  checks pass. The final native guest loot-to-combat pair exits successfully
  with matching digest `15635602781600446774` and zero guest gameplay rules
  in `/var/tmp/fallout-native-container-loot-9qtor77g`. Its setup now stops
  lingering animations before capturing the synthetic combat checkpoint;
  a networking-only wait did not advance those animations.
- Final staged HAL binary SHA256 is
  `1eb9211c78adc46534d91ae6ed81c2af5cebf9928ad3312aedb0e8f00420dac2`.
  The native Debian guest binary SHA256 is
  `8459d3a5301bd4da015e0a576a2577329efbe4a496c34f81061166064e71e66e`.
  Different platform builds use the same frozen source and protocol.

- Final visible wire 35 testing loads the preserved SLOT03 through the normal
  host menu, restores HP 27/31, and opens the world map without submitting any
  route. Both physical desktops show terrain on that first idle opening.
  The map interiors contain 39,586 host and 40,624 guest terrain-colored pixels;
  the previous guest panel was black. Screenshots are retained locally as
  `/var/tmp/host38-idle.png` and `/var/tmp/guest38-idle.png`.

- The final ordinary town reload exposed another disconnect after the characters
  loaded correctly. The guest reported an invalid packet, while the host still
  reported a connected session. The preserved town save and role 39 logs remain
  available. Read-only live diagnostics identify a raw zero-byte packet, not a
  checkpoint acknowledgement failure. TLS writes violated the library's retry
  contract by compacting/appending the outbound buffer and changing the retry
  length. A deterministic real TLS backpressure test reproduces corrupted frame
  bytes on the original implementation. The stable pending-write record passes the original backpressure
  regression. Local and native Debian CTest pass with one-way and bilateral
  4,096-frame stress tests. The actual failing SLOT04 now restores on both desktops and remains connected
  after a 30-second idle wait. Native guest facing, one-tile movement, Home,
  and Tab automap all work afterward. Both journals agree on Natalia moving
  from tile 13112 to 13111; HP remains 43/31 and resources are preserved.
  Role 40 logs and journals record this live verification. The final delivered-
  cursor build repeats the check in role 41 after 31 seconds, with matching
  native movement on both peers. Max retains nine loaded pistol rounds and four
  Stimpaks; Natalia retains twelve loaded rounds and three Stimpaks. Both
  clients remain muted and visible in Shady Sands. Original SLOT04 and backups
  are unchanged.
- Independent checkpoint tests expose a separate API gap when a complete state
  arrives ahead of events. The lobby now records the validated receive boundary,
  skips covered events, and acknowledges only after the engine confirms that
  the checkpoint applied. Tests cover stale/future/MAX rejection, queued and
  same-poll covered events, strict duplicates, and reconnect/reset behavior.

- The combined compatibility rerun caught a knife-combat regression. Its host
  passed and guest applied the final checkpoint, but the guest reported no
  observed turn events. Review identifies a regression in the new
  deferral predicate: combat events are delivered before their acknowledgement,
  so the applied cursor alone falsely identifies a delivery gap. A separate
  delivered cursor now preserves contiguous attack, turn, and equipment cues
  while acknowledgements still wait for the checkpoint. The focused regression
  checks delivery of two unacknowledged equipment events alongside their state.
  Both native knife-combat and inventory-action combat pairs now pass in
  `/var/tmp/fallout-final-combat-cues-targeted-20260930`. Knife peers match
  digest `14843959392692670043`, with the guest observing both player turn
  owners. Inventory peers match `5524184697321064985`, with two guest Sneak
  cues and passing poison/radiation effect and numeric replica checks. The guest
  executes zero scripts, attacks, and gameplay RNG. The final campaign passes
  17 scenarios, then catches unequal recovery state digests despite both local
  fixtures reporting success. Its guest captures immediately after restoration
  while the host captures two seconds later. Exact diagnostics prove all world
  state sections match, including actors, critters, inventory, timers, map, and
  game time. The guest captures event boundary 0 and host boundary 1. The test
  now verifies restored Exploration first, then uses the existing final Ending
  checkpoint and acknowledgement to compare a frozen common boundary.
- Final recovery passes with matching digest `16367991551162056949`, event
  boundary 2, phase revision 4, and game time 264600. Both peers verify wounded
  HP 17, poison 7, radiation 11, crippled-arm state, inventory, and timers before
  the final barrier. Logs and exact encoded snapshots are in
  `/var/tmp/fallout-final-recovery-boundary-20260930`. This completes all 18
  scenarios alongside the 17 passing cases in
  `/var/tmp/fallout-final-delivered-cursor-campaign-20260930`. The recovery fix
  changes only the smoke harness; the already verified live town41 production
  build is unchanged.
- The final harness-only binaries are staged as `.next` for reproduction, with
  HAL SHA256 `41da7320963a625d1294c782584dd290137aee354d8496ad0eb85132c55084c9`
  and Debian SHA256 `61beae691ef031958d653c493d8dd5c7af0b263ffa5f4c71a9c242e5ca47662d`.
  Local and Debian all 3 CTest checks pass. No further live restart is required.

## Solo code review continuation

The following fixes were reviewed and implemented without sub-agents on
`multiplayer-plan`:

- Loot distribution now applies only when taking loot. Depositing caps or items
  into a corpse or container honors that destination and leaves distribution
  priority unchanged. Taking caps still splits them, and taking corpse items
  still uses the configured priority.
- Generic loot rejects active NPCs and locked containers. An open loot modal
  closes if its target wakes or becomes locked. Player gifts use their own
  adjacency check, so this restriction does not block transfers to live players.
- Transfer and ground-drop commands require an existing authoritative item ID.
  Client descriptors cannot manufacture an item. Guests wait for host identity;
  hosts can register actual newly created inventory objects.
- Exploration transfers and low-level drops reject equipped or active gear.
  Explicit inventory equipment/drop actions retain responsibility for armor
  and HUD updates.
- Native nested-wallet spending now deducts an exhausted child's caps instead
  of adding them. Money-object allocation failure reports failure so the
  multiplayer transaction can roll back.
- Trade inventory acknowledgements wait for the authoritative checkpoint to
  apply. Previously a guest could acknowledge the trade before receiving its
  final items and caps, allowing the host to finish with the guest still stale.
- Snapshot validation rejects missing item holders, multi-item ownership cycles,
  and missing timed-event owners before native inventory traversal. Valid nested
  inventories and native door/scenery holders remain supported.

The complete 18-scenario compatibility campaign passes in
`/var/tmp/fallout-solo-authority-graph-final-campaign-20260930`. All three local
CTest checks and all three native Debian checks pass. Targeted trade and gift
checks pass in `/var/tmp/fallout-trade-checkpoint-authority-20260930`.
Native regression fixtures report:

```text
NATIVE_NESTED_CAPS_PASS initial=9 spent=7 remaining=2
NATIVE_LOOT_DIRECTION_PASS active_rejected=1 equipped_rejected=1 equipped_drop_rejected=1 caps_deposit=1 caps_split=1 item_deposit=1 item_priority=1
```

Native container UI testing also passes in
`/var/tmp/fallout-native-container-loot-v2ewqzt6`: knife drag, ammunition Take All,
empty-container verification, locked-target rejection, and subsequent combat.
Both peers finish with digest `17417293318364725773`; the guest executes zero
scripts, attacks, or gameplay RNG.

Updated binaries are staged as `.next`, without restarting the visible town41
session. HAL SHA256 is
`66c75a6561c6c821d6e55ded406cd3f0f16540757a87a64ec383dd38a240c109`;
codex-testbox SHA256 is
`73a6d6ecf70a2e5ad9e1a4f237416c2bcfe34e2a44cfecb47dad3927483a7335`.
That review kept wire version 35, snapshot version 21, and sidecar version 5 unchanged.

## Owned container transfers

Exploration inventory now sends transfers into and out of owned containers to
the host. Dragging onto a container deposits the selected quantity; using the
container opens its contents; dragging onto the body panel returns an item to
its parent inventory. Each nested level retains its own scroll offset.
Checkpoint updates validate the navigation chain before reading its pointers.
Equipping an item still requires moving it back to the player inventory first,
and container navigation now also works during the active player's combat turn.

The host checks ownership, authoritative item identity, quantities, and equipped
or active flags, and rejects moving a container into itself or a descendant.
Native host scripts retain their existing authoritative inventory path outside
inventory windows.

Actual guest UI testing exposed a snapshot restoration defect: items were
rebound by entity ID, so an older item could precede its newer container. Native
restoration now orders holders before their contents for both rebinding and
ownership application. That update preserved snapshot encoding and protocol versions.

Native host and guest UI verification passes in
`/tmp/fallout-native-inventory-jyxv9je0`. Both players deposit a knife, open the
container, withdraw to the parent inventory, return to the parent view, equip
and unequip, then complete knife combat. Both processes exit successfully with
digest `8303482296403498125`; the guest runs zero scripts, attacks, or gameplay
RNG. The complete 18-scenario campaign passes in
`/var/tmp/fallout-owned-containers-complete-20260930`, including dialogue and
recovery. The native authority fixture also passes stack splitting, nested moves,
withdrawal, and rejection checks with `NATIVE_OWNED_CONTAINER_PASS`.
All three local and three Debian CTest checks pass; all four campaign-tool tests
pass. Updated binaries remain staged as `.next` rather than replacing a running
game. HAL SHA256 is
`ddfc6e13bc3a486b4467e66bdde5c14d2b6ce6b34f16c6c0d88231d2be84264a`;
codex-testbox SHA256 is
`0b1b61f879813791e0ec97c32e8a013e9faec4cdd5d4733eeff71fa59a701839`.

## Contained inventory actions

The host now accepts use, unload, reload, and inventory drop actions for items
inside the acting player's containers. It checks the complete ownership chain
and performs stack operations on the actual holder. Drugs affect the player;
unloaded rounds stay in the weapon's container; explicit reloads draw from the
ammunition's holder. Contained items with equipped flags are rejected.

Combat navigation validates the player's turn through the root actor, rather
than treating a container as a combat participant. Existing turn-revision and
paid-inventory-access checks remain in force. Closing while inside a container
uses the player's equipment state. Navigation and contained actions add no AP
charge beyond the normal inventory opening cost.

Native host and guest testing passes in
`/tmp/fallout-native-inventory-egq_1kqn`: contained Stimpak use, pistol unloading,
ammo dragging and reloading, withdrawals, and following knife combat. The guest
also opens its container during combat, consumes its contained Stimpak,
unloads its contained pistol, and closes from inside the container. Its AP
changes from 10 to 6. Both processes exit successfully with digest
`16604506903707226548`; the guest runs zero scripts, attacks, or gameplay RNG.

The authoritative combat fixture also passes two-level nested use, loaded-weapon
stack splitting, reload, and contained drop. It verifies foreign-owner rejection,
round conservation, player-tile ground placement, and unchanged AP after the
opening charge. Latest focused artifacts are in
`/var/tmp/fallout-contained-actions-drop-final-20260930`, with matching digest
`14061166272396088596`. The full 18-scenario campaign passes in
`/var/tmp/fallout-contained-actions-complete-20260930`, including shared dialogue
and recovery. All three local and all three Debian CTest checks pass.
Latest `.next` HAL SHA256 is
`2d0b239c3f00dfa230b0683757754636d8ca9c208fba46591177d0952c673a74`;
codex-testbox SHA256 is
`9c4a28627aca61a59e2049e8db04756417312f93cc28d45f9511383aabbabb0d`.

The agent journal now reports contained inventories, holder IDs, and rounds in
ammo stacks. Native UI tests select rows from each peer's current inventory
rather than assuming checkpoint reconstruction preserves the host's row order.

## Combat container transfers

Owned-container transfers now work during the active player's combat turn after
inventory access has been paid. Transfer commands carry a 64-bit turn revision;
the host rejects stale turns, closed inventory access, other players' holders,
equipped or active items, and containment cycles. Moving items adds no AP cost
beyond opening inventory. Corpse looting and player gifts remain exploration
commands.

Gameplay wire version is now 36. Both peers must update together. Snapshot
version 21 and sidecar version 5 are unchanged. Transfer events now publish an
inventory checkpoint, and guests acknowledge after applying that checkpoint.

The focused native combat test passes in
`/var/tmp/fallout-combat-transfer-targeted-20260930`, with matching digest
`3243179000594910475`. It verifies stale-turn and closed-access rejection,
partial-stack withdrawal from a nested container, return to the original holder,
and unchanged AP. Wire tests preserve all 64 revision bits and reject mismatched
phases or a truncated command lacking the revision field.

Native host/guest UI testing also passes in
`/tmp/fallout-native-inventory-x94fe97h`. During its combat turn, the guest
withdraws a contained Stimpak to the player inventory, deposits it again, uses
it, unloads a contained pistol, and closes from inside the container. AP remains
6 after the inventory opening charge. Both processes exit successfully with
digest `16925422663240834353`; guest scripts, attacks, and gameplay RNG stay at
zero. The complete 18-scenario campaign passes in
`/var/tmp/fallout-combat-container-transfer-complete-20260930`, including
scripted combat, shared dialogue, and recovery. Local and Debian all three CTest
checks pass, as do the four campaign-tool tests.
The updated builds are installed in both remote test directories; HAL SHA256 is
`2291c4d2e87f78d711e9e26c0e8918ccf31df0c5c9575554557cf9043444da51`;
Debian SHA256 is
`ade7f1fbfebe73166c948471b31cb4978d7557d92cfcc6fcb93598c4494747f1`.

## Installed-build multiplayer test, October 1

HAL and codex-testbox started a fresh wire-36 multiplayer session with Max Stone
and Natalia. Both clients remain running in exploration with audio muted.
Their journals are copied to `/var/tmp/fallout-live-wire36-20261001`.

The live pair exercised walking and running, equipment selection, host and guest
pistol attacks, turn changes, three rat kills, corpse-loot UI, guest Bones
container Take All, and an adjacent-player ammunition gift. The guest acquired
one knife and 24 AP rounds, then transferred the AP rounds to the host. Journal
checks confirm the rounds exist only in the host inventory after the gift.
Both pistols reloaded through the HUD to 12 rounds. Host JHP reserves decreased
from 72 to 70; guest reserves decreased from 72 to 66, matching the rounds fired.
Final player positions, HP and AP match across both peers.

The native inventory UI rerun passes in
`/tmp/fallout-native-inventory-241zyd2_`, including host and guest contained use,
unload and reload, and guest combat withdrawal, deposit, use and unload.
Both processes exit 0 with digest `16925422663240834353`. Guest combat AP is 6
after the normal opening charge; guest scripts, attacks and gameplay RNG remain
zero. No new gameplay failure was reproduced in these checks.

## Shady Sands dialogue and skill tests, October 1

The visible HAL/codex-testbox pair loaded Shady Sands with Max Stone and Natalia.
Guest-initiated Seth dialogue displayed matching replies and choices on both
peers, including the doctor and radscorpion branches. Both players exhausted
Katrina's help topics. Completing the conversation awarded 250 XP; both native
character sheets showed 575 XP. Repeating the complete conversation produced no
second reward. SLOT05 preserves the pre-quest skill checkpoint, and SLOT06
preserves Katrina completion. Radscorpion and poison quests are not completed.

A guest Steal attempt reproduced a host-only native theft modal. Stealing now
returns an explicit unavailable message locally and is rejected by the host
command processor before execution. The native theft entry point also rejects
active network worlds, protecting direct script callers. A core test verifies
that rejection calls no skill executor and publishes no skill-start event.
This prevents misrouted theft; it does not implement multiplayer stealing.

First Aid and Doctor on healthy players returned their normal healthy messages.
On the existing wounded SLOT03 save, guest First Aid, guest Doctor and host
First Aid failed without changing HP. Host Doctor healed 5 HP, from 27 to 32;
guest First Aid subsequently healed 2 HP, from 32 to 34. After the asynchronous
callbacks/checkpoints settled, both peers reported the same HP. Healing tests
were discarded by restoring SLOT06. Sneak appeared on both native HUDs.

Guest Traps, Science and Repair on reachable scenery executed their native
fallbacks: no traps found, nothing learned, and cannot repair. Invalid critter
targets were also exercised. Lockpick on ordinary unlocked scenery produced no
native feedback or mutation. Attempts against an unreachable bookshelf did not
execute callbacks and are not counted as successful skill tests. Successful
lock picking, disarming a real trap, scripted repair/science success, crippled
limb healing and healing-use exhaustion still need suitable live fixtures.

Skill failure messages appeared on HAL but not on the guest HUD. Authoritative
HP updates work, but remote skill-result presentation remains a gap. Hostile
reaction after friendly fire was exercised from the checkpoint: Seth and
Katrina became hostile and combat refused to end. A tentative player exclusion
in combat_end did not resolve this and was removed; native hostile-NPC checks
are unchanged. The peaceful checkpoint was restored afterward.

Local and Debian builds and all three CTest checks pass. The 18-scenario campaign
passes in `/var/tmp/fallout-town-skills-campaign-20261001`. The final build's
focused skill fixture passes on both peers in
`/var/tmp/fallout-town-skill-fixture2-20261001`, including state-digest/replay
checks, with guest script, attack and gameplay RNG counters at zero. No gameplay
wire, snapshot or sidecar version change was required for the stealing guard.

## Still to verify or implement

Loot commands currently require adjacency; native automatic approach from a distant click remains a gap. NPC barter,
host-authoritative stealing, explosive timer choices, and complete combat presentation remain unfinished.
Legacy quest scripts that directly read shared addiction globals still require
review for their intended actor. Character advancement UI has further network
work even though explicit actor skill-point edits are now isolated.

The selected HUD hand is not saved in the multiplayer sidecar.


## Cave and antidote follow-up, October 1

Live two-player testing on HAL and codex-testbox found and corrected seven
additional issues on `multiplayer-plan`:

- Guest skill result messages now reach the acting guest. Healthy/self and
  crippled-limb messages use the acting character. This adds a validated
  recipient-specific feedback event and raises gameplay wire version to 37.
- Seth's scripted cave departure is deferred until dialogue and script scopes
  unwind. The host then loads and places both players, followed by a checkpoint.
- Large corpse adjacency accepts native distance zero in both host validation
  and the guest loot window. Fixing only host validation left the UI closing
  immediately. Both dead radscorpions now yield tails through native Take All.
- Snapshot replacement detaches stale NPC inventories before erasing owners,
  and destroys obsolete item trees from precomputed roots. This fixes the guest
  save-load crash reproduced under GDB at `inven_right_hand` from `obj_destroy`.
- Sorted exit grids have fixed IDs following the two player IDs. Allocator
  history after saved-map loading no longer makes guest exits reference missing
  host entities. Command rejection reasons are recorded in the agent journal.
- Closing shared world-map travel at a city's position returns a submap to the
  city's main entrance. Closing from the same main map preserves placement.
  The controller fixture now uses a terrain position for cancellation so it
  retains its installed NPC and item pointers.
- Host checkpoints register previously untracked native inventory rewards,
  including nested inventory items, before serialization. Razlo's first tail
  exchange previously consumed the guest tail and awarded XP but omitted the
  new antidote from the guest checkpoint. The antidote now appears on the guest.

The live cave fight exercised pistols, critical ammo loss, HUD reload, knife
attacks, punching, poison, and combat Stimpaks. Two of nine radscorpions were
killed. Native loot allocation gave one tail to each player. Both returned to
Shady Sands together through the guest's cave exit. The guest's first exchange
with Razlo consumed its tail, awarded 250 shared XP, and delivered an antidote.
Using that antidote on Jarvis consumed it and awarded 400 shared XP. The full
radscorpion extermination quest remains incomplete.

The native engine regression probe verifies a fresh, initially unregistered
antidote is included in a checkpoint with the guest as holder. The large corpse
probe covers overlap, adjacency, and distant rejection. Local headless campaigns
now default to SDL dummy audio. The native container UI test also normalizes
uppercase installed DAT paths to its lowercase isolated-copy filenames.


The host's second tail exchange delivered a second antidote without another XP
reward. Both native character screens show level 2 and 1,445 XP after the first
exchange, Jarvis cure, and second exchange. This agrees with 795 XP from the
prior checkpoint and cave kills, plus 250 and 400 quest XP. Only one player's
tail is consumed per exchange.

Both builds pass all three CTest checks. The final 18-scenario campaign passes
in `/var/tmp/fallout-script-rewards-final-20261001`. The native container UI test
passes in `/var/tmp/fallout-native-container-loot-gitp6hi3`, including drag,
Take All, locked rejection, later combat, and matching final state digests.
During an earlier long world-map test on a superseded build, the water deadline
produced an ending checkpoint with mismatched scenery counts on the guest.
That fatal-world-state reconstruction still needs a dedicated investigation.
Quest/item result text outside skill scopes can also remain host-only even
when inventory and XP checkpoints are correct.


Using the second antidote on already-cured Jarvis follows the native fallback:
it consumes the item, displays the use message, and grants no additional XP.
The host character screen remained at 1,445 XP. SLOT08 was saved before this
repeat test, and restored afterward to retain the antidote. SLOT07 retains the
cave crash/loot regression checkpoint. Character-screen evidence and compressed
live journals are in `/var/tmp/fallout-live-quest-evidence-20261001`.


The latest binaries were installed on both remote test copies and SLOT08 loaded
successfully on both peers. Both remained connected in Shady Sands, and the
saved antidote was restored to the host inventory. No local Fallout audio stream
remained after testing. No user installation or original save was modified.
After reload, guest-initiated Jarvis dialogue thanked the player for curing him, confirming the saved cure flag survived. Both peers closed that dialogue normally.


### Saved-terrain scenery and Ending reconstruction, October 1

The earlier water-deadline failure reported 87 authoritative scenery objects
against 78 on the guest's fresh terrain map. Replica checkpoint application
now reconciles non-door scenery populations using the existing PID, tile, and
elevation descriptors, removes obsolete scenery, and restores authoritative
entity IDs and presentation. Newly created objects are cleaned up if
reconstruction fails. Door population validation remains strict because its
snapshot does not carry a creation descriptor. No protocol change was needed.

The native guest execution probe adds two scenery objects, removes one, checks
the scenery digest, restores the baseline, and verifies zero guest scripts,
attacks, and random draws. The recovery fixture deliberately removes guest
scenery before the Ending checkpoint. Both peers accept Ending and restore
the durable save with equal state digests. The final focused movement/recovery
campaign passes in `/var/tmp/fallout-scenery-final-focused-20261001`. Both Linux
builds pass all three CTest checks. The original long water-deadline movie
sequence has not been replayed; the reproduction here covers its failing
scenery reconstruction and the Ending/save/recovery boundary.

Two-player reliability remains the scope of `multiplayer-plan`; four-player
work remains on its existing branch. Snapshot capture and exit proximity
checks use the session's registered players, and test entity IDs account for
all snapshot actors, avoiding new fixed two-player assumptions. The isolated
HAL and codex-testbox games were updated and SLOT08 restored in Shady Sands,
with SDL dummy audio.

The full 18-scenario campaign also passes in
`/var/tmp/fallout-scenery-reconstruction-20261001`. After the remote update,
guest-initiated Jarvis dialogue displayed the saved cure acknowledgement
identically on both peers.


### Guest action feedback and stacked flare use, October 1

Native item use, pickup scripts, and guest-initiated dialogue now capture
monitor messages for the acting player through the existing PlayerFeedback
event. Temporary local-player bindings used by native dialogue and inventory
helpers no longer mistake a guest for the physical host monitor. Message
eligibility includes the captured actor while palette fades and HUD updates
still follow the presented player. Skill feedback uses the same generalized
ScopedPlayerFeedback scope. Native probe coverage includes failed item use,
lighting a flare, a guest binding, nested host scopes, scope restoration,
and unrelated messages outside an action scope. No wire change was required.

Live remote testing confirmed the guest receives both “That does nothing.”
and “You light the flare.” It exposed stacked target-use lighting both
flares through one representative and one burnout timer. Flare activation
now detaches one item before changing its PID, then returns the lit item to
its holder. The native regression checks one unlit flare, one lit flare,
and a timer on the lit item. The plain fallback use message also copies
localized text with a literal printf format.

Both Linux builds pass all three CTest checks. The 18-scenario feedback
campaign passes in `/var/tmp/fallout-player-feedback-final-20261001`; the
subsequent flare fix passes native authority and Ending/save/recovery
checks in `/var/tmp/fallout-flare-stack-final-20261001`.

Stealing remains explicitly rejected by the command processor and native
steal inventory entry. Its per-drag roll, caught outcome, transfer, and XP
need an authoritative transaction protocol before the inventory modal can
be enabled. Native NPC barter is disabled in the shared-dialogue shortcuts
and lacks synchronized offers/acceptance; direct player trade is separate.
These remain larger feature gaps, not completed fixes in this pass.

After the final remote update and SLOT08 restore, the guest activated its
stack of two flares again. The connected guest checkpoint now contains
entity 1054, PID 205, quantity 1, and entity 1111, PID 79, quantity 1. Its
monitor received “You light the flare.” Both peers remain in Exploration
in Shady Sands. This confirms the stack fix through the actual asynchronous
target-use action, in addition to the direct native probe. Local tests and
both remote games used SDL dummy audio.

### Authoritative stealing, October 1

The stealing restriction described above is now replaced for adjacent living
NPCs in Exploration. The native inventory UI opens after an authenticated
Steal request. The host validates each drag, runs the native stealing roll,
applies a successful transfer, and awards the native progressive XP on close.
A caught attempt closes access, transfers nothing, awards no XP, and invokes
the NPC pickup script with the acting player as its source. Guest execution
does not rerun rolls or NPC scripts. Equipped and active items remain hidden
and cannot be transferred. Stolen items do not enter party loot distribution.

Access is stored per actor, with one actor per victim, and is cleared on
phase changes, disconnects, world resets, and target destruction. Object
destruction now removes registered critters and scenery as well as items,
avoiding stale victim pointers. This adds no fixed two-player access slots.
The gameplay wire version is 38; both peers require the updated build.
NPC barter is still disabled and needs synchronized offers and acceptance.
Steal auto-approach and its native fallback to corpse/container looting are
not covered by this implementation; ordinary corpse/container loot remains
available through its existing action.

Native tests temporarily stack the guest's Steal skill for success, then
restore a low-skill build for a caught case. They verify taking, planting,
stale quantities and equipped-item rejection before RNG, 30 XP on close,
XP feedback, reopening, no transfer or XP when caught, host RNG execution,
and access cleanup when the victim is destroyed. Test character changes are
restored afterward. Both Linux builds pass all three CTest checks, and all
18 compatibility scenarios pass in
`/var/tmp/fallout-theft-final-20261001`.

Visible HAL/codex-testbox testing in Shady Sands confirmed guest planting and
retrieving one Stimpak from Jarvis, with XP increasing from 1445 to 1475.
The first live close exposed a modal phase check that silently blocked the
close RPC; Theft close now uses Exploration. The host also loads the native
XP message independently of its own inventory UI. A HAL theft against Razlo
was caught, kept the host's Stimpak quantity at three, closed inventory, and
started combat on both peers. It exposed an incorrect capacity error after
the caught result; the UI now suppresses that fallback when authoritative
theft access has closed. The isolated SLOT08 checkpoint remains unchanged.
Local tests and both remote games use dummy audio.

The native inventory regression also passes in
`/tmp/fallout-native-inventory-fu1xzoow`, covering both peers' equipment
and container drags, contained drug use and weapon unload/reload, plus guest
container actions during combat. The guest executes zero scripts, attacks,
and RNG draws, and both peers finish with matching state digests.

After updating the remote pair with the final UI fix, HAL successfully took
Razlo's two Stimpaks and planted/retrieved its pistol. A later pistol plant
was caught and started combat on both peers. The caught monitor message now
appears without the false capacity error, and the caught transaction grants
no closing XP. The pair is restored from isolated SLOT08 after this test.

### Synchronized NPC barter, October 1

NPC barter now opens from the Barter button or B during shared dialogue.
The initiating player controls the offer while the other peer watches the
same item quantities and prices. The offer view never moves real inventory
objects into temporary tables. The host validates both offers on each edit
and again on acceptance, then commits the item exchange and net cap changes
through the existing atomic trade implementation. Cancelling or rejecting
an offer leaves inventory unchanged. Barter-enabled NPC flags are honored,
and an unsupported NPC returns the native refusal message.

Prices use the buyer's Barter skill, Master Trader perk, the merchant's skill
and reaction, and the script's barter modifier. Valuation includes weapon
ammo and the native full-clip/partial-clip distinction for ammo stacks.
Equipped, active, queued, and reserved merchant weapon items cannot be sold.
The host rejects stale revisions and edits or acceptance from another actor.
Cap balances are checked before mutation, and split remainders/new cap stacks
are registered before delivery, while rollback remains possible. Gameplay
wire version 39 carries separate NPC barter commands and result events.

Native scripted barter requests retain their acting player. Requests made
while a dialogue choice is executing wait for the next presentation before
opening, so a guest conversation executed on the host does not silently
switch to the host's inventory or build. The native fixture covers this
source binding and deferred opening. The negotiation is ephemeral; buyer
disconnection cancels it rather than saving uncommitted offers. Observer
disconnection does not cancel the buyer's negotiation. Ownership uses actor
IDs and the registered roster, with no additional two-player access slots.

Visible testing found two related dialogue defects. A fresh guest's initial
native dialogue state is -1, so the replica entry guard must reject only an
already-open state of 1. Both peers now show the first dialogue window.
The voting deadline also continued to expire during barter. Resolution is
blocked while barter is open, and closing it defers the deadline for 60
seconds without discarding ballots. A live 65-second negotiation returned
to the unchanged reply and revision on both peers, then exited normally
when both voted for No.

HAL/codex-testbox testing with Razlo verified a rejected 35-cap flare offer
against a 116-cap Stimpak price. A later accepted exchange gave Natalia one
Stimpak and 46 caps for her loaded pistol, valued at 259 caps. HAL's inventory
remained unchanged. Both peers showed identical offer revisions and prices.
Staging five caps and cancelling preserved all inventory quantities. HAL's
native Barter button also opened a host-owned negotiation, which cancelled
on both peers. The original isolated SLOT08 checkpoint was not overwritten.

Both Linux builds pass all three CTest checks. All 18 scenarios pass in
`/var/tmp/fallout-npc-barter-dialogue-final-20261001`. The final timer,
merchant refusal, scripted source, and disconnect checks run through the
native authority/recovery fixture, with the final focused run recorded in
`/var/tmp/fallout-npc-barter-scripted-final-20261001`. Native inventory tests
also pass in `/tmp/fallout-native-inventory-0f90k7on`, including both peers'
container and equipment drags, contained drug use, unload/reload, and guest
container actions during combat. All test processes use dummy audio.

The final updated remote pair also reopened guest-owned barter with matching
state on both machines, cancelled it, and completed the original dialogue
with matching No votes. Both peers are connected in Exploration in Shady
Sands with SLOT08's original inventory quantities restored and dummy audio.
Live barter and timer journals are preserved locally under
`/var/tmp/fallout-barter72-*-journal.jsonl` and
`/var/tmp/fallout-barter73-timer-guest-journal.jsonl`.

## Steal approach and loot fallback, October 1

Steal now performs native host movement into range before granting inventory
access. Walking or running uses the acting player's native movement rules.
The host keeps that player's context bound until movement completes, blocks
reentrant network command processing, bounds the wait, and checks entity
registration, elevation, range, actor activity, and phase again afterward.
It clears interruptible movement before opening inventory. Locked containers,
player targets, owned containers, and unsupported objects remain rejected.

Dead or unconscious NPCs and unlocked containers use the existing authoritative
loot executor. This preserves pickup script vetoes and ordinary loot transfer
rules without theft rolls or theft XP. An NPC becoming unconscious during
approach also takes this path. No new ownership slots or player-count constants
were added; access and presentation still resolve registered actor IDs.

Visible testing exposed an ordering error. The guest received the Steal event
while its actor was still at the old tile, rejected the presentation's range
check, and then received the correct position without opening inventory.
The runtime now queues Steal presentation until a covering authoritative
checkpoint has applied. Its acknowledgement waits for that checkpoint too.
Recovery and runtime reset discard pending presentations. Ordinary skill
events retain their existing behavior.

On HAL/codex-testbox, Natalia approached Razlo from eight tiles away and opened
her Steal window. Max then approached Jarvis from eight tiles away and opened
his own window. Both peers agreed on the resulting actor positions. A live
Bookcase approach reached the container but its pickup script rejected access;
that was retained rather than bypassing the script. Screenshots and journals
are preserved under `/var/tmp/fallout-steal76*` and
`/var/tmp/fallout-steal-barter76-*-journal.jsonl`.

The native authority fixture covers a six-tile approach with weapon art,
unconscious and dead targets, an unlocked container, and locked-container
rejection, alongside the existing successful, stale, equipped, caught,
XP, and destroyed-target theft tests. The fixture reports
`NATIVE_THEFT_APPROACH_PASS movement=1 weapon_art=1 unconscious=1 corpse=1 container=1 locked=1`.
The first fixture revision incorrectly required missing running art; native
running resolves the same art regardless of weapon code. That assumption
and the redundant animation changes were removed. The final native/recovery
run passes in `/var/tmp/fallout-steal-approach-last-20261001`; both Linux builds
pass all three CTest checks. The 18-scenario campaign is recorded in
`/var/tmp/fallout-steal-approach-verified-20261001`.

Additional visible barter testing sold two of Natalia's three ammo clips to
Razlo for 46 caps. The host quoted 150 against 46, both peers agreed on the
offer, the retained clip received its own registered entity ID, and HAL's
inventory quantities stayed unchanged. A later staged flare offer cancelled
when the buyer disconnected, leaving no active negotiation on HAL. Evidence
is in `/var/tmp/fallout-barter76-before-disconnect.json` and
`/var/tmp/fallout-barter76-after-disconnect.json`. The original SLOT08 was
not overwritten; the pair is restored from it after these tests.

This closes the previously listed range and body/container fallback gaps.
Script-specific Steal skill hooks still merit a separate review: active-NPC
theft does not yet dispatch the native USE_SKILL_ON hook before granting
access. More merchant types and a longer quest playthrough also remain.
