# Visible multiplayer campaign, October 1

This records ordinary installed-content play on the isolated HAL and
codex-testbox sessions. It does not claim a completed campaign. The host uses
DISPLAY :0 and the guest uses DISPLAY :7. Both retain SDL dummy audio and
disabled TTS. No quest variables or fixture routes were used to advance this
playthrough.

## Cave progress on build 54

The pair resumed from SLOT10, "Five goals before cave quest", and entered
CAVES.MAP through native Shady Sands dialogue. The initial handoff counted two
kills from an earlier trip. A later read-only parse of this checkpoint's
actual CAVES.SAV establishes that those two kills are absent from this run.
The current map began with all nine scorpions alive.

Natalia killed scorpion 1238 near tile 28937 with her equipped 10mm pistol.
Both peers agreed on the 5 AP cost, ammunition use, damage and death. Max's
native Stimpak use restored 17 HP and cost 2 AP. The fight awarded 110 XP.

Natalia's ordinary loot approach toward that corpse triggered another
scorpion's combat before the inventory window opened. Both peers agreed on
the resulting position and turn ownership. The second fight killed scorpion
1237 near tile 28535 and awarded another 110 XP. Natalia healed and reloaded
through the normal authoritative actions. One later healing request used an
obsolete stack representative and was rejected without consuming the item.
The fresh inventory ID worked afterward.

Natalia opened both actual corpse windows and used native Take All. Party loot
allocation gave tail 1262 to Max and tail 1263 to Natalia. She also approached
and picked up the loose 10mm JHP near tile 30144. Host First Aid on Natalia
approached her, produced the native "looks healthy already" message, and
advanced the poison timers. Both peers then agreed on 42/30 HP and poison 5/2.

The pair followed the other cave branch through ordinary clicks and movement.
Home worked again at legal central tiles 22542, 21510 and 16900. Max equipped
his existing brass knuckles, approached scorpion 1230 and used native 3 AP
melee attacks. Natalia lit exactly one flare during combat and received the
use message. The remaining flare stayed unlit. A later hit killed this
scorpion near 14098 and awarded another 110 XP. Natalia's native corpse
window and Take All gave its tail to Max, who now holds two tails. Natalia
holds one. They healed to 42/31 HP before saving.

Three of the cave's nine scorpions have died in this saved campaign.
The extermination quest and its turn-in remain pending. The native host save
in SLOT09 is "Caves five scorpions killed", an inaccurate description based
on the initial handoff count. Its final pre-update checkpoint
has Max at 13897 and Natalia at 13896. SLOT10 remains intact. The previous
SLOT09 is preserved at
`/var/tmp/fallout-parity-visible-20260930/SLOT09-before-visible-campaign-20261001`
on HAL. Multiplayer disk saves are host-owned; the guest restores the shared
checkpoint when joining.

## Exploration camera failure

Ordinary movement carried Max to 31528 and Natalia to 31130. Both clients
agreed on these positions, but their local actors moved beyond the viewport.
Home failed to recenter in Exploration on both clients. An ordinary HAL
`xdotool key Home` also failed. Native Pip-Boy still opened and closed, and
mouse edge scrolling still worked. This was a visible input/camera failure,
not just stale journal coordinates. After edge scrolling and a fresh facing
checkpoint, Max's journal screen position was -17/293.

HAL screenshots preserve the unchanged chamber and the edge scroll:
`/var/tmp/visible-campaign-caves91-east.png`,
`/var/tmp/visible-campaign-caves91-xhome.png`, and
`/var/tmp/visible-campaign-caves91-scroll.png`.

Build 60 added camera-bound clamping, but deployment was held for a separate
recovery regression. Build 62 retains that camera fix and repairs the map
transition/recovery failure. The isolated native build succeeded, the pair
restarted as host92/guest92, and native SLOT09 load plus authenticated guest
join restored 42/31 HP, poison 11/6, all three kills, three tails, equipment and
the lit/unlit flare pair.

After ordinary movement settled at the prior failing tiles 31528/31130, Home
kept Max visible at screen 368/198 on HAL and Natalia at 339/172 on the guest.
Both actual screenshots show the actors near the center. Home also passed at
legal tiles 22542/22743, with local positions 321/162 and 320/162. This closes
the observed Exploration camera failure. The screenshots are copied locally
to `/var/tmp/fallout-visible-campaign-20261001/host92-edge.png` and
`/var/tmp/fallout-visible-campaign-20261001/guest92-edge.png`.

The build 54 journals are copied locally to
`/var/tmp/fallout-visible-campaign-20261001/host91-journal.jsonl` and
`/var/tmp/fallout-visible-campaign-20261001/guest91-journal.jsonl`.

A read-only parse of `/var/tmp/fallout-visible-caves92-readonly.sav` found
three dead scorpions at 14098, 28535 and 28937, and six living scorpions with
26 HP at 13255, 15252, 16668, 21468, 22058 and 22067. This corrects the earlier
count. It does not establish a save bug, since the earlier two kills came
from a different checkpoint. No save bytes or quest variables were changed.

## Continued combat on build 62

Ordinary movement into the remaining branch started real combat before the
requested destination. The host saw scorpion 285 near 15491, and the guest
also saw living scorpion 281 farther east. Max moved into melee range and
dealt 11 damage with brass knuckles. Natalia reloaded, moved and attacked
within her native 10 AP budget. Her later shots dealt 8 and a critical 16
damage, killing 285. End Combat returned both peers to Exploration. Max
opened that corpse's native loot window; Take All allocated its tail to
Natalia. Each player then held two tails.

