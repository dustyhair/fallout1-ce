# Multiplayer release review, October 1, 2026

This review covers the dirty `multiplayer-plan` implementation and the TCP,
snapshot, object-lifetime and recovery paths. It does not establish completion
of the story campaign. The active protocol formats remain gameplay 47,
snapshot 26, sidecar 8 and recovery 4. Live networking remains two-player.

## Confirmed fixes

### Native destruction retained freed inventory pointers

`obj_erase_object` notified multiplayer only for the directly erased object.
Native `obj_inven_free` then called `obj_remove` for its children without that
notification. Destroying a registered bag therefore left its freed contents in
`worldItems` and the entity registry. A later allocation at the same address
appeared to be an already registered item. Checkpoint capture could dereference
freed storage or retain the wrong identity.

The notification now runs inside `obj_remove`, after the native
`OBJECT_NO_REMOVE` rejection and before recursive inventory destruction. This
covers every descendant and preserves registry membership when native removal
is refused. Snapshot reconciliation can still discard an obsolete container
without leaving its removed children registered.

The native authority fixture reproduced the bug on build 57. The first door bag
used entities 294 and 295; after its destruction, the scenery child's address
still resolved to entity 295 and was not initially untracked. Build 59 assigns
the next bag and child entities 296 and 297, captures both ownership graphs with
quantity three, and verifies both IDs disappear after each bag is destroyed.
The fixture also verifies that refused removal of a door or scenery owner
retains that owner and its descendants.

### Script-created door and scenery inventory missed checkpoints

Host capture registered new inventory on players, NPCs and known item owners,
but omitted doors and other scenery. Native scripts can place inventory on
these objects. The snapshot ownership validator already accepts those holders.
Initial world registration now includes those inventories on both peers, and
host capture discovers new items and nested contents before serializing items.
The native fixture checks a nested bag and quantity-three stack on each holder
without opening an inventory window first. It also rebuilds the tracked world
and verifies that both nested entries remain indexed.

### TCP queues grew without a bound

The per-packet limit did not bound queued sends or unread received frames.
A stalled peer could grow these queues throughout a session.

Outbound framed data now has a 32 MiB bound. Crossing it disconnects the stream
explicitly; it does not report the frame as sent or silently drop a checkpoint.
The lobby retains the applied cursor and authoritative event journal for
reconnect. This is overload handling, not an automatic reconnection feature.
Received payload data has a 32 MiB bound and 8192-packet bound. Reading pauses
at either bound and resumes when the application drains packets. The incomplete
frame buffer remains bounded by a maximum frame plus one TLS read buffer.

The core gate preserves the existing 16 MiB blocked-write and bilateral TLS
retry checks. New checks cross the received packet-count and byte limits,
drain every frame in order, saturate an idle peer's outbound queue, and reconnect
to the same pinned listener. Commit `86717fc` contains the verified TCP batch.

### Map replacement needed to restore a destroyed peer body entry

Bulk map cleanup now removes the freed peer body from the registry. The map
replacement bridge previously required that old entry to survive and could not
rebind a replacement body after cleanup. It now restores the known stable
player/actor identity and owner when that entry is absent, before normal
rebinding. This preserves the character build and drops the old pointer.
Map transition and disk recovery failed on build 59 before this bridge fix;
movement, elevator, map transition and disk recovery all pass on build 62.
A core check covers restoration after the old body entry has been unregistered.
Commit `3313ab4` contains the verified object/map batch.

Native snapshot application also compares actor count with the registered
roster instead of the literal two. Every entry still must match
its registered player, current actor and ownership. This removes one engine
preflight obstacle; it does not add four-player transport or body respawn.

### An unread command could strand every later guest command

A successful nonblocking TLS send can leave a command queued locally. The guest
advanced its command sequence, but disconnect discarded pending command
tracking. If authority had not received that command, later guest commands
skipped the host's expected sequence and stayed rejected as stale.

The guest now retains at most 64 unconfirmed commands and resends their original
player, actor, target, phase and revision on a fresh stream. Authority either
executes the missing command or returns its recorded outcome. Commands rejected
because the world changed still consume their original sequence. New input at
the outbox bound fails without consuming a sequence or disconnecting. A stream
failure during a write retains the intent because some bytes may already have
reached the host. A new session clears the outbox.

