# Installed Ian battle and recovery checks

The companion driver recruits, dismisses and recruits the installed Ian through
native voted dialogue. It also requests his native weapon instruction, equips
his existing pistol, excludes human roster actors from the legacy NPC party,
and checks that companion following uses the shared host leader while the guest
changes floors. Tests use separate muted Xvfb sessions and copied game data.
They do not control the visible HAL/testbox game.

`tools/test_native_multiplayer_companion.py --battle` replaces the first travel
destination with the installed CAVES map, then starts combat with a real guest
attack on an installed Radscorpion. The host skips its player turns. Guest turns
end through the normal multiplayer command. Ian takes native NPC AI turns. The
fixture requires pistol ammunition consumption and damage to the real enemy,
then stops at a stable host turn after the guest acknowledges the checkpoint.
It returns to Shady Sands before native disk load and authenticated reconnect.
This is a bounded combat fixture, not a cave-clearing or quest-completion test.

The build-58 battle run passed on both clients:

- Driver log: `/var/tmp/fallout-companion-battle58-20261001.log`.
- Artifact root: `/var/tmp/fallout-native-companion-6949b1mi`.
- Ian's pistol went from 12 rounds to 10. The selected Radscorpion went from
  26 HP to 14. Ian retained 50 HP, team 0 and native party membership.
- Native map travel covered CAVES 16 and Shady Sands 26.
- Native disk load and authenticated reconnect retained Ian's HP, team,
  equipped pistol, loaded ammunition, carried pistol quantity, 163 caps and
  membership on both clients. Recovery digests matched at
  `1750455163429732563`.

The build-58 corpse run also passed:

- Driver log: `/var/tmp/fallout-companion-corpse58-20261001.log`.
- Artifact root: `/var/tmp/fallout-native-companion-qhgqj7os`.
- Native `critter_kill` produced a non-party Ian corpse with 0 HP, team 0,
  12 loaded pistol rounds and 163 caps. Both humans survived.
- Native disk load and authenticated reconnect retained the corpse and its
  resources on both clients, with digest `17131879866627759904`.

The final build-64 combined battle and corpse run passed on both clients:

- Driver log: `/var/tmp/fallout-companion-battle-corpse64-20261001.log`.
- Artifact root: `/var/tmp/fallout-native-companion-0inxrla2`.
- The installed Radscorpion went from 26 HP to 22. Ian fired his existing
  pistol through native AI, consuming one round from 12 to 11. He retained
  50 HP, team 0 and native membership through CAVES 16 and Shady Sands 26.
- Native death then left a non-party corpse with 0 HP, team 0, the same
  pistol with 11 loaded rounds, and 163 caps. Both humans survived.
- Native disk load and authenticated reconnect preserved those exact corpse
  fields on both clients. Recovery digests matched at
  `10643866795214206692`.

The driver checks matching recovery digests and requires a native combat pass
marker from each client when a combat mode is requested. Recovery assertions
compare the companion's exact HP, team, loaded weapon ammunition, weapon
quantity, caps and expected native membership. Gear preparation waits for an
existing animation to finish before calling native `inven_wield`; a failed
wield still fails the fixture.

## Ian's native retaliation policy

The actual installed `SCRIPTS/IAN.INT` was extracted read-only from the fixture's
`master.dat` using the repository's DAT directory and LZSS formats. Its SHA-256
is `0810b2e4e9955002139f50865b6e90ceb02a652ec04606199e0764283cb704b8`, and its
length is 11,646 bytes. The decoded procedure table has no `damage_p_proc` or
`combat_p_proc`. The table is recorded at
`/var/tmp/companion-installed-IAN-procedure-table.txt`.

Attacking Ian therefore does not establish a native requirement to dismiss him
or change his team. The native combat engine initializes `whoHitMe`, and
`ai_danger_source` selects that attacker even when they share a team. The
`--hostility` fixture tests this retaliation while preserving Ian's native
party membership and team. It uses actual guest attacks, native Ian AI, and
extra guest health only in the fixture so a missed punch can be retried. Its
pass condition requires Ian HP loss, pistol ammunition consumption, Ian's
native target identifying the guest, and guest HP loss. Disk load and reconnect
must preserve the post-combat HP, resources, team and membership.

The build-64 retaliation run passed on both clients:

- Driver log: `/var/tmp/fallout-companion-hostility64-20261001.log`.
- Artifact root: `/var/tmp/fallout-native-companion-c31ces82`.
- The guest damaged Ian from 50 HP to 49. Ian fired two native pistol shots,
  consumed ammunition from 12 to 10 and targeted the actual guest body.
  The padded guest dropped from 330 HP to 315.
- Ian retained native party membership and team 0 on both clients. Native
  disk load and authenticated reconnect preserved HP 49, team 0, 10 loaded
  pistol rounds, 163 caps and membership. Recovery digests matched at
  `7839476183656194111`.

The fixture waits for idle native player animations before its setup snapshot,
waits for the guest to receive the padded-health state before attacking, and
acknowledges the Exploration checkpoint after combat before starting recovery.
These waits prevent a rejected setup capture or a missed intermediate phase
from being mistaken for a companion failure. A native wield or combat failure
still fails the test.

These tests cover Ian. Other installed companions, delayed departures,
quest-specific loyalty branches, and a full content campaign remain separate
acceptance checks.

## Native duplicate cleanup acceptance, 2026-10-02

The explicit `--cleanup` fixture passed with canonical build 79. It first
recruits, dismisses and recruits the installed Ian through guest-initiated
native voted dialogue. After both peers capture an idle native checkpoint,
the host creates a labeled duplicate at tile 0. The native iterator visits
that body twice. The duplicate has a native bag, an ammunition stack of
quantity three and a lit flare with its real native timer, plus root script
and poison timers.
This is duplicate-state setup, not ordinary quest progress.

Two cases invoke actual native `partyMemberSave` and `partyMemberLoad`:
the duplicate owns a distinct native script, then a second duplicate aliases
the retained Ian's SID. Both cases remove the duplicate and all three nested
entity IDs, remove root and child timers, preserve the genuine Ian's script
owner, and preserve his native timer payload and deadline, HP and resources.
The distinct case also checks that the removed script cannot be looked up.

The canonical acceptance artifacts are:

- Driver log: `/var/tmp/fallout-native-companion-cleanup79-canonical-20261002.log`.
- Native logs and snapshots: `/var/tmp/fallout-native-companion-u_43agvp`.
- Copied engine: `/var/tmp/fallout-companion-pinned79-20261002/fallout-ce`.
- Engine SHA-256: `702e138ea1093f8fb7db3b3eb7911a00930f2f9517e327e69634cafaced7c8ba`.
- Both clients passed Junktown map 10 and Shady Sands map 26 travel, native
  disk recovery and authenticated reconnect, then exited normally. Their
  final full and section digests matched; the full digest was
  `11010798490602263099`.
- Both retained Ian at HP 50, team 0, 12 loaded pistol rounds, 163 caps and
  native party membership. The host retained owned SID 67126940. The guest's
  reconstructed body had SID -1, which is valid for a replica.
- Guest execution probes reported zero native script procedures, combat
  attacks and random draws in both active multiplayer windows: original
  dialogue/travel/session ending, and recovered state/reconnect/session
  ending. The recovery fixture's explicit single-player baseline map load
  between sessions is outside these probes.

The ordinary guest companion run on copied build 78 also passed with both
processes exiting normally. Its log is
`/var/tmp/fallout-native-companion-normal78-20261002.log`, artifact root
`/var/tmp/fallout-native-companion-zq9zdyo0`, matching recovery digest
`18035523220016940988`.

### Private native negative tests

The completed build-78 engine objects and archives were copied before further
shared builds. Only the corrected fixture runtime was replaced in that private
copy. An unchanged private link exactly matched the private positive engine's
SHA-256, `7c64e5f0cf7138db53d4dd7e500f5b1e064e0fd818039c4bc722468618cf7024`.
That full positive run exited normally on both peers with matching digest
`7705522473758270782`; log
`/var/tmp/fallout-native-companion-cleanup79c-private-20261002.log`.

Each negative changed one cleanup behavior in a private source copy; shared
sources and build outputs remained intact:

| Private change | Observed native failure | Driver log |
| --- | --- | --- |
| Restore the old distinct-SID clearing behavior | `sid_mode=distinct gate=retained_script_or_resources`; the orphan script remains registered | `/var/tmp/fallout-native-companion-negative-sid79-20261002.log` |
| Omit generic `obj_remove` timer removal | `sid_mode=distinct gate=dangling_timer`; the nested lit flare's timer remains | `/var/tmp/fallout-native-companion-negative-timer79-20261002.log` |
| Omit pointer deduplication from party cleanup | AddressSanitizer reports a heap-use-after-free in `partyFixMultipleMembers`, called by actual `partyMemberLoad` | `/var/tmp/fallout-native-companion-negative-iterator79-20261002.log` |

All three negative drivers exited 1 at those expected failures. The iterator
failure reads a body freed by native `obj_remove` during the first tile-0
visit. A matching private build with the same sanitized party compilation
and deduplication restored passed the entire fixture and both clean exits:
`/var/tmp/fallout-native-companion-fixed-asan79-20261002.log`, artifact root
`/var/tmp/fallout-native-companion-tko1u8wx`, digest `8170684656302164311`.
Only the party source was instrumented; leak detection was disabled. This
checks the invalid access and its correction, not whole-engine leak freedom.
Its engine SHA-256 is
`c2b02e885257816a74dd1faf2180dc5bb095c3d7baa2752b7ae21dba7b4c5917`.

Private sources, pinned objects, commands and binaries are retained under
`/var/tmp/fallout-companion-negative-pinned78-20261002`.

### Fixture corrections retained in the evidence

Early fixture failures were test assumptions, not accepted production defects.
Ian's prototype has no default SID, so the duplicate uses the retained native
script index to create its owned SID. Build 76's cleanup setup also omitted
`obj_disconnect` before native inventory insertion; its freed child objects
remained in the floating list and crashed the next map load. The corrected
setup detaches every child before insertion. Build 77 then reached a busy
player-animation capture immediately after dialogue; the fixture now retries
only `BusyObject` for at most five seconds and fails other errors immediately.
Build 78's final guest assertion incorrectly required a native script on the
reconstructed replica. The corrected assertion keeps the host's strict owner
check and permits an absent guest script, or requires a valid owner when one
is present. A first execution probe also included the inactive baseline map
load; the accepted probes explicitly measure active multiplayer sessions.

These corrections and failed artifacts remain under the corresponding
`/var/tmp/fallout-native-companion-cleanup76*`, `cleanup77*`, `cleanup78*` and
`cleanup79-private*` logs. The accepted checks did not force quest variables,
run companion scripts on the guest or bypass native cleanup.

## Guest name in native dialogue

The installed `TEXT/ENGLISH/DIALOG/IAN.MSG` has SHA-256
`073e4859bdc365eb9fad7e25da40aa5c59e4dbccf26c6d7d0ccc3dfcf0a84c07`.
Its introduction and reply strings supply fragments such as `I'm `,
`So, ` and `What can I do for you, `. They contain no predefined player names.
The installed Ian bytecode gets the name through `dude_obj`, `obj_pid` and
`proto_data` field 1. The PC prototype name path previously read the shared
host body despite the guest acting-player scope. The production fix uses the
acting player in that name path.

The build-71 guest lifecycle run passed with native name assertions:

- Driver log: `/var/tmp/fallout-native-companion-guest-name71-20261001.log`.
- Artifact root: `/var/tmp/fallout-native-companion-wn685enj`.
- Both actual dialogue journals and both native runtime assertions saw
  `I'm Smoke Guest.` and `What can I do for you, Smoke Guest?`, excluding
  `Smoke Host`. The driver selected the native introduction option through
  the usual two-player dialogue votes.
- Recruit, dismiss, recruit again, native gear, travel, disk load and
  authenticated reconnect passed. Both recovery digests were
  `10288845579586349245`.

## Timed following during shared rest, 2026-10-02

The installed Ian `timed_event_p_proc` calls `follow_player`. That procedure
reads `dude_obj`, writes its VM destination variable 16 and schedules another
one-second native script timer. Its instruction listing is retained at
`/var/tmp/companion-installed-IAN-follow-disassembly.txt`.
Ambient companion updates already selected the shared host leader, but
`script_q_process` previously inherited the current action's actor. The final
rest approver and the talker in time-advancing dialogue therefore determined
which human a timed follow procedure saw. The correction binds both acting
and local context to the host for NPC party script owners. The ambient and
timed predicates exclude null owners and human roster actors.

