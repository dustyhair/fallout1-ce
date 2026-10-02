# Installed weapon acceptance

The build-67 dedicated weapon matrix passed all 10 families on the installed
Fallout data. Each family used one real native host attack and one real guest
command executed by the host. Both clients produced the same final snapshot
digest. The guest execution probes reported zero script procedures, combat
attacks and RNG draws during inventory actions and combat.

Evidence is in `/var/tmp/fallout-native-weapons67-all-20261001.log` and
`/var/tmp/fallout-native-weapons-7b6wg303`. All 20 copied host/guest executables
have SHA-256 `4042d68ff19c74abe27a90ab2b8c4612646d7435fcd725c1933d769220eee9d9`.
The driver exited successfully. Earlier build-66 focused and remaining-family
runs also passed, but their NPC padding lacked a native maximum-HP bonus. The
build-67 results below include that correction and measure actual damage.

## Checks and setup

`tools/test_native_multiplayer_weapons.py` pins one copied binary for its entire
matrix and uses separate muted Xvfb sessions, copied patch data and the real
`master.dat` and `critter.dat`. It does not control visible HAL/testbox games.
Each family has a 90-second process limit and detailed setup and command logs.
The ordinary 50-case runner is unchanged.

The fixture gives both humans 180 allocated points in the weapon skills,
a strength bonus and a 1,000-point maximum-HP bonus. It pads the installed NPC's
native maximum HP too, places the humans on opposite sides of it, and supplies
one native ammo clip or two extra grenades. These are explicit acceptance-test
conditions. The test does not claim natural character progression or quest
completion. It stops native combat after the guest's attack checkpoint.

The actual installed item prototypes and English item messages were inspected
read-only from the archive. Runtime setup checks the native weapon and ammo
prototypes, including matching caliber. Accepted attacks also pass the native
executor's installed attack-art check. Capacities, attack AP and burst rounds
come from native prototype/item helpers, rather than pistol constants.

For each attack, the host checks exact native AP and resource consumption, and
checks that the other player's ammo and AP stay unchanged. Resource accounting
includes loaded weapons and carried ammo of the matching caliber. Grenades use
native inventory quantity. Both players also unload and reload through normal
semantic inventory commands in Exploration and Combat. Each non-thrown family
passes eight conservation checks, with no extra AP charge beyond the native
four-AP inventory opening charge in Combat. Grenades have no magazine.

## Build-67 results

AP and resource use below are per attack. Damage columns sum the two attacks.
Friendly damage measures damage to the other human from native extras; it does
not include damage to the shooter from their own explosion. Snapshot equality
also covers both humans' resulting HP, equipment, AP and carried resources.

| Family | Native PID | Mode | Capacity / ammo PID | Attack AP | Resources spent | NPC damage | Friendly damage | Matching digest |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10mm SMG | 9 | Secondary burst | 30 / 29 | 6 | 10 rounds | 175 | 0 | `4252842938942811492` |
| Assault rifle | 23 | Secondary burst | 24 / 35 | 6 | 8 rounds | 180 | 0 | `4053962207561525560` |
| Minigun | 12 | Primary burst | 120 / 35 | 6 | 40 rounds | 710 | 28 | `3825505575413888543` |
| Frag grenade | 25 | Primary throw | No magazine | 4 | 1 grenade | 44 | 57 | `12695883556677498537` |
| Rocket launcher | 13 | Primary explosion | 1 / 14 | 6 | 1 rocket | 114 | 149 | `6211241748984967803` |
| Laser pistol | 16 | Primary single | 12 / 38 | 5 | 1 cell | 27 | 0 | `10200500569228693013` |
| Plasma pistol | 24 | Primary single | 16 / 38 | 5 | 1 cell | 54 | 0 | `7121889647373593178` |
| Flamer | 11 | Primary continuous | 5 / 32 | 6 | 1 fuel unit | 104 | 128 | `338199177645383096` |
| Laser rifle | 118 | Primary single | 12 / 39 | 5 | 1 cell | 57 | 0 | `11957695869141495994` |
| Plasma rifle | 15 | Primary single | 10 / 39 | 5 | 1 cell | 99 | 0 | `16117569815439485326` |

Minigun extras damaged the guest by 11 HP on the host's attack and the host by
17 HP on the guest's attack. Flamer extras dealt 83 HP and 45 HP respectively.
Grenade extras dealt 26 HP and 31 HP; rocket extras dealt 97 HP and 52 HP. Both
humans survived. Their final native HP and resources matched the guest replica
for every family.

The review covered the generic native attack executor's owner/turn checks,
attack-art validation, native animation drain, and inventory reload/unload
ownership and magazine handling. No production weapon bug was reproduced by
this matrix. The added code is acceptance-fixture instrumentation and setup;
it does not change weapon rules. This covers the listed representative modes,
not every weapon PID, every critical result, last-grenade depletion, or a full
combat campaign. Existing knife and aimed-pistol checks remain separate.

Reproduce with:

```sh
TMPDIR=/var/tmp python3 tools/test_native_multiplayer_weapons.py \
  build/fallout-ce /var/tmp/fallout-npc-barter-scripted-final-20261001/host
```
