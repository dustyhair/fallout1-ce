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