The core fixture queues 20 MiB through real TLS while authority does not read,
then queues a resource pickup and closes that stream. A pinned reconnect resends
sequence one, which executes once. A second pickup executes before its result
and event are discarded by closing the guest first. Reconnect returns the
recorded result and recovers its event without another pickup. Sequence three
then succeeds. Across both boundaries, three units move from stock to inventory
with exactly three executor calls. This is a command-boundary resource model,
not a claim that native attacks or barter have been disrupted in a real quest.

An isolated build that restores the previous pending-clear/no-resend behavior
fails the same TLS fixture because the first original intent is missing. The
fixed build passes CTest. Commit `eff1b2b` contains the outbox and its
self-contained TLS fixture. No wire-format change is needed for this outbox.

### Native descriptors passed wire validation with unsafe categories and tiles

Actor and NPC records accepted item or scenery FIDs. Native `obj_change_fid`
assigns these values directly, while the snapshot adapter then writes critter
union fields and later code uses the FID category to interpret that storage.
The validator also accepted tiles at or above 40000, although the native grid
has exactly 200 by 200 tiles. Native movement rejected these coordinates after
item reconciliation could already have changed the world.

Commit `2caedf1` checks native critter FID categories and NPC PID categories,
and validates actors, NPCs, scenery and ground items against the native grid.
It keeps weapon, animation and facing bits intact. Valid corpse art with facing
bits, rotation five and tile 39999 still round trips. A private binary linked
against the previous core library fails ten new assertions. The fixed core
passes CTest, including checksum-valid malformed packets that now fail before
native application. The wire layout and version remain unchanged.

### Rejected native checkpoints mutated objects before identity and phase checks

The native adapter checked authoritative phase revisions after reconstructing
items and deleting obsolete objects. Actor ownership checks also followed map
loading and scenery reconstruction. A syntactically valid rejected checkpoint
could therefore change the native world before returning failure.

Actor ownership and the existing authoritative phase rules now run at entry,
before native mutation. Forward revisions remain legal. The native replica
fixture submits a same-revision phase mismatch and an older phase revision with
an empty item section, which would otherwise delete all items before rejection.
It also submits a wrong actor owner with a different map ID. All three cases
reject and preserve the entire snapshot digest and registry size, with no
script procedures, combat attacks or random draws. Build 66 logs
`NATIVE_SNAPSHOT_REJECTION_PREFLIGHT_PASS`; movement, elevator, map transition
and disk-load/reconnect recovery then pass. Both recovery peers report digest
`6814706862886534857` on map 26.

## Capacity evidence and unresolved release work

The aggregate snapshot payload cap remains 524288 bytes. The dense core fixture
encodes 8192 scenery objects into a 396040-byte packet with two sample actors
and small variable arrays. It then admits 2290 inventory objects, round trips
the near-limit checkpoint, and verifies that the full recovery envelope fits
the protocol and TCP frame limits. One additional item fails with
`PayloadTooLarge` before any partial packet is emitted. An oversized declared
length fails before checksum processing or payload allocation.

The individual section ceilings do not all fit together. Maximum critters,
doors, scenery, items, three variable arrays and timed events alone total
917504 bytes, before actors and the other state. This synthetic calculation
is not a measurement of a late campaign map. The native build 62 recovery
captures equal 31432-byte packets on both peers for Shady Sands map 26, with
2 actors, 32 critters, 5 doors, 406 scenery objects, 57 items, 618 global values,
5 map globals, 79 local values and 20 timers. Build 72 also passes real native capture, guest application, disk load and
authenticated reconnect on Gun Runners, Hub downtown, Necropolis, Mariposa and
the Cathedral. The largest recovered packet is 65903 bytes with 1118 scenery
objects. Long-session captures with accumulated inventories, scripted objects
and companions still need broader campaign evidence.

The handshake exchanges the content digest and reconnect credentials but does
not negotiate snapshot capacity. Raising the cap on one build would let a
same-format older peer connect and reject the larger checkpoint later. Keep the
cap until a versioned compatibility decision and real-map evidence justify a
change. Increasing an aggregate cap alone cannot prove content acceptance.