The before/after checks used separate private source, object and binary copies
under `/var/tmp/fallout-companion-timed-context-20261002`. The engine objects
and archives came from the pinned build-78 bundle. Only the private fixture
runtime and copied script dispatcher were compiled or replaced. Shared build
outputs and the visible game were untouched. The ordinary companion driver
recruited, dismissed and recruited the actual Ian through guest voted dialogue
before a labeled placement fixture separated the humans by 30 hex. No quest
variables or VM variables were supplied by the fixture.

The direct dispatcher check invoked actual `script_q_process` with Ian's SID
and follow parameter 1 under host and guest scopes. It read the installed
script's resulting destination, rather than supplying a movement target:

| Dispatcher | Caller scope | Destination distance from host | Destination distance from guest |
| --- | --- | ---: | ---: |
| Before correction | Host | 2 | 32 |
| Before correction | Guest | 28 | 2 |
| Corrected private copy | Host | 2 | 28 |
| Corrected private copy | Guest | 2 | 28 |

Each invocation executed one native procedure, scheduled the real one-second
follow timer and restored its acting and local caller scopes. The old
dispatcher's driver exited 1 at the guest-follow assertion, recorded in
`baseline-driver.log`. The corrected direct run exited normally on both peers,
then passed native travel, disk load and authenticated reconnect with matching
digest `3975317086569868646`. Its driver log is `fixed-driver.log`, artifact
root `/var/tmp/fallout-native-companion-a_be9ijs`, and private engine SHA-256
`73aaff8ae59d74211b7d1140418ff127e857a6cd3151f836a1baf4a337dbba13`.

The queued acceptance restored the captured queue after the direct diagnostic,
leaving exactly one follow timer created by Ian's installed script. The host
proposed the ordinary minimum ten-minute rest and the guest approved last
through TLS. A private observation callback forwarded every script queue event
to the original `script_q_process`, counting Ian's actual SID and parameter 1
without substituting script behavior:

- The pending timer was due at `264633`; rest advanced the native clock from
  `264628` to `270628`, exactly 6,000 ticks.
- Ian's native follow timer fired 600 times. Every destination stayed near the
  host, every handler entered with the guest acting actor, and every handler
  restored its acting and local entry scopes. The final destination was one
  hex from the host and 30 from the guest.
- The guest executed zero native script procedures, combat attacks and random
  draws during the rest check. Fixture positions were restored before travel.
- Both peers then passed Junktown map 10 and Shady Sands map 26 travel, native
  disk load and authenticated reconnect, and exited normally. Recovery digest
  `2716186453274564462` matched. Ian retained HP 50, team 0, 12 loaded pistol
  rounds, 163 caps and native party membership.

The queued driver log is `queued-rest-observed-driver.log` in the private root;
native logs and snapshots are `/var/tmp/fallout-native-companion-3sxhdblq`.
The final private engine SHA-256 is
`3291c08a8a2cb7da4f67779889feb8020cfe1b23a9f520a6f376813cabeda878`.

A separate private callback executable compiled the corrected native timed
dispatcher and observed its selected contexts. With a guest caller, NPC party
ownership selected the host. Host-human and guest-human ownership retained the
guest even when the test membership predicate classified those humans as party
members. Nonparty, null-owner and inactive-world cases also retained the guest.
All six cases preserved the timer parameter and restored nested and outer
scopes. The source, executable and log are `native-timed-owner-context.cc`,
`native-timed-owner-context` and `native-timed-owner-context.log` in the private
root. This is a dispatcher branch check with controlled owner predicates, not
an additional installed companion lifecycle test.
The later `native_timed_companion_context_test.cc` probe includes current
repository `game/scripts.cc` directly and replaces only the VM procedure call
with its observer. Actual `script_q_process` and `exec_script_proc` execute.
All six cases also passed with that current dispatcher compiled under
AddressSanitizer; `native-timed-current-source.log` records the result. Only the
dispatcher/test compilation was instrumented, and leak detection was disabled.

An initial queued fixture incorrectly requested one minute, which the native
rest command rejects. That failed log remains as
`queued-rest-invalid-one-minute-driver.log`; the accepted fixture uses the
existing ten-minute command without changing valid rest durations. The shared
dispatcher also handles timers pumped by native `game_time_advance`, but an
installed time-advancing dialogue and other companions remain separate content
acceptance checks.


