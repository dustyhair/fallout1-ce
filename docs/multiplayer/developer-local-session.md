# Developer local session

The Phase 0 developer session proves that the engine can keep two player-owned actors on one map without changing the normal single-player path.

Launch it with an installed Fallout data set:

```sh
./build/fallout-ce --multiplayer-dev
```

Starting or loading a game creates a second actor beside the player. The actor uses the player prototype and belongs to the guest player in the multiplayer entity registry. The session also creates in-process loopback endpoints for later transport work.

In exploration mode, a normal map click controls the host actor. Hold either Ctrl key while clicking to control the guest actor. This works for movement and usable doors. Shift still toggles walking and running. The command processor rejects these commands during combat. Movement also fails while that actor is busy.

The guest actor is temporary and has `OBJECT_NO_SAVE`; original map and save formats remain unchanged. Before a map load, the engine removes the temporary object. After the new map loads, it creates a replacement and rebinds the existing guest `EntityId` to it.

The flag is intentionally separate from `fallout.cfg`, so running the experiment does not persist a multiplayer setting. Other guest actions still have no input route.