The next approach started combat with scorpion 281 near 16083. Its large body
blocked a requested adjacent destination; another reachable tile put Max in
melee range. Pistol and brass-knuckle hits killed it. Max took ordinary melee
hits down to 15 HP, used one Stimpak for 11 HP, and left combat at 26 HP.
Native corpse looting gave the fifth tail to Max.

Max's First Aid on himself failed with "You fail to do any healing." The
native action advanced time and poison decay. After both checkpoints settled,
the actors had 25/29 HP and poison 5/0. No healing or successful skill result
was forced. This records a failed ordinary skill roll and its authoritative
time/status effects, as well as both players' successful combat and looting.

Nearby scorpion 286 started another fight before the attempted five-kill save
opened. The native message explained that combat could not end with hostile
creatures nearby; that attempted save did not replace the four-kill checkpoint.
Natalia reloaded her pistol, and Max used his last Stimpak before approaching
with brass knuckles. Their attacks killed 286 and awarded another 110 XP.
End Combat and the sixth corpse's native Take All left the players at 31/29 HP,
with three tails each and no Stimpaks. Max remained poisoned; Natalia's poison
had cleared.

The actual campaign now has six dead scorpions and three living ones. Native
SLOT09 save completed with "Game Saved." and description "Caves six confirmed
kills". SLOT10 remains intact. The pair is returning to Shady Sands for
antidotes and ordinary Ian recruitment before finishing the remaining fights.

## Shady Sands return

Both players approached the cave exit, approved the native world-map proposal,
and returned to Shady Sands. Poison ticked on Max during travel. Natalia opened
Razlo's curtain and initiated his actual dialogue. Both voted for the poison
sample option and completed the conversation. The native 250 XP message
appeared; Natalia's tails changed from three to two and she received Antidote
825. She used it on Max through normal targeted item use and approached him.
The item disappeared, and both checkpoints agreed on poison 0/0 and 29/29 HP.

Both opened native Pip-Boy alarm clocks and chose "Rest until healed". Rest
completed with 49/36 HP on both clients, the actors' higher maxima after their
ordinary level gains. The lit flare had expired during the elapsed game time;
Natalia's separate unlit flare remained. No resources or healing were injected.

Jarvis still showed native poisoning symptoms in this checkpoint. Natalia
converted a second tail to Antidote 827 through Razlo's ordinary dialogue and
used it on Jarvis 744. Both clients displayed "Over the next few minutes, the
man shows definite improvement." and the native 400 XP cure reward. The
antidote disappeared. Max still holds three tails and Natalia one, leaving
four collected tails after the two ordinary antidote exchanges.

Natalia approached Ian 730 at tile 15683 and initiated native dialogue.
Both peers showed talker player 2, but the introduction option said "I'm Max
Stone." and a later reply addressed Max Stone. The guest dialogue used the
host's name; its cause is under investigation. Her world actor remained Natalia
throughout, and the actual guest character sheet reads "NATALIA". No loose Ian
script or message override exists in HAL's configured DATA patch directory.
The installed archive has the same hash as the inspected native archive. Its
English `IAN.MSG` inserts the name between messages 104/105 and 131/132.
`IAN.INT` retrieves it through `dude_obj`, `obj_pid`, then `proto_data` at
offsets 5142/5144/5152 and 6132/6134/6142. It does not use `obj_name` for these
lines. This identifies the prototype-name path for implementation review.
The pair asked for directions to Junktown and the Hub, then requested Ian's
help. The native offer cost 100 caps. Natalia's alternative offer to share the
loot failed, producing "Come back when you have some caps." Ian did not join,
and neither player had caps deducted. This preserves another ordinary failed
skill/dialogue outcome; recruitment remains pending.

The safe town checkpoint completed in native SLOT09 as "Shady cured safe six
kills" with the actual "Game Saved." event at host sequence 3231. Both actors
have full 49/36 HP, zero poison, and their ordinary inventories. Ian remains
unrecruited. The cave's six corpses persist in this town save. A full copy is
preserved on HAL at
`/var/tmp/fallout-parity-visible-20260930/SLOT09-shady-safe-six-20261002`.
The host92 and guest92 journals are copied to
`/var/tmp/fallout-visible-campaign-20261001/`.

Natalia's actual character sheet shows level 2, 1,885 XP, Speech 29%, Small
Guns 35%, and 14 unspent points in that save. A native canceled skill draft
restored the points. A later editor input used the wrong plus-button row and
committed 13 points to Small Guns, reaching 48% with one point left. That edit
was not saved. Native Load correctly refused while the multiplayer session
was active, so the safe pre-edit checkpoint will restore those points during
the coordinated restart. This does not establish an allocation defect.

Host journal anchors are sequence 1117 for failed First Aid, 1376 for the
six-kill save, 1928 for Razlo's 250 XP reward, 2321 for the full 49/36 HP rest
checkpoint, 2623 for Jarvis's 400 XP reward, and 3231 for the town save.
The guest sheet before editing and the actual connected-load refusal screen
are preserved with the journals as `guest92-sheet-before-edit.png` and
`host92-connected-load-refusal.png`.

The host's native sheet separately confirms Max Stone at level 2, 1,885 XP,
49/49 HP, Speech 33%, and 13 unspent points. It is preserved as
`host92-sheet-after-rest.png`. The earlier connected-load refusal screenshot
still showed 29 in the host HUD, despite the settled 49 HP journal and later
native sheet. That display discrepancy needs review; actual health was correct.

