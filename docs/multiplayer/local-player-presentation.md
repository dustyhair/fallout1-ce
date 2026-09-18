# Local-player presentation

Each process can bind one `PlayerId` as its local presentation player. The binding stores the active `LocalSession` and player ID, then resolves the character state and actor through the session registries on every access. It does not cache an `Object*`, so replacing an actor during a map load keeps the HUD and modal screens attached to the same stable entity.

The developer two-player session binds the host as local. Guest commands submitted with the developer modifier still execute in the guest's acting-player context, but they do not switch the host process's camera or UI. A later network guest process will bind its own player ID.

## Presentation boundary

`localPlayerActorOrStoryActor()` preserves the single-player path: it returns the bound local actor when a multiplayer session is active and otherwise returns `obj_dude`. The story actor itself is never changed.

`ScopedLocalPlayerContext` temporarily installs the local player's `CharacterBuild` as the acting-player context. This lets legacy APIs without an actor parameter, such as perk and trait lookups, return values for the character currently shown by the UI. Nested scopes restore the previous acting context when they end.

The current routing covers:

- HUD hit points, armor class, equipped items, attack modes, action-point costs, reloads, and item use.
- Character-sheet stats, skills, perks, traits, derived values, and edit rollback.
- Inventory ownership, combat cost, armor changes, equipment animation, item use, looting, and barter calculations.
- Player-facing weapon range, attack cost, and called-shot rules while the local context is active.

The binding is cleared when its session stops or is destroyed. Headless tests cover invalid selection, host/guest selection, acting-context installation, actor rebinding, and teardown.

## Current limits

The legacy player name is still process-wide, so distinct names belong with temporary lobby character creation. Camera and general mouse input remain on the Phase 2 guest-presentation path. Deferred callbacks that read character globals must carry player identity and install a context; this slice covers the synchronous HUD and modal UI paths.