## Native Tycho guest hostility attribution, 2026-10-02

A separate private native fixture reproduced an introduced multiplayer target-selection bug in the installed `TYCHO.INT`. Ordinary guest dialogue can set Tycho's hostility flag, but the subsequent NPC update attacks the host after the dialogue participant's actor scope has ended. This does not establish a bug in polite training, recruitment, dismissal or guest consent for rest.

The fixture first recruited, configured, dismissed and re-recruited actual Ian through the existing native TLS dialogue driver, then traveled normally to `JUNKCSNO.MAP` (map 11). Installed Tycho is PID `0x010000D2`, fixture entity 1307 and SID 67108865. His native schedule places him at tile 7000 outside bar hours 16:00–22:00, so the fixture used a real host-proposed, guest-approved `until_evening` rest. A native Tycho NPC update then moved him into tile 19690 at 18:00. Nearby human placement is an explicitly bounded fixture setup; no clock, quest, roll, script global or VM variable was forced to open the branch.

Both humans voted for actual guest talker 2 through these ordinary installed choices: `Sure.` → `So, what's your story?` → `That's a pretty long trip.` → `Why don't you?` → `Thanks for the tip, and the drink.` → `None of your business, you nut.` → `Bite me.` Tycho remained a nonparty NPC, with story status 121 initially 0 and Killian quest global 36 equal to 0. Native dialogue set VM variable 5 to 1. After `networkRuntimeProcessPendingTalk` returned, the acting actor was null and the local binding was the host. One real `exec_script_proc(Tycho, SCRIPT_PROC_CRITTER)` executed the installed hostility branch, reset its flag to 0 and requested combat against the host. A transparent observer in private `scripts_request_combat` recorded the actual defender before native processing; it did not substitute a target.

| Isolated native run | Native Tycho combat request | Artifact |
| --- | --- | --- |
| Current behavior, automatic negative | Host defender; guest offender ignored | `/tmp/fallout-native-companion-6p6_xt9u/host/game.log` |
| Private argument-provenance adapter | Guest defender; host binding preserved | `/var/tmp/fallout-native-companion-s2b0p5o0/host/game.log` |

Source review explains why existing fields cannot recover this actor: `gdialog_enter` invokes `SCRIPT_PROC_TALK` without `scr_set_objs`; `exec_script_proc` defaults a null target to the script owner and clears source after each procedure. `scripts_request_combat` only copies the attacker and defender that `op_attack` supplies. Rebinding every later ambient query to the last dialogue participant would also change unrelated movement behavior.

The private correction instead marks only a `ProgramValue` returned by `op_dude_obj`, using a default-neutral pointer-origin enum. The pointer itself stays unchanged. At `op_attack`, only a marked argument in a later NPC `CRITTER`/`TIMED` procedure can resolve retained dialogue provenance to the initiating `PlayerId`. An ordinary explicit pointer remains its actual target even when its address equals the default human. Provenance is keyed by the live `Program`, resolved through the current player roster, cleared after the first subsequent ambient procedure, and removed through existing program-reference cleanup. This is a private candidate, not a production change or a complete policy for every delayed script action.

Nine additional native callback checks passed in the positive run: an explicit host target with guest provenance; an explicit guest target with host provenance; a formerly implicit VM slot reused for an explicit target; an implicit guest target; cleared program references reverting to native fallback; and implicit values carried through actual VM local, global, swap and return-stack callbacks. Every check restored the VM stack size. These are labeled callback checks and do not claim additional ordinary dialogue outcomes. Read-only export review found whole `ProgramValue` copies for pointer export store/fetch; export behavior was not separately exercised in this fixture. Native map pointer lookups reconstruct neutral values in the private candidate.

The same positive binary also executed actual Ian timed follow under host and guest scopes with humans 30 hex apart. Each call executed one native procedure; both resulting destinations were two hex from the host and 28–30 hex from the guest. Nested and outer contexts restored, and the original event queue was restored before travel. Thus the narrow attack-argument adapter preserved the installed shared-leader follow behavior in this check.

