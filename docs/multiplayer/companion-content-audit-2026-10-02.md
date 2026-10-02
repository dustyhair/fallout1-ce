# Installed Dogmeat, Tycho and Katja audit

This is a read-only audit and acceptance proposal. It does not claim these
three companions have passed native multiplayer lifecycle tests. The evidence
comes from the installed `master.dat` used by the isolated native fixtures.
The local decompiled SSL files helped locate branches; installed INT bytecode,
MSG files and MAP records verify the conditions described below. Empty or
incomplete local SSL files were not treated as authoritative.

## Installed identities

| Companion | Installed recruitment script | Script index | Native map | PID | Tile and elevation |
| --- | --- | ---: | --- | --- | --- |
| Dogmeat | JUNKDOG.INT | 352 | JUNKCSNO, map 11 | 0x0100007A | 30687, floor 0 |
| Tycho | TYCHO.INT | 388 | JUNKCSNO, map 11 | 0x010000D2 | 19891, floor 0 |
| Katja | KATJA.INT | 622 | LAFOLLWR, map 29 | 0x0100012E | 19887, floor 0 |

The archived script SHA-256 values are:

- JUNKDOG: `67e69643fb02e1c22f3c8bc75311faebfafeed8830eacb0a0d4f1b67f7c69857`.
- TYCHO: `3e5e073791874e955d09cba62a5bb89ffac08c4d9339fef55b884d271cd581f0`.
- KATJA: `36ade7d41b73a79bbd05b780a2a8fd47d32fd78470e02c45195d25fb50a3bfb2`.

Extracted bytecode, messages and instruction listings are in
`/var/tmp/companion-installed-{JUNKDOG,TYCHO,KATJA}.*` and the corresponding
`-disassembly.txt` files. `DOGMEAT.INT`, `DOG2.INT` and `DEMODOG` describe
different dog behavior and must not substitute for Junktown's JUNKDOG.

## Dogmeat

JUNKDOG has two native recruitment paths while shared global 5 is false:

1. Its ambient critter procedure sees `dude_obj` wearing native Leather Jacket
   PID 74 in armor slot 0. It requires `obj_can_see_obj`, not an invented fixed
   recruitment distance. Bytecode offsets 1004 through 1100 check visibility,
   equipped PID and recruitment state before calling `dog_joins_dude`.
2. A native use-item-on action supplies Iguana-on-a-stick PID 103 or 81.
   Offsets 1860 through 1948 check either PID and the same recruitment state.
   Both installed prototypes are drug items named Iguana-on-a-stick.

The join procedure at offset 1976 grants 100 experience, sets global 5 to 1,
sets globals 186 and 187 to 2, adds the real dog to the native party, sets
team 0 and clears map variable 5. Native XP is a shared party award in
multiplayer. JUNKDOG does not override default food handling, so the native
drug-use path also determines consumption and drug timers. A test should
observe that path instead of assuming the food remains in inventory.

Talking to Dogmeat produces `Woof!`, with no recruitment ballot or dismissal
option. His native death procedure clears recruitment, updates globals 186
and 187, removes party membership and clears map variable 5. There is no
JUNKDOG damage or combat procedure establishing a scripted betrayal policy.

Minimal native acceptance: select actual Junktown arrival as labeled fixture
setup; give only the guest one native food item; execute the ordinary guest
use-on action; require the installed dog's join, team and party state, exactly
one 100-XP award to each human, native food consumption and valid timer state.
Repeat use-on after recruitment must not award another recruitment reward.
Then travel, save/load and reconnect with the same dog and resources.
A separate armor case should equip only the guest through a native command,
keep the host outside the dog's visibility and approach through native guest
movement. This tests which human the ambient visibility check actually sees.

## Tycho

Tycho's native status is shared global 121: 0 before introductions, 1 after
introductions, 2 recruited and 3 departed or dead. Initial conversation needs
INT 4 or more for the normal dialogue branches. INT 6 exposes the survival
training path. A first friendly conversation transfers a native Nuka-Cola
from his inventory to `dude_obj`. It does not require a quest variable.

The training branch `Tycho17`, offset 6628, modifies the talker's Outdoorsman
skill 17 by five points and advances native time by 120 seconds. The native
skill opcode uses the acting player's character state. This reward is personal;
it should increase the actual guest's skill while preserving the host's skill.
It is separate from shared party XP.