## Build 73 restore and name check

Candidate 73 was copied from a pinned ten-file source delta and rebuilt
natively on codex-testbox. The source manifest matches on both machines.
HAL's pinned binary hash is
`383268a4b9d4238e588488cf695c3273b95cefc0f3eef1da08886f2c91f055e3`;
the native guest binary hash is
`9494661beb942dde15f31cf74611d7444b3c351ad41991678b19fae4f03e35fb`.
The ordinary pair restarted as host93/guest93, keeping save/config backups,
TTS disabled, and SDL dummy audio. Native SLOT09 load and authenticated guest
join restored 49/36 HP, zero poison, tails 3/1, ammunition, equipment and quest
progress. Natalia's sheet again has Small Guns 35% and 14 unspent points;
the unsaved editor input was discarded.

Natalia initiated Ian's actual dialogue after restoring. Both peers now show
talker player 2 with "So, Natalia, what can I do for you?" at dialogue revision
1. This closes the observed prototype-name failure on the ordinary save.
The HP-changing shared-rest HUD check remains pending the next actual fight.

Both actors committed their level-2 points through native character editors.
Natalia spent all 14 on Speech, increasing 29% to 43%; Small Guns stayed 35%.
Max spent all 13 on tagged Small Guns, increasing 62% to 88%. Reopening both
sheets confirmed the changes and zero unspent points. Their distinct point
budgets and tagged-skill cost behaved normally. The two committed sheets are
preserved as `guest93-speech-committed.png` and `host93-guns-committed.png`.
Ian again refused Natalia's loot-sharing offer. Read-only inspection of the
installed `Ian17a` procedure confirms a Speech roll with a +20 modifier, so
that refusal is another ordinary failed roll, not a fixed skill threshold.

The next native offer succeeded at dialogue revision 7, with "All right,
I'll help you out in exchange for my share of loot." Both voted through
the following "Let's go" reply. Ian became friendly with team 0 and 50 HP.
No caps were spent, and no party membership or skill roll was forced.

Ian followed Max from his house at 15683 to 17881, then 17681. Both peers
show the companion-only menu, including dismissal, formation and weapon
commands, with the correct Natalia name. Both voted to draw his best weapon;
he answered "Sure thing." Native host SLOT09 save succeeded at host93
sequence 1749 with "Ian joined six cave kills". The previous cured-town save
is preserved externally as `SLOT09-before-Ian-save73-20261002`; SLOT10 remains
intact. Recruitment and both committed skill allocations are now saved.

Seth's ordinary cave dialogue moved both humans and Ian into CAVES. Ian
remained friendly, team 0 and 50 HP, and followed inside the cave. Natalia
reloaded her empty 10mm pistol through the native HUD weapon button outside
combat. It gained 12 rounds, and the 4-box JHP stack became three boxes with
20 rounds in its partial box. The replacement stack has a fresh entity ID.

## Build 73 cave combat with Ian

Max advanced into the unexplored room and entered combat at 19267, while
Natalia was still catching up. Scorpion 1232 at 20067 took Max's 10-point
pistol hit. During the following AI turn it took 9 and 8, dying at 19667
before Natalia's next turn. These hits occurred at host93 sequences 2274
and 2285 while owner 0 was active, with Natalia at 0 AP. Her next turn began
at sequence 2286 with 10 AP. The attempted guest attacks on the dead target
spent no ammunition. Ian therefore finished this seventh actual kill too. Max suffered 8 damage and poison 3, leaving
41/36 human HP. Natalia's approach spent AP through ordinary movement.

Scorpion 1234 took Max's 7-point hit. During an AI turn it then took an
8-point brain hit at host93 sequence 2316, before Natalia's two misses.
Max's next shot did 5, leaving 6 HP. Ian suffered ordinary 1 damage and
critical 3 damage at sequences 2334 and 2339. During that AI sequence the
scorpion took 9 and died at sequence 2350, before Natalia's next turn.
Ian is the only allied NPC in this fight, so the AI-turn damage and final
kill are consistent with his native attacks. Both peers agree on Ian's
46 HP and the corpse at 19266. Eight of nine cave scorpions are now dead.

The last living scorpion, entity 1233 at 22258, engaged when Max advanced
through the cleared room. Two native 2-point attacks increased his poison
to 10 and reduced him to 37 HP. Max's final loaded pistol round dealt 10;
Natalia's two shots dealt 4 and 3. During the next AI sequence Ian missed,
then the scorpion took 10 and died at host93 sequence 2466. The native
500 XP extermination reward appeared immediately at sequence 2467.
All nine actual cave scorpions are dead. End Combat awarded the final
110 combat XP normally. Native corpse loot left tails 5/2 between the
humans, totaling seven after the earlier two antidote exchanges.

Both players chose native Pip-Boy "Rest until healed". Real poison ticks
appeared during the shared time advance. Max healed from 37 to 49, poison
10 cleared to zero, and Ian healed from 46 to 50. Natalia stayed at 36.
Both journals agree at host93 sequence 2566 and guest93 sequence 1888.
The actual HAL HUD now shows `049`, and the guest HUD shows `036`. The
screens `host93-hud-after-real-rest.png` and
`guest93-hud-after-real-rest.png` preserve the result. This closes the
observed build-62 stale host HUD after shared rest.

Native SLOT09 saved the completed cave at host93 sequence 2642 with
"Nine scorpions cleared healed". It contains both full-health humans,
zero poison, recruited Ian, committed skills, ammunition and all quest
progress. The save is also copied externally as
`SLOT09-nine-cleared-healed73-20261002`. Earlier cured-town and Ian saves
remain backed up, and SLOT10 is untouched.