Program-lifetime provenance matches the installed transient hostility flag: native `scr_save` writes a zero program pointer and does not serialize the live VM static variables. Reloading or replacing a program must discard both the flag and its actor provenance. The private reference-cleanup callback checks this discard, but a new disk-load/reconnect campaign for pending Tycho hostility was not run. Resolving arbitrary `PlayerId` values avoids a host/guest switch; this remains two-human native evidence, not a four-player acceptance claim.

Rest profiling in the negative fixture measured 38,340 actual Ian script callbacks during approximately 10.6 game hours of ordinary shared rest: 698,138 microseconds total in those callbacks and 717,705 microseconds for the whole rest. The positive candidate measured 675,051 microseconds in the same callback count and 736,563 microseconds for the whole rest. This isolated Xvfb result does not explain the visible campaign's 57–90 second intervals. `advanceSharedRest` is synchronous and does not yield transport/UI work at queue boundaries; any future progress UI should preserve callback order, random draws, exact native time and one authoritative rest result, and avoid reentering command processing. No callback skipping or rest performance change is proposed from this measurement.

Katja's installed `critter_p_proc` has a similar dormant hostility test, but this review found no installed procedure setting its VM hostile variable 0 nonzero. The visible recruitment and dismissal choices modify other variables. Ordinary Katja dialogue hostility is therefore unproven and must not be inferred from Tycho's result. Installed Dogmeat remains the `JUNKDOG` script: food use-on receives an explicit acting source, while unrecruited ambient leather-jacket recognition uses `dude_obj`; guest-only jacket recognition still needs a native reproduction. No additional Dogmeat bug is claimed here.

All changed C++ copies, the shadow VM header, pinned private objects and exact compile/link drivers are under `/var/tmp/fallout-companion-attribution-20261002`; no shared build or production C++ source was written. Negative source/binary copies are `negative-network_runtime.cc`, `negative-scripts.cc` and `negative-fallout-ce`. The clean automatic negative log is `tycho-insult-clean-driver.log`; the final positive log is `tycho-insult-tag-copy-controls-driver.log`. Private builders are `build-tag-private.py` and `build-tag-copy-private.py`. The driver now tracks votes by role, target and revision because dialogue revision numbers restart after map travel. These deliberately hostile fixtures clear the private combat request and stop before the existing travel/recovery continuation, so the legacy driver exits 1 by design; success here is the recorded native request and control results, not `MULTIPLAYER_SMOKE_TEST_PASS`.

Binary SHA-256: negative `02e57f4617906da89542a36aecdbafbec3ebdffc567b8caa2b4a9e218e99ff81`; final private tag/copy-controls candidate `8f0f6d9d78581e86e38df9ab294995c991a592eb0a6220ebda73097658f446ec` (`positive-tag-copy-controls-fallout-ce`). Failed setup fixtures retained their logs/journals; copied DATA from only `q79ekyf2`, `2ah5ihkx` and `lhxicyq_` was removed after `/tmp` filled. The negative and positive proof fixtures remain intact.

### Dialogue ordering and provenance lifetime controls

A second native negative established that the next ambient reaction cannot safely use whichever human talked most recently. In `/var/tmp/fallout-native-companion-qlbhahmf`, the guest took the installed insult branch, then the host completed a polite Tycho conversation before Tycho's next NPC update. The hostility flag remained 1, but the earlier last-dialogue adapter attributed the attack to the host. The native round-robin scheduler runs one critter script per update, so a second conversation can arrive before this NPC reacts.

The proposed ordering guard rejects another talk command for the same NPC while its live program has pending dialogue provenance. It returns the existing `InvalidAction` status before changing the session phase or shared modal. Other NPCs remain available. Capture is limited to a nonhuman critter owner in the native critter script list, with a valid CRITTER procedure. The lookup table is built before capture for a newly loaded program. Scripts with only TIMED procedures and scenery scripts are outside this policy, so they cannot acquire a pending record with no ambient expiry.

Two final private runs compiled the proposed production attack and dispatcher code against current source headers. They retained the transparent combat-request observer and the existing isolated fixture's dialogue/travel setup. In `/var/tmp/fallout-native-companion-hveo916y`, ordinary guest insults set Tycho's flag to 1. A host talk attempt was rejected while the phase stayed Exploration. One actual CRITTER procedure then reset the flag to 0 and requested the guest as defender, with the host's local binding and no acting scope still intact. In `/var/tmp/fallout-native-companion-2hwbn1q1`, a polite guest conversation left the hostility flag 0; the initial host talk attempt was rejected; one actual ambient procedure expired the pending actor without combat. A new host conversation was then accepted, and the host's ordinary insult branch produced a host-targeted attack. These are native conversation and reaction contrasts, not a complete travel/recovery acceptance run.