The event journal remains bounded at 256 events or 256 KiB and requests a
snapshot when earlier history has expired. Applied acknowledgements validate
against the latest sequence and prior acknowledgement, not the retained front
of the journal. Command result history retains 64 outcomes per player; older
replays fail the monotonic sequence check rather than executing again. Core
checks cover retained-history recovery, checkpoint cursor ordering and stale
commands. A long running real session with disrupted attacks, barter commits,
quest rewards, map travel and saves is still required.

Snapshot decoding rejects invalid wire state before native application. Native
application can load a map,
resize local-variable storage, reconstruct objects and rebind identities before
all later engine operations succeed. Recovery application failure aborts the
lobby; the general authoritative-state path logs a failure and awaits another
checkpoint. This is not proof of atomic rollback after allocation, missing-art
or native placement failure. Failure injection and real large-map recovery
remain release checks.

Pair-shaped lobby credentials, command transport, reconnect registry, runtime
local-role selection and several native placement/save bridges remain on the
four-player follow-up. The new capture loops use the registered roster, and
snapshot ownership IDs remain bounded to 1 through 16 with unique membership.

## Script-created world objects

Native `create_object` places an object without registering its multiplayer
identity. Previously, captures iterated only the existing registry and its
inventories. New ground items, NPCs and ordinary scenery could therefore remain
absent from a successful checkpoint. A non-script timer owned by a new NPC made
capture fail instead. The installed CAVEWALL damage handler creates scenery
PIDs 33554839 and 33554499, so the missing scenery affects an actual map script.

Commit `cf5f99e` adds host discovery of previously unregistered placed items,
NPCs and ordinary scenery without resetting the combat, dialogue or player roster. It
checks section limits before registration and then registers nested inventory
through the existing path. The installed prototypes for all seven identified
script-created scenery PIDs are generic scenery. Dynamic doors and miscellaneous
exit grids still lack reconstructable descriptors and remain outside this fix.

Copied build 70 passes the dedicated TLS fixture. The guest receives a bag with
three antidotes, a new NPC, new scenery and the NPC's timer. It erases all four
identities locally, disconnects, then reconstructs those identities with the
same resource quantity and exact timer during authenticated recovery. Both
peers finish with digest 1559574927803082093 and exit successfully. The guest
executes no scripts, attacks or random rolls. A private build retaining the same
fixture but omitting only the discovery call fails cleanly at the unregistered
NPC timer owner. This proves supported object discovery and recovery, not a
complete CAVEWALL quest playthrough.

The first build 69 fixture run exposed a fixture cleanup error. Native
`item_add_force` leaves the child's floating object node connected; the fixture
now calls `obj_disconnect` after insertion, as the native item pickup path does.
Build 70 passes both the clean shutdown and the existing focused map/recovery
gates.

Actual Gun Runners map 46 failed capture after load despite both player actors
being idle. The private diagnostic build identifies `DuplicateEntityId`, with
2 actors, 11 critters, 2 doors, 1118 scenery and 101 items. The inherited native
iterator revisits the first occupied tile, and initial multiplayer registration
appends the same object and identity twice. Incremental discovery can also
collect the repeated object before registration. Multiplayer pointer
deduplication in both scans is committed in `719e0cf`. The native iterator and
single-player behavior are unchanged. The focused discovery fixture places its
objects at tile zero to exercise this case. All five dense-map fixtures pass on
build 72, including Gun Runners with 100 items instead of the duplicate 101st
entry. No snapshot capacity increase has been made.

## Permanent capacity failure and native simulation stop

Capture failures now report their actual branch, section, entity, map, phase
and object counts. Busy animation remains a transient retry. A confirmed
aggregate or section capacity failure enters a persistent failed lobby state,
closes the stream, stops accepting commands and shows the instruction to load
the last successful save. It does not repeatedly attempt an impossible
checkpoint or negotiate a larger format.

