# Installed dense map recovery checks

The dedicated driver `tools/test_native_multiplayer_dense_maps.py` checks five
installed maps through actual native map loading, snapshot capture and guest
application, native disk save/load, and authenticated reconnect. Each case uses
a fresh isolated profile and muted Xvfb sessions. The driver pins one executable
for the whole matrix. It does not control the visible game.

Arrival is an explicit fixture choice after both humans approve world-map
departure. It does not represent normal route traversal or quest completion.
Before the standard recovery fixture adds its status and timer checks, the host
saves the native arrival to SLOT02 in the isolated profile. Recovery then tests
the normal recovery save and load path.

## Read-only map ranking

The installed DAT reader parsed all 72 native MAP files through their end,
including script tables, elevation object lists and recursive inventories.
Counts below are archived records before map scripts run. Item counts include
owned inventory records rather than stacked unit quantities. Immutable walls
and miscellaneous objects do not contribute to this snapshot-category ranking.

| Map | Native ID | Top-level objects | NPCs | Doors | Other scenery | Items |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Gun Runners, LAGUNRUN | 46 | 2189 | 11 | 2 | 1118 | 95 |
| Hub downtown, HUBDWNTN | 38 | 3550 | 50 | 28 | 844 | 217 |
| Necropolis, HALLDED | 3 | 4311 | 67 | 11 | 924 | 94 |
| Mariposa, MBSTRG12 | 31 | 4546 | 32 | 9 | 840 | 151 |
| Cathedral, CHILDRN1 | 17 | 2511 | 28 | 19 | 705 | 89 |

Gun Runners and Hub downtown have the largest combined captured-category
counts. The matrix also includes dense Necropolis and Mariposa maps and the
Cathedral map. Raw ranking artifacts are
`/var/tmp/fallout-installed-map-density-20261001.json` and
`/var/tmp/fallout-installed-map-density-20261001.log`.

## Failures found before the final matrix

Build 69 captured the Hub, Necropolis, Mariposa and Cathedral native arrivals.
Mariposa also passed the full native recovery sequence with matching digest
`4580624430428634343`. Hub, Necropolis and Cathedral exposed a fixture timing
error: the host could start recovery after an event acknowledgment before the
guest had applied the later native checkpoint. The corrected fixture waits for
an explicit driver witness written only after the guest captures and validates
the target native map. The wait is bounded and failure identifies the missing
native-ready witness. An event acknowledgment alone is not checkpoint proof.

Gun Runners failed native snapshot validation. The diagnostic executable
reported `DuplicateEntityId`, error 23, with map 46, phase 3, two actors,
11 critters, two doors, 1118 scenery records and 101 items. Both human bodies
were idle. The native object iterator revisits objects on the first occupied
tile. Multiplayer registration and incremental discovery now deduplicate
object pointers during that scan. Snapshot capacities were not increased.

Diagnostic log:
`/var/tmp/fallout-native-dense-gunrunners-diagnostic-20261001.log`.

## Build-72 matrix

All five maps passed on October 2. The driver exited 0 with
`NATIVE_MULTIPLAYER_DENSE_MAPS_PASS maps=5`.

- Driver log: `/var/tmp/fallout-native-dense-maps72-20261002.log`.
- Artifact root: `/var/tmp/fallout-native-dense-maps-cdnkzw3o`.
- All ten host and guest executable copies have SHA-256
  `fcb89879c5c040622b29fe0c3f6b0ea49e02291615dd414a5f64dab3af3710eb`.

| Map | Encoded recovery bytes | NPCs | Doors | Other scenery | Items | Timers | Matching recovery digest |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Gun Runners | 65903 | 11 | 2 | 1118 | 101 | 9 | 11295325609784311006 |
| Hub downtown | 62863 | 50 | 28 | 844 | 219 | 12 | 958127434027615350 |
| Necropolis | 61487 | 67 | 11 | 924 | 95 | 19 | 13128901650081762221 |
| Mariposa | 58259 | 32 | 9 | 840 | 152 | 9 | 14803599880699830348 |
| Cathedral | 46999 | 28 | 19 | 705 | 90 | 9 | 7934737033192495968 |

Every case passed native map capture on both clients, the driver's explicit
guest-native-ready witness, native baseline disk save, recovery disk load and
authenticated reconnect. Both clients retained the fixture's incapacitation
and its exact wake timer. The final native checkpoint uses the acknowledged
Ending phase to freeze comparison. Event, phase, revision and time matched,
as did the session, actor, NPC, door, scenery, item, global variable, map
variable, timer and world-map section digests. Encoded byte counts and all
reported native section counts also matched.

The largest recovered snapshot is 65,903 bytes, below the existing 512 KiB
payload limit. These five representative installed maps do not establish
support for arbitrary modified maps or unbounded script-created populations.
Explicit handling of snapshot exhaustion remains a separate test.