Recruitment is offered in `Tycho15` only when global 36 is true, with adequate
reaction. The displayed option is `Killian's asked me to clean up this town,
and I'd like your help.` A hostile Gizmo-related branch takes priority when
global 39 is 1 and global 36 is 0. Installed KILLIAN bytecode writes global 36
in `Killianx4`, immediately after giving the player an item, at offsets 16982
through 16994. A recruitment test must reach that node through Killian's native
plot dialogue and actions. Directly setting global 36 would not establish
quest acceptance.

`TychoJoins`, offset 8750, sets status 2, team 0, native party membership and
a one-second follow timer. After dialogue, global 314 gates one shared 300-XP
recruitment reward. Native dismissal removes the timer and party membership,
sets status 3 and offers a later rejoin. Formation choices change global 278.
Tycho accepts non-weapon gifts except healing items 40, 47 and 91, plus an
explicit weapon whitelist. A guest gift must come from the guest inventory.

Tycho's named introduction uses the same `proto_data(obj_pid(dude_obj), 1)`
PC-name path already fixed and tested with Ian. It is a direct additional
native name-regression case. His hostile dialogue sets a VM `hostile` flag;
the later critter procedure attacks `dude_obj`. Guest insults followed by
delayed native retaliation need a dedicated test because the actor scope of
the conversation has ended by that later tick. This is a risk identified by
the audit, not a reproduced multiplayer bug.

Minimal first test: actual guest Tycho conversation, native drink transfer,
guest-only Outdoorsman training, guest name and exact time/reward checks.
Full recruit/dismiss/rejoin acceptance then extends the native Killian plot
slice rather than supplying its prerequisite variable.

## Katja

Katja's native status is shared global 244: 0 unknown, 1 introduced, 2 recruited
and 3 departed or dead. Her normal introduction requires INT 4 and offers
`The name's <actual talker>.` through the same PC-name path. INT 3 or lower
gets a native refusal. Water-chip PID 55 and global 101 gate a water-information
topic. They do not gate her exploration-topic recruitment path.

A short native recruitment route uses INT 5 or higher. It selects the named
introduction, `First you tell me who you are.`, `I'm looking for information.
Can you help?`, the area-information topic, `I don't plan to stay in this place
too much longer, myself.`, and `You're a welcome addition, but the desert's
not much more fun.` These are real message IDs 103/104, 109, 116, 130, 145 and 149.
The recruitment node `Katja20`, offset 6382, sets status 2, adds native party
membership, schedules a follow timer and sets team 0. Global 315 gates the
one-time shared 200-XP award. No quest-variable forcing is needed.

Native dismissal sets status 3, removes membership and removes her timers.
The later option `I changed my mind. I'd still like your help.` rejoins her
without resetting the one-time reward. Formation choices change global 279.
Her use-item-on procedure transfers accepted guest gifts from `dude_obj` to
her inventory, with its own weapon whitelist. A real knife or SMG gift can
test actor-specific ownership and resource conservation. Her lock-help options
depend on actual map pointer variables and elevation and should be a later
content slice, not synthetic map-variable setup.

Minimal native acceptance: guest voted introduction and recruitment, exact
guest name, shared 200-XP award once, accepted guest gift, native dismissal and
rejoin, actual travel, disk load and authenticated reconnect.

## Multiplayer paths to exercise

`op_dude_obj` returns the acting actor; PC `proto_name` now does the same.
Native inventory operations use their explicit actor pointers. The XP opcode
awards both registered humans on the host and ignores replica execution.
`op_critter_mod_skill` updates the scoped acting character. These paths should
be checked through the actual installed dialogue and actions above.

Native party addition excludes human roster actors. Companion ambient critter
procedures and party placement explicitly bind the shared host leader. Native
`script_q_process` currently executes timed scripts without that explicit
leader binding. Tycho and Katja follow timers therefore need a guest-action
test that pumps a due native timer while the guest is on a different floor or
far from the host. The expected companion leader remains the shared host.
No timer-context bug is claimed without a native reproduction.

Prioritize Katja's complete native lifecycle and Dogmeat's guest food path.
They require no forced quest state. Tycho's free drink, training and name can
be checked immediately; his full recruitment belongs with the real Killian
plot sequence. Fixture arrival, starter equipment and character attributes
must be labeled as setup. Quest variables, party additions and dialogue
results must come from native gameplay.
