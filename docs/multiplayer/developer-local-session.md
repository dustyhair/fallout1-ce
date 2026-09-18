# Developer local session

The Phase 0 developer session proves that the engine can keep two player-owned actors on one map without changing the normal single-player path.

Launch it with an installed Fallout data set:

```sh
./build/fallout-ce --multiplayer-dev
```

Start a new game and finish the host character first. When the first map opens, the developer session creates a second actor and opens the character editor for the guest. Accepting that character submits both sheets to the temporary host lobby. The map becomes playable only after both pass validation.

Cancelling the guest editor stops the multiplayer session and continues in single-player mode. A new developer session still begins from a new level-one host. After that session is saved, loading the slot with the flag restores both progressed characters from `MULTI.DAT`. Older saves without matching multiplayer metadata print a diagnostic and continue without multiplayer.

The guest actor uses the player prototype and belongs to the guest player in the multiplayer entity registry. The session also creates in-process loopback endpoints for later transport work.

In exploration mode, a normal map click controls the host actor. Hold either Ctrl key while clicking to control the guest actor. This works for movement and usable doors. Shift still toggles walking and running. The command processor rejects these commands during combat. Movement also fails while that actor is busy.

The guest actor is temporary and has `OBJECT_NO_SAVE`; original map and save formats remain unchanged. Before a map load, the engine removes the temporary object. After the new map loads, it creates a replacement and rebinds the existing guest `EntityId` to it. Multiplayer slots store both names and complete character builds in the versioned sidecar described in [save-sidecar.md](save-sidecar.md).

The flag is intentionally separate from `fallout.cfg`, so running the experiment does not persist a multiplayer setting. Other guest actions still have no input route.