The native capacity fixture passes on copied build 75. It first proves a busy
actor rejects capture without stopping the session, then proves capture works
after the actor becomes idle. It injects a global-variable count above the
supported limit without reading beyond native storage. The host stops command
processing, scripts, combat AI, attacks, queued timers, existing animation and
shared rest. The clock, health, poison, world digest and command boundary remain
unchanged. Native save entry points reject before preparation or backup, and
all four files of the earlier save retain their SHA256 hashes. After stopping
the failed session and loading that save, a due poison timer runs again.

The guest-ready barrier waits for an actual native capture before the host
triggers failure. The guest then reports a normal disconnect, with no protocol
failure, and both processes exit successfully. The older build 74 fixture closed the host during guest startup and reported
a protocol error without rejected wire packets. A separate focused test then
confirmed the checkpoint acknowledgement bug described below. The older
fixture alone does not prove which abort branch ran.

This proves the tested exploration stop boundary. It does not prove rollback
of a native operation that already mutated objects before capacity exhaustion,
or atomic recovery after native allocation and placement failures. The
production stop patch is committed in `abe01f0` after an isolated full build,
CTest and clean native disk/reconnect recovery. Copied build 78 repeats the
capacity, freeze and save-file preservation checks successfully.

## Checkpoint acknowledgement disconnect and native cleanup

A checkpoint can finish native application just as the host closes its stream.
`confirmSnapshotApplied` retains that applied event cursor, but sending the ACK
can enter `Disconnected`. The runtime then calls `abortRecovery`, which used to
overwrite the transport failure with fatal `ProtocolError`. That prevented a
fresh connection despite successful reconstruction.

The focused core test closes the host after the guest receives event-three's
checkpoint and before it sends the applied ACK. Before the fix, six assertions
fail across abort classification, reconnect and later event delivery. The fix
preserves the existing transport failure. The guest resumes from event three,
completes recovery and receives event four once. A connected native application
failure still aborts with `ProtocolError`.

Copied build 75 also reproduced a clean-shutdown spin after both recovery peers
printed equal digests. Child GDB identifies `scr_remove_all` repeatedly trying
to remove SID `0x04004650`. The native list contains a protected first slot and
an unprotected second slot with that same SID and owner. SID lookup returns the
protected slot every time, so bulk cleanup never advances.

Bulk removal now passes the exact slot to the shared removal helper. A native
included-translation-unit test reproduces the old five-second hang with forty
scripts across three extents. The fixed AddressSanitizer probe removes every
unprotected slot, retains both protected scripts, tolerates another cleanup
pass and completes forced cleanup. Normal single-SID removal still honors the
protected flag.

The parent's native party recovery fix keeps a roster actor's newly allocated
unique SID during map load. A read-only GDB audit on copied build 78 records no
SID duplicates after each of three party recoveries, including actual disk load.
The player owns SID `0x0400000F` after disk recovery, and all thirty-seven script
slots remain unique before cleanup. Both peers exit cleanly, with the same
recovery digest as before. This audit proves the exercised native player path;
it does not establish every companion or modified prototype path.

The stopped-combat audit also found that `combat_input` calls
`scripts_check_state_in_combat` after leaving its input loop. That dispatcher
previously guarded only a guest replica and could still process a pending
elevator request during stopped unwind. The one-line guard now includes the
runtime stop. A native included-translation-unit probe selects the stopped
state and counts a substituted elevator selector, so it does not open a modal
window. The old dispatcher reaches the selector; the fixed dispatcher clears
the request without calling it. Ordinary unstopped dispatch still reaches the
selector. This follow-up passes the scoped ASAN probe and the subsequent combined
builds 80 and 81.

## Native prototype rejection and metadata publication retries

A checksum-valid checkpoint can describe an unavailable native NPC prototype.
On a private copy of build 79, application rejected that checkpoint after
clearing the item registry. Capture still succeeded afterward, but all thirteen
tracked items had disappeared and the registry shrank from 252 to 238 entries.
The prototype preflight in `54b8a26` checks native prototype availability and
object category before replacing maps, variables or identities. Actual native
build 81 rejects unavailable NPC, scenery and item prototypes while preserving
the full digest, registry and zero guest script, attack and RNG execution.
Movement and disk/reconnect recovery finish with both process exits zero.
This closes unavailable-prototype rejection, while native allocation and
placement rollback remain open.

