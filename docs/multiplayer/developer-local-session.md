# Developer local session

The Phase 0 developer session proves that the engine can keep two player-owned actors on one map without changing the normal single-player path.

Launch it with an installed Fallout data set:

```sh
./build/fallout-ce --multiplayer-dev
```

Starting or loading a game creates a second actor beside the player. The actor uses the player prototype, belongs to the guest player in the multiplayer entity registry, and communicates with the host through the in-process loopback transport.

The guest actor is temporary and has `OBJECT_NO_SAVE`; original map and save formats remain unchanged. Before a map load, the engine removes the temporary object. After the new map loads, it creates a replacement and rebinds the existing guest `EntityId` to it.

This mode does not provide guest input yet. Movement and the first interaction will be routed through authoritative commands in the next implementation slice. The flag is intentionally separate from `fallout.cfg`, so running the experiment does not persist a multiplayer setting.
