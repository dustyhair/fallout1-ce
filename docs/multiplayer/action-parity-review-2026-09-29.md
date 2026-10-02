# Single-player and multiplayer action review

The [September 30 follow-up](action-parity-followup-2026-09-30.md) repairs
several gaps listed here and updates the current protocol versions.

Reviewed branch: `multiplayer-plan`, starting at `a4038eb`.
Source checkout: `/home/jwagner/Development/side_projects/fallout1-ce`.
The installed TTS game and the separate TTS source checkout were not modified.

The comparison uses the native single-player paths in the same engine as the
reference. Multiplayer should execute those rules once on the host, with the
acting player's build selected, then apply the results on the guest. Guest
animation and UI code must not run gameplay scripts or roll damage again.

## Changes

| Action | Divergence found | Change |
| --- | --- | --- |
| HUD reload during exploration | Guest calls `item_w_try_reload` locally, changing ammo before host approval. | Submit an inventory command and apply the host checkpoint. Combat HUD reload keeps its native AP command. |
| HUD direct item use | Exploration use calls native item rules on the guest. | Route self-use through the host. World-target item use retains its separate animated command. |
| Inventory use in combat | Inventory use takes the HUD path and charges an extra two AP after opening inventory. | Require a host-recorded open inventory for the current turn. Native inventory drug use has no additional AP charge. |
| Inventory unloading | Network inventory reports unloading unavailable. | Host unloads one weapon, registers generated ammo, and publishes a complete inventory checkpoint. |
| Ammo dragging | Dropping ammo on an occupied weapon slot swaps equipment; dropping on a list weapon never reloads it. | Recognize compatible ammo before equipment swaps. Keep the native quantity picker and validate ammo ownership and quantity on the host. |
| Stack editing | Editing a loaded weapon can affect a representative of several weapons, and detaching it clears equipped flags. | Detach one weapon with the native split operation and restore its equipped slot before returning it. |
| Dropping stacks | Network context menus discard the native quantity selection. | Keep the quantity picker. Caps use their quantity command; ordinary items use the existing host-confirmed sequence of individual ground objects. |
| Combat perks and traits | Several calculations gate player rules on `obj_dude`, excluding the guest. | Include registered player attackers for Slayer, Silent Death, Sniper, Sharpshooter, One Hander, Finesse, ranged damage bonuses, and lighting penalties. Sniper reads the attacker's luck. |
| Incoming critical hits | The guest uses NPC critical-hit tables. | Registered player defenders use the native player critical-hit table. |
| Stats and skills of another player | A target outside the current acting context falls back to the shared critter prototype. | Explicit actor scopes select the target's own stats, bonuses, derived stats, traits, and skill points, then restore the caller's context. |
| Deferred skill/item/pickup callbacks | The command's context expires before animation callbacks execute. | Select the source player's context again in native callback entry points. |
| First Aid and Doctor | Guest use skips the native elapsed-time rules and some healthy-target bookkeeping. | Apply player rules to the acting guest as well as the story player. |
| Chem Resistant | Guest drug duration skips its trait. | Select the drug recipient's context and apply the native duration adjustment to either player. |
| Camera and facing shortcuts | Home and comma/period manipulate the story actor directly. | Home follows the local actor. Facing keys submit the existing host-authoritative facing command. |
| Exploration smoke setup | Older raw-transport fixtures omit the initial host checkpoint. Starting kits therefore have different IDs. The previous build also fails the transfer test. | Exchange and apply the host's initial state before preparing those fixtures. |

`InventoryActionCommand` validates action type, item and ammo identities,
quantity, phase, ownership, and current combat turn. The agent journal now
records native inventory indices so UI tests select the displayed row even
after host stack splits reorder the inventory. Replayed commands reuse the
recorded result. Its result uses the existing inventory checkpoint boundary and
recovery handling. The guest never executes inventory item rules from that
result event.

Gameplay wire version is **32**. Host and guest must use matching builds.

## Other action paths inspected