The metadata publication probe also reproduced data loss across retries. The
first interrupted publication retained the original `MULTI.BAK` when both
publication and restoration renames failed. The next attempt removed that
backup before trying another rename, losing the original metadata. The
fix in `4c9d651` uses `MULTI.OLD`, which native `*.BAK` processing does
not erase, and recovers outstanding metadata before starting another publish
or native backup. It also recovers legacy `MULTI.BAK` and rejects nonregular
paths without discarding a retained backup.

The permanent native fault probe forces a failed publication and failed
restoration, then a failed retry, then successful recovery. It checks exact
bytes after each attempt. Legacy-backup recovery before native backup and a
blocked directory destination also pass. All nine assertions pass against
build 81's source. These tests prove the injected rename failures and retry
behavior. They do not establish atomic recovery from every kernel or disk
failure.

A further private build-81 proof found that an incompatible fixed global count
can reject after map-local storage has already expanded from zero entries to
one. The ordering fix in `69a7529` checks game globals before replacing a map and
checks target-map globals and static counts before resizing locals. Build 82
passes checksum-valid rejection cases for game globals, map globals and doors,
including a proposed map change, with unchanged digest and registry. Legitimate
local expansion and restoration also pass. Movement and disk/reconnect
recovery finish with both peers exiting successfully. Cross-map load rollback
remains open.

A separate private build-81 test confirms missing actor art and an oversized
frame can reject after an earlier actor's health changes from 30 to 31. The
missing-art case also leaves the rejected actor with the invalid FID. Actor art/frame preflight now runs before native mutation and checks the
actual FRM frame count. An unchanged roster actor's exact FID/frame pair is
permitted without requiring its art to load. All actor and NPC catalog indices
are checked before native death-art alias indexing. NPC, scenery and item
frame availability still require safe reconciliation-specific checks.

Build 83 rejects missing actor art, oversized actor frames and an out-of-range
NPC electrify alias while preserving the full native digest and registry.
Native fall-back and burned-to-nothing alias poses apply with their actual
final frames and restore the original digest. Guest script, attack and RNG
counts stay zero. Movement and disk/reconnect recovery also pass. All five dense installed maps also pass against the same pinned build 83,
including actual guest application, disk load and authenticated reconnect
with exact section, timer and recovered digest equality.

The next private build-83 test confirms an oversized NPC frame can reject
after an earlier actor's health changes from 30 to 31. Its checksum-valid
packet leaves the registry size unchanged while changing the full digest.
NPC frame preflight now uses the same side-effect-free exact PID/location and
sole-PID matching helper as actual reconciliation. Stable equal-count native
identities retain the existing registry-ID path. Art availability is checked
when the selected native frame differs, including a new object's initial
frame zero. This check runs after any target-map load and before local resize;
it does not provide rollback for a map replacement that already completed.
Build 85 passes stable, reconciled and newly created NPC oversized-frame
rejection with unchanged digest and registry. Unchanged hidden presentation
with unavailable artwork and a reconstructed hidden NPC at frame zero both
apply and restore the original digest. Movement and disk/reconnect recovery
exit cleanly. The first build-84 fixture wrongly assumed different NPC PIDs;
the corrected reconciliation test supports a same-PID encounter population.
All five dense-map compatibility checks pass against pinned build 85,
including actual guest application, native disk load and authenticated
reconnect with exact recovered section, timer and digest equality.

A private build-85 batch also confirms late frame failures for doors, scenery
and items. Each checksum-valid oversized frame rejects after an earlier
actor's health changes from 30 to 31. Native capture succeeds afterward, with
an unchanged registry size but a changed full digest. The preflight now uses the actual door FID and shared scenery/item
reconciliation matchers. It checks required frames before resizing locals or
changing objects. Parent-first item ordering also resolves native container
bodies before their children. The isolated patch includes the existing main
code's ordinary-scenery reconstruction and parent-first holder transfer as
self-contained dependencies, without changing the schema.