Both final binaries also retained actual Ian timer controls. With the humans 30 hex apart, separate host and guest timer dispatches each executed one installed procedure and chose a destination one or two hex from the host and 28–31 hex from the guest. Nested and outer actor contexts restored. The nine installed-program callback controls for explicit pointers, reused slots, cleanup and VM copies passed again. Final private binary SHA-256 is `819638808b32598f0f9130840cbe135704e773d7adb0e90992c55cb3791b34f2`. Exact logs are `candidate-guest-insult-driver.log` and `candidate-polite-host-insult-driver.log` under `/var/tmp/fallout-companion-attribution-20261002`. Each fixture deliberately stops after its hostile request and reports the legacy smoke failure; it does not claim the later recovery continuation passed.

Independent review found a further lifetime hazard in the first provenance candidate: a tagged value stored in VM locals or globals could outlast its dialogue record, then be mistaken for an implicit value from a later conversation. The reviewed candidate associates a monotonic generation with both the pending PlayerId and the tagged `ProgramValue`, and only rewrites an attack when both generations match. Neutral pointer construction and native map lookups have generation 0. Ordinary VM copies preserve the generation alongside the unchanged pointer. Clearing or consuming an origin does not make an old copied value current. The generation allocator survives session reset; saturation disables new provenance rather than reusing an old token.

`networkWorldLeave` clears all pending origins, and `networkWorldEnter` already calls Leave before starting another session. Ongoing-session reconnect and body replacement retain their PlayerId lookup. This metadata remains outside the native disk save and multiplayer wire/save schemas. The actual evidence remains two-player; no new pending-hostility disk load or four-player campaign is claimed.

The permanent regression is `tests/native_deferred_attack_context_test.cc`, with the dedicated runner `python3 tools/test_native_deferred_attack_context.py BUILD_DIRECTORY`. The test includes the current native VM and attack implementation directly, retains the native script registry and stack callbacks, and substitutes world lookup and the final combat-request observer. All 13 controls passed privately under AddressSanitizer, including the original nine and old local values across a new dialogue, old global values across session reset, live-program reset fallback, and renewed provenance. Every control restored both VM stacks. The exact private runner log is `native-regression-tool.log`; the runner creates temporary objects and its binary and does not rebuild the supplied build directory. Leak detection is disabled, and the linked engine objects are not ASan-instrumented.

The six-file production candidate was checked against an exact private copy of cached HEAD `306b61a788bc860617583ffe573e03072f6f8629`. `tycho-deferred-attack-provenance.patch` includes required source headers; `tycho-deferred-attack-current-tree.patch` omits the two includes already present in the working tree. The latter passed `git apply --check` before root integration. Root owns the production application, enabled/disabled builds, final acceptance and commits. The companion reviewer wrote only these documentation and permanent regression/tool files in the shared repository; native proof sources, objects and binaries remained private.

The existing dispatcher can defer an ambient procedure while the VM has flags masked by `0x0124`. Review identified `0x20` and `0x100` as temporary parent suspension, so these do not expire provenance before an executable reaction. `0x04` is an interpreter failure; that program cannot execute another native TALK either. Its pending origin remains until program purge or session reset. No installed-script reproduction of a nested TALK starting a new origin inside the same CRITTER callback was found; unconditional callback-exit expiry remains a limit of this narrow policy.

Root integrated the reviewed candidate as `6b9c01b`. Combined build 93 passes CTest 3/3 and the permanent runner's 13 ASAN controls. Native movement, disk loading and authenticated reconnect pass with both processes exiting zero. The isolated committed-production build also passes CTest 3/3. Evidence: `/var/tmp/fallout-native-deferred-attack93-20261002.log`, `/var/tmp/fallout-native-dialogue-recovery93-20261002/summary.tsv`, and `/var/tmp/fallout-verified-dialogue-build93-20261002.log`. The intentionally stopped hostile fixtures retain their narrower acceptance scope described above.