Movement and facing already submit semantic commands. Combat movement uses the
native path and AP machinery. Pickup reserves ground items and publishes a
completion event. Loot and direct trade already validate range and ownership;
party caps and corpse-loot priority intentionally differ from solo play.
Targeted skills and world-target item use already execute on the host. This
review repairs their player context at deferred native callbacks.

Aimed attacks use the native called-shot selection and host combat rules.
Equipment, inventory opening, and hand selection already have host commands.
Dialogue has shared voting and uses the selected talker's build. Stairs, ladders,
elevators, exit grids, world-map travel, and rest use party transitions or
approval where needed. These cooperative rules are intentional differences.
Save/reconnect uses a host checkpoint and multiplayer sidecar rather than a
second independent game simulation.

## Remaining gaps

This review does not establish complete single-player parity.

- Owned-container navigation and transfers are still unavailable from the
  network inventory. The legacy container UI changes its inventory actor and
  directly edits ownership; it needs a separate command and refreshed view.
- Sneak shortcuts still use the legacy local toggle. Sneak success has one
  global variable, and its timed check targets `obj_dude`. It needs an
  authoritative per-player toggle and timed state before guest sneaking is
  reliable.
- First Aid/Doctor daily usage slots remain one global table. They need
  per-player persisted history for independent limits.
- Drug addiction/withdrawal code still contains story-player checks and shared
  addiction state. Immediate recipient stats and Chem Resistant are corrected;
  a complete addiction and withdrawal parity pass remains necessary.
- NPC movement, projectile/sound sequences, damage reactions, and deaths still
  rely heavily on checkpoints. Player attack art is presentation-only, but a
  complete native combat presentation sequence needs explicit host events.
- NPC barter and several modal flows remain intentionally blocked until their
  mutations have host commands. Explosive timer dialogs also need a peer-facing
  choice flow.
- Character advancement and script-specific story-player assumptions require
  further per-player review. The checks here are not an exhaustive audit of
  every installed quest script.

## Validation

- The rebuilt engine and headless tests compile successfully.
- CTest passes all 3 tests, including multiplayer command/protocol regression checks.
- Python tool tests pass all 17 tests.
- Native inventory UI checks pass for both host and guest: knife equip/unequip,
  drug use, dropping two items from a stack, weapon unloading, and dragging
  selected ammo onto a weapon. Knife combat then completes with matching state
  digest `9203250779729831459`. The guest runs zero scripts, damage attacks, and
  gameplay RNG calls. Screenshots and logs are under
  `/tmp/fallout-native-inventory-mxdrv1ux`.
- The new inventory action fixture conserves all 60 rounds for each player,
  preserves equipment, consumes one drug from a stack, and keeps host inventory
  combat AP at 6 after the native 4 AP opening charge. The host verifies that
  guest Endurance is 8 and maximum HP is 57 while the host context is active.
  Guest One Hander raises pistol accuracy from 21 to 41. Final host/guest state
  digest is `5484268495670539799`.
- All 47 live compatibility scenarios pass, plus the headless campaign gate.
  The first run passed scenarios 1 through 30. Scenario 31 could not bind TCP
  port 46530 before a game started. Scenarios 31 through 47 were rerun on
  different ports and all pass. Logs are under
  `/tmp/fallout-parity-campaign-final-20260929` and
  `/tmp/fallout-parity-campaign-remaining-20260929`. The latter directory's
  `combined-summary.tsv` records every case's successful result. This is a
  combined result from the two runs, rather than one uninterrupted campaign.

Commands:

```sh
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
PYTHONPATH=tools python3 -m unittest discover -s tools/tests
FALLOUT_CAMPAIGN_FULL=1 bash tools/run_multiplayer_compatibility_campaign.sh build/fallout-ce /path/to/lowercase-game-data /path/to/results
python3 tools/test_native_multiplayer_inventory.py build/fallout-ce /path/to/game-data --context-actions
```

The inventory action campaign checks both players unloading/reloading with
native and selected ammo, round conservation, drug stack consumption, equipped
slot preservation, combat inventory AP, guest stat lookup while the host is the
acting player, and the guest's One Hander accuracy bonus. Protocol tests cover
malformed payloads, replay, ownership, and phase/turn requirements.