Build 86 passes six checksum-valid rejection cases, preserving the full digest
and registry for static frames, a reconstructed scenery object, a rebased
item and a new item. The native door's final valid FRM frame applies and
restores, as do unchanged hidden scenery/items with missing art and newly
reconstructed hidden scenery/items at frame zero. Those accepted paths match
actual setter behavior. Movement, disk/reconnect, all five dense maps and the
TLS script-created-object reconstruction test pass. Allocation, native
placement and cross-map rollback remain open.

The later isolated shutdown failure was a duplicate ground-item list node,
corrected by `286bc5a`. Builds 90 and 91 then pass native recovery and clean
shutdown on both peers. `f12a224` stages missing bodies and required scripts
before reconciliation; `0c85393` also reserves native inventory arrays before
changing existing holders. Permanent allocator controls reject fourth-body,
new-container storage and existing-character storage failures with unchanged
digest, registry, body/script counts and event queue. Corrected build 94 exits
0/0; old storage code leaves four bodies, one script and rebased IDs behind,
with explicit failure exits 1/1. Existing holder storage remains unchanged on
rejection. Later timer allocation and cross-map replacement remain open.

`9664697` closes process-interrupted native/metadata publication recovery,
including interruption before rollback starts. Checked native file cleanup
retains the recovery record on blocked directories. It does not establish
power-loss durability or platform fsync ordering. `6b9c01b` fixes the reproduced
immediate Tycho hostility attribution, with thirteen native VM ASAN controls.
`7047a65` prevents unvoted idle dialogue from selecting a reply. Matched visible
build 93 holds an actual Ian conversation unchanged for 90.697 seconds, then
accepts both players' later ballots. These scopes are recorded in the completion
plan and do not close the ordinary full-story or long-session release gates.

## Validation artifacts

- `/var/tmp/fallout-object-frame-recovery86-20261002/guest/movement.log`:
  six object-frame rejection cases preserve state, and all five native
  presentation controls apply and restore; disk recovery also passes.
- `/var/tmp/fallout-native-dense-maps86-20261002.log`: all five actual dense
  maps pass with door, scenery and item frame preflight.
- `/var/tmp/fallout-native-world-discovery86-20261002.log`: real TLS new
  objects and timers apply, disappear and reconstruct with exact digest.

- `/var/tmp/fallout-native-dense-maps85-20261002.log`: all five actual dense
  maps pass with NPC frame preflight and shared reconciliation matching.
- `/var/tmp/fallout-snapshot-object-frames-negative85-native-20261002.log`:
  door, scenery and item frame failures each change earlier actor health.

- `/var/tmp/fallout-npc-frame-recovery85-20261002/guest/movement.log`:
  stable/reconciled/new NPC frames reject unchanged; hidden/native-frame-zero
  controls apply and restore. Disk recovery also passes.

- `/var/tmp/fallout-snapshot-npc-frame-negative83-native-20261002.log`:
  oversized NPC frame rejects after changing an earlier actor's health.

- `/var/tmp/fallout-native-dense-maps83-20261002.log`: all five actual dense
  maps pass guest application, native disk load and authenticated reconnect.
- `/var/tmp/fallout-art-recovery83-20261002/guest/movement.log`: unavailable
  actor art, oversized frames and unsafe NPC alias index reject unchanged;
  native death/alias controls apply and restore. Disk recovery also passes.

- `/var/tmp/fallout-variable-recovery82-20261002.log`: native layout rejection,
  legitimate local expansion/restoration, movement and disk/reconnect pass.
- `/var/tmp/fallout-snapshot-art-negative81-native-20261002.log`: unavailable
  actor art and oversized frame reject after changing health or FID.

- `/var/tmp/fallout-snapshot-variable-atomicity-negative81-native-20261002.log`:
  checksum-valid incompatible globals reject after native local count changes.
- `/var/tmp/fallout-native-sidecar-publish-positive-20261002.log`:
  all nine interrupted publication, retry and legacy recovery assertions pass.
- `/var/tmp/fallout-sidecar-publish-negative79-20261002/result.log`:
  a second failed publication removes the only retained original metadata.
