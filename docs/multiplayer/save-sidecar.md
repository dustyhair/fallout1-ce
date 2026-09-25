# Multiplayer save sidecar

A multiplayer save keeps Fallout's original slot files intact and adds `MULTI.DAT` beside `SAVE.DAT`. The developer session writes the sidecar only when a two-player session is active. A normal single-player save removes stale multiplayer metadata from that slot after `SAVE.DAT` succeeds.

The current version 3 sidecar contains:

- A monotonically increasing slot generation.
- An FNV-1a digest of the exact `SAVE.DAT` bytes it belongs to. This is a mismatch and corruption check, not a security boundary.
- A bounded roster of two to eight records in increasing `PlayerId` order, starting with the story actor. IDs are stored explicitly, so gaps do not reassign a player to a different slot. The live session still accepts exactly the host and guest IDs; a different valid roster is rejected at load until the engine can restore every actor.
- Each player's ID, printable name, complete `CharacterBuild`, and optional recursive object record. The host's object remains in `SAVE.DAT` and has no sidecar object record. The guest record includes carried and equipped items, stack quantities, weapon ammunition, item charges, nested containers, current health, and saved map position.
- A payload length and checksum. The decoder rejects unknown versions, oversized or malformed values, duplicate players, truncation, trailing bytes, and checksum failures before changing session state.

Version 1 and 2 sidecars remain loadable. Version 1 restores both character builds and starts the guest with an empty inventory. Version 2's trailing guest object is migrated into the guest's player record. The next successful multiplayer save upgrades either format to version 3.

## Save transaction

The legacy save backup remains authoritative. Once the new `SAVE.DAT` closes, the game digests it, encodes both player builds, writes `MULTI.TMP`, and atomically renames it to `MULTI.DAT`. The prior sidecar is held as `MULTI.BAK` during that rename. If metadata capture or publication fails, the save is reported as failed and the existing `SAVE.DAT` backup is restored. The old sidecar remains paired with that restored file.

## Load behavior

With `--multiplayer-dev`, a matching sidecar is decoded after the legacy load completes. The first subsequent session update recreates the temporary guest actor, restores both builds against the registered host and guest IDs, binds presentation to the host, and enters Exploration. Loading during an active developer session follows the same path after the guest actor is rebound.

A missing, corrupt, unsupported, or mismatched sidecar never modifies the original save. The load continues without multiplayer and prints a diagnostic. Without `--multiplayer-dev`, `MULTI.DAT` is ignored.

The guest actor remains `OBJECT_NO_SAVE`; map objects and the original save format are unchanged. The sidecar restores player character state, not a second copy of the world.