Back in Shady Sands, Natalia talked to Seth. Both peers received "Our
entire town is grateful to you for destroying the radscorpions. Thank
you." His remaining menu no longer offers the caves or radscorpion topic.
The shared quest activity now records subject 724 with value 2, following
the original value 1. This confirms ordinary quest completion through
native combat and town dialogue.

Razlo exchanged Natalia's two remaining tails for two real Antidotes
through native dialogue. No repeated XP appeared. Natalia then gave one
to Max through the ordinary player gift action. The first attempt from
three hexes away was rejected; moving adjacent allowed the transfer.
Both journals show one Antidote per actor, with a new ID for the remaining
guest stack. Five tails remain in Max's inventory.

The safe town checkpoint saved at host93 sequence 4068 with "Shady nine
cleared antidotes" and was backed up as
`SLOT09-Shady-cleared-antidotes73-20261002`. The group then walked outside
the gate to the visible native exit grid at 8506. Max proposed travel,
Natalia accepted, and the actual world map opened with Junktown and the
Hub discovered through Ian's directions.

## Ordinary Junktown travel

The native Junktown route produced the random lone-traveler encounter in
MOUNTN1.MAP. Both humans arrived at full health with their ammunition and
one Antidote each; Ian arrived with 50 HP. Max reloaded his empty pistol
through the native HUD, drawing 12 rounds from his ammo stack. Natalia
talked with Patrick about the water chip and his music. Both peers showed
the same native branches. Patrick could not help with Vault technology.
No combat or reward was forced during this encounter.

The nearest encounter border tiles at 18665 and 18865 contain native
blocking scenery. Moving north along the border to 18265 reached an
unblocked exit. Max proposed travel, Natalia accepted, and the group
returned to the world map normally. This was ordinary blocked terrain
and routing, with no bypass of movement or travel checks.

The resumed Junktown route generated an ordinary large radscorpion pack
in DESERT2.MAP. Five hostile 26-HP scorpions appeared around both players
and Ian, alongside a native dead merchant. They advanced and attacked
during AI turns. Max took 14 damage, then 16 more, reaching 19 HP and
poison 9. Natalia took 3 damage, reaching 33; Ian stayed at 50. The group
fought with actual loaded pistols, misses, critical hits and native AI.

On Natalia's turn she fired her final pistol round for 3 damage, reloaded
to 12 rounds for 2 AP, then used her real Antidote on Max for 2 AP. Her
10 AP became 1. The drug disappeared and both peers showed Max poison
9->0, with host93 sequence 5599 recording the poison reduction and 5600
"You use the Antidote on Max Stone." This verifies native combat use-on
another player, ammunition consumption, reload and AP costs in an
unmodified campaign encounter.

Max's final native shot did 9 and killed the fifth encounter scorpion at
sequence 5603. End Combat awarded 550 XP and a level-up at sequences
5607-5608. Natalia's First Aid on Max then failed normally, with no
healing, at host93 sequence 5801 and guest93 sequence 4138.

The actual merchant corpse held 81 caps and one Stimpak. Native Take All
split the caps 41/40 and placed the Stimpak in Natalia's inventory. She
used the found drug on Max, healing 13 HP from 25 to 38, at host93
sequences 5878-5879 and guest93 sequences 4182-4183. Shared rest subsequently
healed both actors to their new level-3 maximums, 55/41, with zero poison.

Both reopened native character sheets show level 3 and XP 3365. Max chose
Quick Pockets and spent 13 tagged Small Guns points, raising 88% to 114%.
Natalia chose Awareness and spent 14 Small Guns points, raising 35% to 49%.
Both show zero unspent points and their own distinct perk. Committed
sheets are preserved as `host93-level3-committed.png` and
`guest93-level3-committed.png`.

One presentation concern remains to review. Natalia's already-open alarm
page kept its old 36/41 text after shared rest while the journals reported
41 HP. Closing the page showed HUD `041`; a fresh character sheet also
showed 41/41. This concerns refreshing an open Pip-Boy page, separate from
the fixed host HUD after rest. Clicking the middle of the native rest row
at y=338 was reliable; clicks at the text's lower edge were inconsistent.

Native SLOT09 saved the healed road encounter and committed level-3
characters at host93 sequence 6365, with "Road pack healed level3". An
external copy is preserved as `SLOT09-road-pack-level3-73-20261002`, in
addition to all previous town/cave checkpoints and SLOT10.

The cleared DESERT2 encounter exited through the native grid at 21656.
Movement onto the grid already proposed travel (host93 sequence 6763);
an extra explicit exit request was rejected while that proposal was
pending. Natalia accepted, and the native Junktown button resumed travel.
Both peers entered JUNKENT.MAP with Max 55 HP, Natalia 41 and Ian 50.

Kalnor immediately warned Max to put his weapon away. Both peers showed
the same three dialogue branches about holstering weapons and respecting
the town's law. The group chose the peaceful replies, returned to
exploration, and both players unequipped their knives and pistols. The
native town save completed at host93 sequence 6941 as "Junktown safe
level3". Its external backup is
`SLOT09-Junktown-safe-level3-73-20261002`. This provides a safe deployment
checkpoint before advancing Junktown's quests.

Natalia then asked Kalnor for entry during the night. Her polite reply
was refused by the native dialogue: no strangers admitted at night.
The group saved again at host93 sequence 7587 as "Junktown gates waiting",
with external backup `SLOT09-Junktown-gates-waiting-73-20261002`.

