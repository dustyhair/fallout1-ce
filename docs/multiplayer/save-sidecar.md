# Multiplayer save sidecar

A multiplayer save keeps Fallout's original slot files intact and adds `MULTI.DAT` beside `SAVE.DAT`. The developer session writes the sidecar only when a two-player session is active. A normal single-player save removes stale multiplayer metadata from that slot after `SAVE.DAT` succeeds.

The current version 2 sidecar contains:

- A monotonically increasing slot generation.
- An FNV-1a digest of the exact `SAVE.DAT` bytes it belongs to. This is a mismatch and corruption check, not a security boundary.
- Canonical host and guest player IDs.
- Each player's printable name and complete `CharacterBuild`: base and bonus stats, skill investments, perk ranks, tagged skills, traits, prototype flags, unspent skill points, level, and experience.
- The guest actor's recursive legacy object record. This includes carried and equipped items, stack quantities, weapon ammunition, item charges, nested containers, current health, and saved map position.
- A payload length and checksum. The decoder rejects unknown versions, oversized or malformed values, duplicate players, truncation, trailing bytes, and checksum failures before changing session state.

Version 1 sidecars remain loadable. They restore both character builds and start the guest with an empty inventory. The next successful multiplayer save upgrades the slot to version 2.

## Save transaction

The legacy save backup remains authoritative. Once the new `SAVE.DAT` closes, the game digests it, encodes both player builds, writes `MULTI.TMP`, and atomically renames it to `MULTI.DAT`. The prior sidecar is held as `MULTI.BAK` during that rename. If metadata capture or publication fails, the save is reported as failed and the existing `SAVE.DAT` backup is restored. The old sidecar remains paired with that restored file.

## Load behavior

With `--multiplayer-dev`, a matching sidecar is decoded after the legacy load completes. The first subsequent session update recreates the temporary guest actor, restores both builds against the registered host and guest IDs, binds presentation to the host, and enters Exploration. Loading during an active developer session follows the same path after the guest actor is rebound.

A missing, corrupt, unsupported, or mismatched sidecar never modifies the original save. The load continues without multiplayer and prints a diagnostic. Without `--multiplayer-dev`, `MULTI.DAT` is ignored.

The guest actor remains `OBJECT_NO_SAVE`; map objects and the original save format are unchanged. The sidecar restores player character state, not a second copy of the world.