- `/var/tmp/fallout-snapshot-atomicity-negative79-native-default-20261002.log`:
  unavailable NPC rejection loses all thirteen tracked items before preflight.

- `/var/tmp/fallout-stopped-script-entry-before-20261002.log` and its `after`
  counterpart: native stopped combat dispatcher rejects the pending elevator
  request while the ordinary dispatcher still processes it.

- `/var/tmp/fallout-release-review-focused78-20261002/summary.tsv`: core,
  movement, elevator, map transition and recovery pass with both exits zero.
- `/var/tmp/fallout-release-review-recovery-gdb78-sid-audit-20261002/guest/recovery.log`:
  real native party and disk recovery retain unique script identities and exit.
- `/var/tmp/fallout-native-script-cleanup-before-20261002.log` and its `after`
  counterpart: old duplicate-SID hang, fixed native ASAN cleanup proof.
- `/var/tmp/fallout-ack-disconnect-negative-test-20261002.log` and its `positive`
  counterpart: six failed reconnect assertions before the guard, complete core
  pass after preserving the disconnect.

- `/var/tmp/fallout-native-snapshot-capacity75-20261002.log`: native capacity,
  transient busy, stopped simulation, save preservation and load recovery pass.
- `/var/tmp/fallout-native-snapshot-capacity-onflfj5y/fixture/`: host and guest
  logs plus matching baseline/final hashes for all four prior save files.
- `/var/tmp/fallout-native-dense-maps72-20261002.log`: all five actual dense
  native map capture, guest apply, disk load and reconnect fixtures pass.

- `/var/tmp/fallout-native-world-discovery70-20261001.log`: real TLS native
  object application and reconstruction pass with clean process exits.
- `/var/tmp/fallout-script-created-world-negative-20261001/driver70.log`:
  discovery omitted privately; capture fails on the new NPC timer owner.
- `/var/tmp/fallout-release-review-focused70-20261001/summary.tsv`: core,
  movement, elevator, map transition and recovery pass after discovery.
- `/var/tmp/fallout-release-review-focused66-20261001/summary.tsv`: focused
  core, movement, elevator, map transition and recovery pass after early
  native snapshot rejection checks.
- `/var/tmp/fallout-release-review-focused66-20261001/guest/movement.log`:
  phase mismatch, stale phase and wrong actor owner all reject while preserving
  the full world digest and registry; the native authority fixture passes.
- `/var/tmp/fallout-release-review-ctest65-20261001.log`: CTest passes 3 of 3
  after descriptor checks, including checksum-valid malformed packets.
- `/var/tmp/fallout-snapshot-descriptor-reproduction-20261001/before.log`:
  the pre-fix library fails all ten new descriptor rejection assertions.
- `/var/tmp/fallout-five-goals-build62-20261001.log`: combined enabled build
  passes after the map replacement and initial inventory fixes.
- `/var/tmp/fallout-release-review-ctest62-20261001.log`: CTest passes 3 of 3
  before the outbox change.
- `/var/tmp/fallout-release-review-ctest63-20261001.log`: CTest passes 3 of 3,
  including TLS in-flight command reconnect, queue saturation/retry and dense
  snapshot boundary checks.
- `/var/tmp/fallout-release-review-focused57-20261001/host/movement.log`:
  reproduction of the dangling child identity before the destruction fix.
- `/var/tmp/fallout-release-review-focused62-20261001/host/movement.log`:
  native door/scenery reward, initial indexing, refused-removal and nested
  destruction checks pass.
- `/var/tmp/fallout-release-review-focused62-20261001/summary.tsv`:
  movement, elevator, map transition and recovery all pass on both peers.
- `/var/tmp/fallout-release-review-focused62-20261001/host/recovery-smoke-snapshot.bin`
  and its guest counterpart: equal 31432-byte real recovery packets.
- `/var/tmp/fallout-inflight-reconnect-reproduction-20261001/core.log`:
  isolated negative outbox proof fails on the missing original intent.

These focused fixtures do not replace the full campaign matrix.

Full quest-route completion, alternate major story branches, a final expanded
campaign run, multiplayer-disabled native behavior and platform acceptance
remain separate release gates. None is checked off by these focused fixtures.
