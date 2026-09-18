# Developer local session

The Phase 0 developer session proves that the engine can keep two player-owned actors on one map without changing the normal single-player path.

Launch it with an installed Fallout data set:

```sh
./build/fallout-ce --multiplayer-dev
```

Start a new game and finish the host character first. When the first map opens, the developer session creates a second actor and opens the character editor for the guest. Accepting that character submits both sheets to the temporary host lobby. The map becomes playable only after both pass validation.

Cancelling the guest editor stops the multiplayer session and continues in single-player mode. A new developer session still begins from a new level-one host. After that session is saved, loading the slot with the flag restores both progressed characters from `MULTI.DAT`. Older saves without matching multiplayer metadata print a diagnostic and continue without multiplayer.

The guest actor uses the player prototype and belongs to the guest player in the multiplayer entity registry. The session also creates in-process loopback endpoints for later transport work.

In exploration mode, a normal map click controls the host actor. Hold either Ctrl key while clicking to control the guest actor. This works for movement, usable doors, ground-item pickup, and looting a critter. Shift still toggles walking and running. The command processor rejects these commands during combat. Movement also fails while that actor is busy.

Press `I` or use the inventory button to open the host inventory. Press `Ctrl+I`, or hold Ctrl while using the inventory button, to open the guest inventory. Closing the guest inventory restores the host HUD and normal inventory selection.

Guest inventory objects move to the replacement guest actor during a map change. Version 2 of `MULTI.DAT` also stores the recursive guest object record, so carried and equipped items survive save and load. Version 1 slots still load with an empty guest inventory and upgrade on their next save.

The guest actor is temporary and has `OBJECT_NO_SAVE`; original map and save formats remain unchanged. Before a map load, the engine removes the temporary object. After the new map loads, it creates a replacement and rebinds the existing guest `EntityId` to it. Multiplayer slots store both names and complete character builds in the versioned sidecar described in [save-sidecar.md](save-sidecar.md).

The flag is intentionally separate from `fallout.cfg`, so running the experiment does not persist a multiplayer setting. Talking, combat, skills, and using inventory items on world targets still have no guest input route.