## Matched build 81 town continuation

The pinned build-81 bundle supplied by the parent agent was deployed as
host94/guest94 after the town save. All 306 source/configuration hashes
matched on the testbox before its native rebuild. HAL uses pinned ELF
SHA-256 `b6b3c9ba2a7e56861375997b10f8f567911ebcdef4e743b6ac57dec8a0d4ae7d`;
the testbox native ELF is
`9156fa2d870168fdacd60a7d357e722bb92a89c52d30a2f94aa037873bcd626f`.
The restart preserved save/configuration backups, `enabled=0` for TTS
and the dummy SDL audio driver. Native SLOT09 restored both humans,
Ian, holstered gear, caps 41/40, pistol loads 8/12 and remaining drugs.

The group used the native alarm's shared until-morning option to wait
outside the closed gate. After matching requests, the proposal cleared
and Natalia's next conversation received Kalnor's daytime welcome.
Both peers showed his directions to Killian's Darkwater supplies shop
to the north. This verifies ordinary time-gated town entry; it does not
yet establish the build-81 open-alarm HP redraw fix, since both actors
were already at full health.

Inside town, Natalia talked to Lars about Junktown and why the guards
had not arrested Gizmo and the Skulz. He said they needed direct
evidence. When the party offered to help, he directed them to Killian
for Gizmo and asked them to bring information about the Skulz back to
him. Both peers showed the same replies and recorded the same votes;
the completed conversation returned to exploration without hostility.

The group crossed the native north exit at 10073 with both humans
nearby. The local transition loaded JUNKKILL.MAP directly, placing them
at 28917/28918; it did not require the world-map travel vote. Home then
showed Ian beside Max with 50 HP on both peers. Native SLOT09 saved
outside Killian's shop at host94 sequence 2605 as "Killian shop before
talk", with backup `SLOT09-Killian-before-talk81-20261002`.

Killian initially returned the native floating reply "It's late. Come
back some other time." The group requested a shared wait until noon.
Review of the full journals corrected an early interpretation that
the matching request had failed: the first proposal at host94 sequence
3082 cleared at 3203 about 57 seconds later, during native rest
processing. A premature retry created a second noon proposal at 3365,
which cleared at 3542 about 90 seconds later. This unintentionally
waited an additional day. Fresh alarm pages on both peers show
29 December 2161, 12:02, with full 55/41 HP and no pending rest.
Subsequent rest checks must wait for completion rather than retry
after five seconds.

The separate missing-proposer-label concern has a concrete presentation
cause identified by the read-only review: DrawAlarmText prints the
proposal at line 2, then DrawAlrmHitPnts clears and overwrites that same
line. No shared-rest approval failure is established by this run.
Exact inputs and pending transitions are archived in
`rest94-exact-chronology.txt` alongside both full journals/logs and the
actual screen captures in the evidence directory.

Killian's open shop conversation used Natalia as the actual talker.
His introduction correctly offered "I'm Natalia." He described
Junktown's troubles, the Hub and Necropolis through the native branches.
Several idle branches advanced after 60 seconds while evidence was
being reviewed; their choices are recorded without the explicit
`[Max Stone, Natalia]` vote suffix. Explicit selections afterward used
the current revision. No hostile insult or quest variable was forced.

Ending the conversation naturally spawned Kenji (entity 2172) on both
peers. He announced "Gizmo sends his regards!" and attacked Killian.
Max's first combat turn expired before his attempted shot, which was
rejected without firing. Natalia's actual pistol shots then critically
hit Kenji for 13, missed, hit for 12 and hit for 11, killing him. Killian
took a native 40-point critical hit, falling from 65 to 25 HP, and used
his own Stimpak to reach 41. The humans remained at 55/41 and Ian at 50.

The installed quest awarded 400 XP for helping Killian at host94
sequence 4209. Native End Combat awarded 250 more at 4213. Killian's
automatic gratitude dialogue asked for evidence against Gizmo. Both
humans accepted, and native Bug 2178 and Tape Recorder 2179 appeared
in Max's inventory. Both journals record quest subject 736, value 1,
attributed to Natalia.

Natalia opened Kenji's actual corpse inventory, shown in
`guest94-kenji-loot.png`. Native Take All assigned the corpse's Hunting
Rifle 2173, loaded with eight rounds, to Max. He equipped it in his
right hand while retaining his eight-round pistol in inventory. Native
SLOT09 saved these gains at host94 sequence 5018 as "Killian saved rifle
bug", with external backup
`SLOT09-Killian-saved-rifle-bug81-20261002` before the next matched
deployment. The earlier cave, town and SLOT10 backups remain preserved.


Build 85 was deployed as matched host95/guest95 after the external
Killian checkpoint backup. HAL used pinned SHA256
`2f2e09ea422e5075c0c78fe7a3903dcba35c86eab960adb5e66c4531d9c80025`;
the testbox native binary is
`05f1c992d51db9c393d3e74756320f2d253b49b8d1bf09ec5674fa5f00b9c8ef`.
All 306 source/config hashes matched the pinned bundle. Both sessions
remain muted. Restored inventory includes Max's Bug, Tape Recorder and
loaded Hunting Rifle. Natalia's actual native sheet shows her name,
INT 7, Outdoorsman 1%, XP 4015 and no unspent skill points.

Both proposal labels now remain visible below the HP strip.
`host95-proposer-label.png` shows the local cancellation hint, and
`guest95-proposer-label.png` shows "Max Stone proposes rest - select red
time" with the ten-minute row selected. A fresh bounded attempt recorded
matching host sequence 1007 at 1790925699015 and guest sequence 603 at
1790925699595, both proposing ten minutes as Max Stone. The request
still matched at host 1029 and guest 623. Natalia approved the same
row and the pending request cleared on both peers at host 1184 and
guest 717. Both humans remained at full 55/41 HP. This verifies label
placement and shared approval, not the pending HP-changing open-page
redraw acceptance.

Earlier apparent guest-label failures in this check were operator
errors: scrot preserved an existing filename and wrote a suffixed
image, and extra Escape input opened the host pause menu after the
rest row had already closed Pip-Boy. Current guest captures use
scrot's overwrite option. No proposal synchronization defect is
established by those attempts.


The ordinary north exit entered JUNKCSNO with Ian following Max.
Tycho was absent at midday, as his installed bar schedule requires
16:00-22:00. Both players approved native rest until evening. Tycho
then appeared at tile 19690 as entity 2481, within three hexes of
Natalia at 20093. Max remained at 24100 with Ian 2500 near 24500.

Natalia followed Tycho's actual friendly drink, story and survival
options. The drink placed Nuka-Cola 2562 in her inventory. Native
training reply 7 states that his instruction course helped, followed
by the correct introduction option "I'm Natalia." The course improved
only Natalia's Outdoorsman skill by five. Her fresh evening sheet
showed INT 9 and Outdoorsman 2% before the lesson, then INT 9 and
Outdoorsman 7% afterward. Her Night Person trait had changed the
daytime baseline of INT 7 and Outdoorsman 1%, so the daytime sheet
alone is not the training baseline. Max's separate native sheets
show 11% before and after. No skill points were spent during this
conversation. XP remains 4015.

Fresh guest alarm captures show 28 December 2161 at 18:02 before
training and 18:04 afterward. This matches the installed course's
two-minute advance. Throughout that actual timer advance Ian remained
at 24500, two hexes from Max and about 25 from Natalia. He did not
follow the guest talker. The before/after sheets, clock captures and
`guest95-tycho-trained-dialogue.png` preserve this evidence. All
choices were polite native options with both players voting.


Tycho's next native conversation offered help cleaning up the town
because Killian's quest had already started. Natalia selected that
option with both votes. Tycho agreed to join and suggested dealing
with Gizmo. Actual follow then moved Max from 24100 to 25100, with
Tycho 2481 at 25299 and Ian 2500 at 25099, both team 0. Natalia
remained at 20093. Tycho has 60 HP and Ian 50. Native SLOT09 saved
this progress at host95 sequence 4704 as "Tycho trained and joined".
The external backup is `SLOT09-Tycho-trained-party85-20261002`; the
previous Killian tools/rifle backup and untouched SLOT10 remain.


Build 90 restored this saved party on host96/guest96. All 306 pinned
source/config hashes matched before the native testbox rebuild. HAL
uses SHA256 `2b8d514c9f74ac1cc9ce59e8a287252f680cb67433c364f6a0ef01b24928d8e1`;
testbox uses its native binary
`6f4128cd183b7c5a067adfc6a0de34ce8b7c392d36e1914e9861a05bc49eb500`.
Both connected to JUNKCSNO with Max 55 HP, Natalia 41, Ian 50 and
Tycho 60, no poison, and both companions on team 0 near Max. The
restored native sheet retains Natalia's INT 9 and Outdoorsman 7%.
It now shows XP 4315, confirming Tycho's recruitment added 300 XP
after the lesson's 4015 baseline. The Nuka-Cola remains hers. Max
retains both evidence tools and the eight-round Hunting Rifle. Both
TTS configurations remain disabled and both launches use dummy SDL
audio. Prior saves and backups were preserved by the restart.


Natalia carried Max's Tape Recorder after an ordinary adjacent
`game_give` transfer, then followed Gizmo's native investigation
conversation. Both voted through the offer to replace his failed
assassin and asked why he wanted Killian dead. The actual confession
is captured in `guest96-gizmo-confession.png`. Host96 sequence 4503
at 1790929343894 says Killian cramps Gizmo's business. Revision 8
first appears at sequence 4513, 1790929344099, with both ballots 0.

While I reviewed the capture, the multiplayer 60-second voting
deadline expired. Sequence 4514 at 1790929403913 selected option 3,
which reveals the recorder, without a voter suffix or intervening
vote. This was 60.019 seconds after the confession reply. The native
hostile reply then threatened Natalia with Izo. My subsequent
revision-9 option-2 vote was rejected because that page has only
Done; both then selected Done. No hostile dialogue option was
intentionally voted. The pinned controller's no-ballot
MajorityStatsRandomTie expiry admits every option to the random
selection, and gdialog executes that shared decision. This is
multiplayer timeout behavior, not evidence of a single-player idle
choice. The input and journal chronology is preserved in
`host96-current-journal.jsonl`.

The resulting ordinary combat killed Izo and Gizmo through Natalia's
pistol and Ian/Tycho's native attacks. Max's Hunting Rifle attempts
from the south casino hall were rejected with eight rounds unchanged;
the office walls obstruct that position. Both humans and companions
remain at full HP. The two nearby guards are still hostile, so the
native End Combat action refuses to finish. No restart, quest forcing
or healing-refresh acceptance is claimed. SLOT09 still preserves the
peaceful Tycho checkpoint while this encounter continues.


Both guards subsequently died in native combat. Ian fell to 23/50
HP, used his own Stimpak, and recovered to 42; Tycho remained 60.
The two humans stayed 55/41. Fully specified combat movement worked
for both: Max 20715 to 21915 consumed six AP, then 22515 consumed
three; Natalia 18723 and 19723 consumed five AP per leg. Earlier
movement probes missing elevation were parser errors, not engine
defects. Enter on Natalia's turn did not end combat; her End Turn
followed by Max's native Enter returned both to exploration. The
precise revision-8/9 dialogue subset is also preserved as
`gizmo96-timeout-chronology.jsonl`.


Max's ordinary First Aid on Ian succeeded for five HP and 25 XP,
bringing Ian to 47. Natalia's native corpse Take All on Izo added
25 caps, bringing hers to 65, and retained his ammunition; she still
carries the Tape Recorder and Nuka-Cola. Native save sequence 5547
preserves this as "Gizmo cleared Ian treated". External backup
`SLOT09-Gizmo-cleared-Ian-treated90-20261002` and the earlier
`SLOT09-Gizmo-confession-combat90-20261002` retain both post-fight
checkpoints alongside the peaceful Tycho backup. The shown aftermath
image is `guest96-gizmo-aftermath.png`.


The pinned build 93 deployment matched all 306 source/config hashes
before testbox's native rebuild. HAL's pinned binary SHA256 is
`a05465454388b5291ec9bf389d6240e344539ff9255a8a8cad6ac890b90dc6b0`;
testbox's native binary is
`1cb1dcdb7d50b34f4022ddafc6ae2cafeff34d0929ee0285c721809fa7c7595d`.
The current treated-Ian save was backed up externally before
restarting host96/guest96 as host97/guest97, preserving mute, prior
checkpoints and SLOT10. The 96 journals/logs were archived before
the restart.


Visible build 93 dialogue idle acceptance passed through Max's
ordinary Ian conversation. Host sequence 84 at 1790931119892 and
guest 30 at 1790931120938 show revision 1 with both ballots 0. After
90.697 seconds measured by the control machine's monotonic clock,
both still showed the same reply, six options and unvoted ballots.
Max then voted option 6. Host 93 at 1790931214173 still showed that
page with Max 6 and Natalia 0. Natalia's matching vote closed the
native conversation; host 107 at 1790931218164 returned to
exploration with no dialogue. Thus an unvoted page waits beyond the
old 60-second deadline and later real votes still resolve.

The restored Izo loot also confirms ordinary currency sharing: Max
has 66 caps and Natalia 65 from prior 41/40, so his 50 caps split
25 to each. The Tape Recorder remains Natalia's, and Ian's saved
47 HP and Tycho's 60 survive the matched checkpoint restoration.


Natalia's ordinary Gizmo corpse Take All gave her a 9mm Mauser
loaded with seven rounds and a 20-round 9mm ball stack. His 100
caps split 50 each, bringing Max/Natalia to 116/115. Max received
three Iguana-on-a-stick items. A read-only installed JUNKCSNO.MAP
inventory check confirms these food items were Gizmo's PID103
quantity3, alongside Money quantity100 and Mauser/ammunition; they
did not come from the guards. Guard loot approaches logged
UNREACHABLE while a companion occupied the front doorway. Moving
Max back into the hall drew Ian away, then native movement took
Max outside. Natalia followed once Tycho also cleared the doorway.
No door or pathfinding defect is claimed from that NPC blockage.


The ordinary casino exit returned the party to JUNKKILL. Shared rest
until morning unlocked Killian's storefront and inner door. Natalia
opened both through native door actions, reached him at 27896, and
received the ordinary floating refusal, "It's late. Come back some
other time." The actual host alarm showed 29 DEC 2161 06:08. Later
rest interaction advanced the visible guest clock to 08:13 and
08:27, but guest world publication stalled, so those pictures do
not establish a clean synchronized two-hour-rest acceptance.

Build 93 then exposed a local-menu background pause. Host snapshot
capture repeatedly reported reason 3 BusyObject, actor entity 1.
Host exploration was confirmed by its actual HUD. The guest was
still inside a Pip-Boy screensaver; its first Escape woke the alarm
and the second closed it. After both native windows were visibly
closed and twelve quiet seconds passed, host sequence 5586 kept
advancing while guest sequence 1630 remained at 1790933662853.
The root's separate actual native UI regression reproduced removal
of map animation background processing by Pip-Boy and inventory
windows. No native flags or animations were forced in this campaign.

One ordinary save still succeeded: host sequence 5702 at
1790934943304 displayed "Game Saved." SLOT09 now reads "Junktown
Gizmo loot morning" and has fresh SAVE.DAT, MULTI.DAT and JUNKKILL.SAV.
External backups `SLOT09-before-stalled93-save-20261002` and
`SLOT09-Junktown-loot-morning93-20261002` preserve both the prior
known-good fight checkpoint and this later loot/town state. The
host97 and guest97 journals and logs were archived before the next
matched deployment. Killian's evidence turn-in and an already-open
alarm HP-change test remain pending.


Pinned build 98 matched all 306 source/config hashes before native
testbox rebuild. HAL binary SHA256 is
`05d7f91e0a7d023aa650ca190916a743946d66423adcbf1696b22b04325242e3`;
testbox native binary is
`b0f011dad0c759b27f5d31f6677ab4db6dc3f1b8e31212e2658c4c466376ae5e`.
Both muted processes restarted as host98/guest98 and restored the
latest Junktown loot/morning SLOT09, including Max/Natalia tiles
28868/27286 and Ian/Tycho party entities 1647/1648.

The first actual menu test passed. Max opened his alarm, and Natalia
ran from 27286 to 26882 while it stayed open. Host sequence 69 to
84 and guest 19 to 24 captured her new tile. The actual host image
`host98-open-during-guest-walk.png` confirms the alarm still displays
08:29 and 55/55 HP. This verifies walking and snapshot publication
continue with the other player's local Pip-Boy open.


The reverse menu test also passed. Natalia's alarm remained open at
08:29 with 41/41 HP while Max ran from 28868 to 28468. Host sequence
160 and guest 29 both captured Max at 28468 and Natalia at 26882.
`guest98-open-during-host-walk.png` preserves the actual open alarm.
Natalia then closed it normally before talking to Killian.

Killian recognized the confession through ordinary dialogue. Revision
1 asked "So, did you get the evidence?" Both chose "I sure did."
Revision 2 asked "Which, bug or tape?" and offered only "The
confession." Revision 3 said "Let's hear it." Both voted its native
More option. Host sequence 264 records that choice at 1790935207493.
The game then stalled with host 265 in dialogue but no active page;
guest 75 retained revision 3 with both ballots 1. The actual head
screen had a blank reply area and the old More ballot text.

One SIGINT to the existing startup GDB printed a stack and resumed
without changing native state. The wait is
`intface_change_fid_animate` through `intface_update_items(true)` and
`op_destroy_object`, inside installed script execution from
`gDialogProcessChoice`. This is a native HUD animation wait during
item destruction, not established audio playback. The session remains
running for review. Logs, journals and the actual head screen are
archived as `host98-killian-stall.log`,
`host98-killian-stall-journal.jsonl`,
`guest98-killian-stall-journal.jsonl`, and
`host98-killian-stall.png`. No subsequent ballot, restart or native
state forcing was used.


Pinned build 101 matched all 306 source/config hashes before native
testbox rebuild. HAL SHA256 is
`6809d901a918bfa408c3e5f3507347a8e1d4763e33ad670e66a636269d1ebcea`;
testbox native SHA256 is
`192cde169368273e168885674eb07f3c2120b38eff24ba9b0193eb6de9a38d5d`.
Muted host99/guest99 restored the pre-turn-in Junktown morning save.
Natalia approached tile 26882 and repeated the same ordinary Killian
choices 2, 1, 1. The previously blocked More now advanced immediately
to revision 4 on both peers, "That's the first time I've been happy
to hear his voice." No debugger interruption or native state changes
were needed. This actual content path accepts the HUD wait fix.

Both chose the offered Stimpaks reward, then agreed to help the guards.
Killian directed the party to Lars. Native exploration resumed on both
peers and displayed 500 XP for securing Gizmo's confession. Shared
activity gained quest 735=1 attributed to Natalia. Her Tape Recorder
was removed and five Stimpaks PID40 appeared in her own inventory;
Max retained his Bug, rifle, three food items and 116 caps. Natalia
retained 115 caps and her Mauser. An ordinary SLOT09 save and external
`SLOT09-Killian-confession-reward101-20261002` preserve the reward.


The user requested a stopping checkpoint before further Lars or
Dogmeat progression. Before that request, Natalia had ordinarily
given Max two of the five reward Stimpaks while adjacent, then both
queued movement toward the guard station. No Lars dialogue or
Dogmeat use-on action was issued. Max reached 22304; Natalia stopped
at 20489, short of the requested 22505. Both remain peacefully in
JUNKENT, with weapons holstered and no combat, dialogue or pending
rest. No pathfinding defect is claimed from this last incomplete
approach; the next session should inspect the actual station doors
and NPC positions before requesting another path.

Native SLOT09 saved "Killian reward peaceful stop" at host sequence
775 at 1790936861351. Afterward host 778 and guest 320 agree on tiles
22304/20489, HP 55/41, no dialogue/rest, and no equipped items. Max
has two Stimpaks and Natalia three. External
`SLOT09-Killian-reward-peaceful-stop101-20261002` preserves this
checkpoint alongside `SLOT09-Killian-confession-reward101-20261002`
and all earlier safe checkpoints. SLOT10 remains untouched. Both
live desktops remain muted with TTS enabled=0 and dummy SDL audio.
Final build 101 journals/logs are archived as host99/guest99-final
files under `/var/tmp/fallout-visible-campaign-20261001/`.

Remaining visible gates at this stopping point:

- Continue the ordinary Lars follow-up after Killian's confession
  reward. Gizmo and his cronies already died in actual combat; use
  whichever installed dialogue acknowledges that existing outcome.
- Recruit Dogmeat through Max's actual Iguana-on-a-stick food, then
  continue Hub/Necropolis exploration toward the water chip. The
  water chip and both mutant threats remain uncompleted content.
- Obtain an ordinary human injury in the next real encounter. Keep
  the injured player's alarm already open while the other player
  uses one of these real healing items. Record actual open-page HP,
  journals and the eventual HUD. Both-direction open Pip-Boy walking
  passed, but full-health menu pictures do not prove HP redraw.
- Later verify native companion dialogue/skill rewards and ordinary
  map transitions as the campaign reaches them. Tycho's polite
  training, free drink, recruitment and leader-follow behavior
  already passed. His hostile insult route was reviewed privately
  and is not a passed visible campaign gate.

No production source or master plan was edited by this agent. This
campaign evidence file is the only repository file changed here.
